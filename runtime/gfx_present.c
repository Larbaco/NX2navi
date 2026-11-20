// Vulkan surface and swapchain, present blit, 60 Hz vsync thread, focus: the window-system-free half of the window
// (the other half: gfx_window_win.c with Win32, gfx_window_sdl.c with SDL, through gfx_wsys.h).
// Contains code Copyright (c) 2026 Peiikai, Copyright (c) 2026 MacNativeGames contributors (MIT; see
// THIRD_PARTY_NOTICES.md).
//
// Threads: the window thread (the back end's) owns the window and never runs guest code. The vsync thread signals the
// bound system events. Presents come from any host thread (the back end's present worker) and take g_pl; every queue
// submission takes the recursive queue lock (vk_queue_lock). The swapchain is rebuilt lazily: a resize only marks it
// dirty (gfx_window_mark_dirty), the next present (or gfx_window_resize) rebuilds it.
//
// The in-game menu (ui/menu.h) draws over the game image in the present's command buffer, after the blit. The window's
// size and fullscreen state are the settings NX2SARIA_WINDOW_SIZE and NX2SARIA_FULLSCREEN, which the back end applies at start
// and whenever a cfg_set changes them (window_settings_live -> gfx_window_apply_settings).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audren.h"
#include "cfg.h"
#include "flight.h"
#include "flight_flash.h"
#include "fs.h"
#include "gfx.h"
#include "gfx_guard.h"
#include "gfx_wsys.h"
#include "input.h"
#include "logq.h"
#include "nvn_api.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "vk.h"
#include "vk_fg.h"
#include "vk_fsr3fg.h"
#include "vk_dlaa.h"
#include "vk_dlssg.h"
#include "vk_lowlat.h"
#include "heap_retry.h"

#define FRAMES 2
#define MAX_IMAGES 16
#define NG_MAX 5   // generated frames per rendered one at most (DLSS Frame Generation through NGX, 6x)
#define VSYNC_NS 1000000000ull
#define VSYNC_HZ 60

// ---- queue lock
// Taking it hands the replay's held submissions to the queue first (vk_sync.c, submit batching), so whatever the
// holder submits or waits for comes after them in queue order.
// NX2SARIA_LOCK_TIME also names who holds it: a hold of QL_HOLD_NS or more goes into the frame-time summary under the
// site that took the lock (perf.c perf_qlock_hold), beside the waits for it. A present had waited 48 ms for the lock
// with no holder named anywhere. The taking functions are not inlined, so that their return address is the caller's
// site, which is also the site the waits are reported under (plat_rmutex_lock_at). ql_site and ql_t0 are written by
// the holder only, at the outermost take and release.
#define QL_HOLD_NS 5000000ull
static PlatRMutex g_ql;
static void *ql_site;
static uint64_t ql_t0;
static void ql_taken(void *site) {
    if (g_ql.depth == 1 && perf_lock_holds_on()) ql_t0 = plat_now_ns(), ql_site = site;
}
__attribute__((noinline)) void vk_queue_lock(void) {
    void *site = __builtin_return_address(0);
    plat_rmutex_lock_at(&g_ql, site);
    ql_taken(site);
    vk_sync_flush_held();
}
__attribute__((noinline)) void vk_queue_lock_raw(void) {
    void *site = __builtin_return_address(0);
    plat_rmutex_lock_at(&g_ql, site);
    ql_taken(site);
}
__attribute__((noinline)) int vk_queue_trylock_raw(void) {
    if (!plat_rmutex_trylock(&g_ql)) return 0;
    ql_taken(__builtin_return_address(0));
    return 1;
}
void vk_queue_unlock(void) {
    if (g_ql.depth == 1 && ql_t0) {
        uint64_t d = plat_now_ns() - ql_t0;
        ql_t0 = 0;
        if (d >= QL_HOLD_NS) perf_qlock_hold(ql_site, d);
    }
    plat_rmutex_unlock(&g_ql);
}

// ---- window state shared with the back end
static VkSurfaceKHR g_surface;
static volatile int32_t g_dirty;               // the window was resized; swapchain stale
static volatile int32_t g_restart;             // gfx_window_request_restart: closing the window starts the game again
static volatile int32_t g_focused = 1;
static volatile int32_t g_focus_msgs;
static PlatRMutex g_start_lock;       // gfx_window_start / vsync start
static int g_started;

void gfx_window_mark_dirty(void) { plat_xchg32(&g_dirty, 1); }

// The window gained or lost the focus: the game hears it as nn::oe's focus state and a notification message. The line
// carries the local wall time, the only one in the log: when a GPU hang takes the whole system down, the log ends
// without an error, and this time set against the driver's entry in the Windows event log (nvlddmkm 153, Display 4101)
// tells whether an alt-tab or an overlay came before the hang or followed it.
void gfx_window_focus_changed(int in) {
    plat_xchg32(&g_focused, in);
    oe_set_focus(in);                          // pinned to 1 by NX2SARIA_ASSUME_FOCUS inside
    oe_push_message(OE_MSG_FOCUS_STATE_CHANGED);
    plat_inc32(&g_focus_msgs);
    struct tm t;
    plat_local_time(&t);
    rt_log("gfx: window %s the focus at %04d-%02d-%02d %02d:%02d:%02d local time", in ? "gained" : "lost",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    if (!in) {   // the session's reports and the log now, should the process be ended from outside next
        perf_focus_lost();     // perf.c
        rt_log_focus_lost();   // main.c
    }
}

// ---- the orderly end: the window's close (gfx_window_closed), the guest's ExitProcess (svc.c) and the end of a
// --seconds run (boot.c rt_frame_presented)
// Once per process, whichever comes first. The window's close and the console's (main.c console_closed) can come
// together, or the guest's ExitProcess beside either: each ran its own sequence, so two pipeline saves and two
// frame-time summaries were written while both raced. 1 for the first caller; a caller on another thread waits here
// for good while the first ends the process; 0 for the first caller's own thread coming back, which goes on with the
// sequence it is in. The window's close runs on a thread of its own (gfx_window_win.c exit_start); the window thread
// runs the sequence itself, and may handle a message inside it, only when that thread could not be made.
static volatile uint32_t exit_tid;   // the thread that runs the orderly end, 0 before one started

int rt_exit_begin(void) {
    uint32_t me = plat_thread_id(), was = 0;
    if (__atomic_compare_exchange_n(&exit_tid, &was, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        rt_log_ending();   // the end's lines reach the log file past its cap (main.c log_sink)
        return 1;
    }
    if (was == me) return 0;
    for (;;) plat_sleep_ms(1000);
}

int rt_exit_started(void) { return __atomic_load_n(&exit_tid, __ATOMIC_ACQUIRE) != 0; }

// What every orderly end saves and puts back, in this order. What a kill of the process must not lose and what is quick
// comes first: Windows ends a closed console about 5 s after the close, a session's end and an "End task" sooner, and
// the pipeline save alone can take that long (vk_pipeline_save_exit waits up to 5 s for the builds that run). So a
// Commit in flight is finished, RivaTuner Statistics Server's profile and the controller's rumble are put back, the
// settings, the boot prebuild's marker, the frame-time summary and the memory ranking are written, and the log reaches
// the disk before the pipeline save starts. The final save flush comes last: the game's threads run until plat_exit,
// and a Commit the game starts during the pipeline save is still taken and waited for. RT_EXIT_SETTINGS: nx2navi.ini
// gets the menu's changes (not in a scripted --seconds run, whose end is no player's).
void rt_exit_steps(unsigned what) {
    fs_save_wait();   // a Commit already under way: finished, nothing refused
    // the game's frame limit out of the profile; rtss_limit.c keeps it out while frames go on during the pipeline save
    rtss_limit_restore();
    input_rumble_hd_stop();   // a controller on HD rumble gets the neutral value: nothing renews or stops it after the exit
    if (what & RT_EXIT_SETTINGS) cfg_save();   // a menu change saves 400 ms later on the render thread (option_rows.cpp)
    // The game ended while the boot still built its pipelines: not the driver crash the marker is there to catch. Left
    // in place, the next start took it for one and every later start skipped the prebuild (vk_pipeline.c pm_skipped).
    vk_pipeline_prewarm_marker_clear();
    prof_write();   // "frametime: exit" and the stutter map; plat_exit runs no atexit handlers
    vk_mem_exit_log();   // the memory report's ranking of the session
    heap_retry_exit_log();   // the model heaps' retries of the session
    logq_flush();   // the lines so far on the disk before the slow part; the queue stays on
    vk_pipeline_save_exit();
    fs_save_flush();   // last: a Commit started since fs_save_wait finishes, and one after this is refused
}

// The window was closed: the process ends with it, once a save Commit still running in the background is
// done. After gfx_window_request_restart the executable then starts again with the same command line and environment
// (plat_restart), and the new process waits until this one is gone (main.c) before it moves the log and opens the
// RomFS and the saves.
void gfx_window_closed(void) {
    if (!rt_exit_begin()) return;
    int restart = plat_cas32(&g_restart, 0, 0) != 0;
    rt_log("gfx: window closed; %s", restart ? "restarting" : "exiting");
    rt_exit_steps(RT_EXIT_SETTINGS);
    if (restart) {
        cfg_setenv("NX2SARIA_REEXEC", NULL);   // the new process counts its own address-space retries (memory.c)
        if (!plat_restart()) rt_log("gfx: the restart could not start the game again");
    }
    plat_exit(0);
}

void gfx_window_request_restart(void) {
    plat_xchg32(&g_restart, 1);
    gfx_window_request_close();
}

// NX2SARIA_FULLSCREEN=1: borderless window over the monitor. NX2SARIA_WINDOW_SIZE=<w>x<h>: client area of the window (the
// swapchain follows the window; the game image is scaled to it).
int gfx_cfg_fullscreen(void) { return cfg_bool("NX2SARIA_FULLSCREEN"); }

void gfx_cfg_window_size(int *w, int *h) {
    int x = 0, y = 0;
    if (sscanf(cfg_value("NX2SARIA_WINDOW_SIZE"), "%dx%d", &x, &y) != 2 || x < 640 || y < 360 || x > 7680 || y > 4320) {
        const CfgMeta *m = cfg_meta("WINDOW_SIZE");   // unparsable or out of range: the default
        if (!m || sscanf(m->def, "%dx%d", &x, &y) != 2) x = 1920, y = 1080;
    }
    *w = x;
    *h = y;
}

// The refresh rate of the window's monitor (NX2SARIA_FPS_LIMIT=display), measured by the back end's window thread when the
// window opens, lands on another monitor or the display mode changes; readers take the stored value.
static volatile int32_t g_rate_mhz;   // millihertz, 0 unknown

void gfx_window_set_refresh(double hz) {
    int32_t mhz = (int32_t)(hz * 1000 + 0.5);
    if (plat_xchg32(&g_rate_mhz, mhz) != mhz) rt_log("gfx: monitor refresh rate %.3f Hz", hz);
}

double gfx_window_refresh_hz(void) { return plat_cas32(&g_rate_mhz, 0, 0) / 1000.0; }

// ---- swapchain and present state (g_pl)
static PlatRMutex g_pl;
static VkSwapchainKHR g_sc;
static VkFormat g_sc_fmt;
static int g_sc_hdr;                        // HDR10 (NX2SARIA_HDR, vk_hdr.c): PQ code values in Rec. 2020
static VkExtent2D g_sc_ext;
static int g_scaler;                        // the last present: 0 the game image 1:1, 1 scaled by the blit, 2 by FSR,
                                            // 3 shrunk by the blit and sharpened by RCAS
static uint32_t g_sc_n;
static VkImage g_sc_img[MAX_IMAGES];
static VkImageView g_sc_view[MAX_IMAGES];   // colour attachment views for the menu overlay
static VkSemaphore g_render_done[MAX_IMAGES];
static VkCommandPool g_pool;
static VkCommandBuffer g_cb[FRAMES];

// The GPU timeline (NX2SARIA_TRACE=perf, vk.h vk_gtl_note): a timestamp pair per present slot around the present's command
// buffer, read once the slot's fence is waited for again. Its gap before it shows a wait for the acquired image (the
// submission waits on the acquire semaphore) as idle time with work queued before presents.
static VkQueryPool pts_pool;
static int pts_state;                       // 0 not set up, 1 on, -1 off
static uint8_t pts_on[FRAMES];              // the slot's pair was written by its last present
static uint64_t pts_sub[FRAMES];            // when that present went to the queue

static void pts_begin(VkCtx *c, VkCommandBuffer cb, unsigned slot) {
    pts_on[slot] = 0;
    if (!(rt_trace & RT_TRACE_PERF) || pts_state < 0) return;
    if (!pts_state) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = 2 * FRAMES};
        pts_state = c->props.limits.timestampComputeAndGraphics &&
                    vkCreateQueryPool(c->device, &qi, NULL, &pts_pool) == VK_SUCCESS ? 1 : -1;
        if (pts_state < 0) return;
    }
    vkCmdResetQueryPool(cb, pts_pool, 2 * slot, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pts_pool, 2 * slot);
    pts_on[slot] = 1;
}

static void pts_end(VkCommandBuffer cb, unsigned slot) {
    if (pts_on[slot]) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pts_pool, 2 * slot + 1);
}

static void pts_read(VkCtx *c, unsigned slot) {   // the slot's fence was waited for: its last present is done
    if (!pts_on[slot]) return;
    pts_on[slot] = 0;
    uint64_t t[2];
    if (vkGetQueryPoolResults(c->device, pts_pool, 2 * slot, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        vk_gtl_note(GTL_PRESENT, pts_sub[slot], t[0], t[1]);
}
static VkFence g_fence[FRAMES];
static VkSemaphore g_acquire[FRAMES];
static unsigned g_frame;
static int g_capture;
static VkBuffer g_cap_buf;
static VmaAllocation g_cap_alloc;
static uint8_t *g_cap_ptr;
static VkDeviceSize g_cap_size;
static int g_cap_w, g_cap_h, g_cap_valid;
static int g_flip_on;            // vk_present_blit submitted the blit of swapchain image g_flip_idx; vk_present_flip presents it
static uint32_t g_flip_idx;
static uint64_t g_flip_frame;    // ... of this game frame, when a driver's low latency follows it (vk_lowlat.c); else 0

// ---- NX2SARIA_FLIGHT thumbnails (flight.c; g_pl): each present also blits the swapchain image down into slot frame % g_th_n
// of a host-visible ring. A slot's tag names the frame it holds complete; recording a new frame clears the tag first,
// and the tag is set once the fence of that present has signalled.
#define TH_BYTES ((VkDeviceSize)FLIGHT_THUMB_W * FLIGHT_THUMB_H * 4)
static int g_th_n, g_th_failed;
static uint64_t g_th_frame;                // frame of the next vk_present_image call (vk_thumbs_frame)
static uint64_t *g_th_tag;                 // per ring slot
static uint64_t g_th_pending[FRAMES];      // per fence slot: the frame whose thumbnail its submission writes
static VkImage g_th_img;
static VmaAllocation g_th_img_alloc;
static VkFormat g_th_fmt;
static VkBuffer g_th_buf;
static VmaAllocation g_th_alloc;
static uint8_t *g_th_ptr;

// ---- vsync
static PlatMutex g_vs_lock = PLAT_MUTEX_INIT;
static uint64_t g_vs_ev[16];
static int g_vs_nev;
static volatile int32_t *g_vs_ctr[4];
static int g_vs_nctr;
static volatile int64_t g_ticks;
static volatile int64_t g_vs_start_ns;
static int g_vs_started;

static void vsync_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_HIGHEST);
    uint64_t n = 0;
    for (;;) {
        uint64_t start = (uint64_t)g_vs_start_ns;
        uint64_t due = start + (n + 1) * VSYNC_NS / VSYNC_HZ;
        uint64_t now = kern_now_ns();
        if (now > due + 100000000ull) {                  // the process stalled: restart the schedule, no catch-up burst
            g_vs_start_ns = (int64_t)(now - (n + 1) * VSYNC_NS / VSYNC_HZ);
        } else if (due > now) {
            plat_wait_ns(due - now);
        }
        n++;
        plat_xchg64(&g_ticks, (int64_t)n);
        uint64_t ev[16];
        volatile int32_t *ctr[4];
        int ne, nc;
        plat_mutex_lock_shared(&g_vs_lock);
        ne = g_vs_nev;
        nc = g_vs_nctr;
        memcpy(ev, g_vs_ev, sizeof ev);
        memcpy(ctr, g_vs_ctr, sizeof ctr);
        plat_mutex_unlock_shared(&g_vs_lock);
        for (int i = 0; i < nc; i++) plat_inc32(ctr[i]);
        for (int i = 0; i < ne; i++) kern_signal_system_event(ev[i]);
    }
    return;
}

static void vsync_ensure(void) {
    plat_rmutex_lock(&g_start_lock);
    if (!g_vs_started) {
        g_vs_started = 1;
        g_vs_start_ns = (int64_t)kern_now_ns();
        if (!plat_thread_spawn(vsync_main, NULL, "vsync", 0)) rt_fatal("gfx: cannot start the vsync thread");
    }
    plat_rmutex_unlock(&g_start_lock);
}

// Lists a vsync event the vsync thread signals. With the list full, an event the game destroyed since
// (kern_system_event_live 0: its state byte is 0 or its memory is gone) gives its slot, which also stops the vsync
// thread signalling it; the list ends the process only when all 16 are live. Each new listing is logged, so a long
// session's log shows how often the game asks for a vsync event at a new address.
void gfx_vsync_bind(uint64_t system_event) {
    plat_mutex_lock(&g_vs_lock);
    int have = 0;
    for (int i = 0; i < g_vs_nev; i++) have |= g_vs_ev[i] == system_event;
    int slot = g_vs_nev;
    if (!have) {
        if (slot == 16) {
            slot = 0;
            while (slot < 16 && kern_system_event_live(g_vs_ev[slot])) slot++;
            if (slot == 16) rt_fatal("gfx: more than 16 live vsync events");
        } else {
            g_vs_nev++;
        }
        g_vs_ev[slot] = system_event;
    }
    int listed = g_vs_nev;
    plat_mutex_unlock(&g_vs_lock);
    if (!have)
        rt_log("gfx: vsync event %#llx bound in slot %d, %d listed", (unsigned long long)system_event, slot, listed);
    vsync_ensure();
}

void gfx_vsync_bind_counter(volatile int32_t *counter) {
    plat_mutex_lock(&g_vs_lock);
    if (g_vs_nctr == 4) rt_fatal("gfx: more than 4 vsync counters");
    g_vs_ctr[g_vs_nctr++] = counter;
    plat_mutex_unlock(&g_vs_lock);
    vsync_ensure();
}

uint64_t gfx_vsync_ticks(void) { return (uint64_t)plat_cas64(&g_ticks, 0, 0); }
uint64_t gfx_vsync_time_ns(uint64_t tick) { return (uint64_t)g_vs_start_ns + tick * VSYNC_NS / VSYNC_HZ; }

// ---- focus
int gfx_window_focused(void) { return g_focused; }
unsigned gfx_window_focus_messages(void) { return (unsigned)g_focus_msgs; }

// Present path (g_pl held): a settings change (cfg_version) reaches the window thread, which applies a changed size or
// fullscreen; a changed NX2SARIA_VK_PRESENT or NX2SARIA_HDR rebuilds the swapchain at this present.
static int g_sc_hdr_wanted;   // vk_hdr_wanted when the swapchain was made (create_swapchain)
static int g_sc_ll_wanted;    // vk_lowlat_swapchain_wanted then
static void window_settings_live(void) {
    static unsigned seen;
    static int started;
    static char present[16];   // the NX2SARIA_VK_PRESENT the swapchain was made with
    unsigned v = cfg_version();
    // Frame generation that failed (vk_fg.cpp fail makes vk_fg_wanted 0 with no setting changed): its swapchain and
    // both contexts go at the next present, not when some other setting moves. Only this way round at every present: a
    // frame generation swapchain wanted and not made is tried again at a settings change, not at every frame.
    if (started && vk_fg_active() && !vk_fg_wanted()) plat_xchg32(&g_dirty, 1);
    if (started && v == seen) return;
    seen = v;
    res_fov_live();
    if (started && vk_fg_wanted() != vk_fg_active()) plat_xchg32(&g_dirty, 1);   // frame generation switched: its own swapchain
    const char *e = cfg_value("NX2SARIA_VK_PRESENT");
    if (!started || strncmp(e, present, sizeof present - 1)) {
        snprintf(present, sizeof present, "%s", e);
        if (started) plat_xchg32(&g_dirty, 1);
    }
    // HDR switched in the menu: a swapchain of the other format. Compared with what the swapchain was made for, not
    // with what it got: on a display without HDR10 the setting stays on and the swapchain SDR, and that is settled.
    if (started && vk_hdr_wanted() != g_sc_hdr_wanted) plat_xchg32(&g_dirty, 1);
    // NVIDIA's low latency switched in the menu (vk_lowlat.c): its mode is asked for when the swapchain is made
    if (started && vk_lowlat_swapchain_wanted() != g_sc_ll_wanted) plat_xchg32(&g_dirty, 1);
    ov_operation_mode_live();
    audio_channels_live();
    if (started) gfx_window_apply_settings();
    started = 1;
}

// ---- swapchain
static const char *g_present_got = "";   // the NX2SARIA_VK_PRESENT value of the mode the swapchain got; "" before one

static VkPresentModeKHR pick_present_mode(VkPhysicalDevice pd) {
    const char *e = cfg_value("NX2SARIA_VK_PRESENT");
    VkPresentModeKHR want = !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, g_surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, g_surface, &n, m);
    VkPresentModeKHR got = VK_PRESENT_MODE_FIFO_KHR;
    int relaxed = 0, immediate = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (m[i] == want) got = want;
        relaxed |= m[i] == VK_PRESENT_MODE_FIFO_RELAXED_KHR, immediate |= m[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    // Mailbox missing (several AMD drivers): plain FIFO holds a frame that misses a refresh to the next one, and the
    // game, paced at 60 on a 60 Hz display, drops to 30 at every slow frame. Relaxed FIFO shows a late frame at once
    // (a tear that once) and waits for the refresh otherwise; immediate where that is missing too.
    if (want == VK_PRESENT_MODE_MAILBOX_KHR && got != want)
        got = relaxed ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : got;
    const char *name = got == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : got == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo";
    __atomic_store_n(&g_present_got, name, __ATOMIC_RELEASE);
    static int logged = -1;   // the mode last logged: a change from the menu shows in the log
    if ((int)got != logged) {
        logged = (int)got;
        rt_log("gfx: present mode %s (NX2SARIA_VK_PRESENT=%s)",
               got == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "fifo relaxed" : name, e);
        if (want == VK_PRESENT_MODE_MAILBOX_KHR && got == VK_PRESENT_MODE_IMMEDIATE_KHR)
            rt_log("gfx: the driver offers neither mailbox nor relaxed FIFO for this window, so VSync on presents "
                   "immediately: no vsync, the picture can tear. vk_present = fifo gives strict vsync, with the frame rate "
                   "halved for each frame that misses a refresh");
    }
    return got;
}

const char *gfx_window_present_mode(void) { return __atomic_load_n(&g_present_got, __ATOMIC_ACQUIRE); }

// kern_now_ns when the newest host present returned with its picture taken (flip_locked); 0 before the first. A
// minimised window and a frame hold present nothing, so it ages: RTSS, which holds the frames inside the host present,
// then holds none, and the game's pacer takes the limit back (rtss_decide.h rtss_pace_period).
static uint64_t g_hosted_ns;
uint64_t gfx_window_hosted_ns(void) { return __atomic_load_n(&g_hosted_ns, __ATOMIC_ACQUIRE); }

// Queues the present of a blit vk_present_blit left waiting (g_pl held). A swapchain rebuild and the next blit flush it
// first; the device release drops it.
static void flip_locked(void) {
    if (!g_flip_on) return;
    g_flip_on = 0;
    if (!g_sc) return;
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &g_render_done[g_flip_idx], .swapchainCount = 1, .pSwapchains = &g_sc,
                           .pImageIndices = &g_flip_idx};
    // A driver's low latency (vk_lowlat.c): its present stage or marker right before the present of a game frame it
    // follows, the frame's number on the present where the plain swapchain takes one, and the end marker after it.
    // Nothing for any other present (the boot screen, a frame the mode did not start, the mode off).
    VkLowlatPresent ll;
    vk_queue_lock();
    pi.pNext = vk_lowlat_present_begin(g_flip_frame, &ll);
    g_flip_frame = 0;
    VkResult r = vk_dlssg_active() ? vk_dlssg_present(vk_ctx()->gfx_queue, &pi)
               : vk_fg_active()    ? vk_fg_present(&pi) : vkQueuePresentKHR(vk_ctx()->gfx_queue, &pi);
    vk_queue_unlock();
    vk_lowlat_present_end(&ll);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) __atomic_store_n(&g_hosted_ns, kern_now_ns(), __ATOMIC_RELEASE);
    vk_device_lost_check(r, "the present");
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) plat_xchg32(&g_dirty, 1);
}

static void free_capture(void) {
    VkCtx *c = vk_ctx();
    if (g_cap_buf && c) vmaDestroyBuffer(c->vma, g_cap_buf, g_cap_alloc);
    g_cap_buf = VK_NULL_HANDLE;
    g_cap_ptr = NULL;
    g_cap_size = 0;
    g_cap_valid = 0;
}

// Destroys g_sc (when there is one: the frame generation paths destroy theirs themselves) with its image views and its
// images' render-done semaphores; the caller holds the queue lock and the device is idle. The swapchain goes before
// the semaphores: a present it still holds may wait on one of them.
static void drop_swapchain_locked(VkCtx *c) {
    for (uint32_t i = 0; i < g_sc_n; i++) {
        if (g_sc_view[i]) vkDestroyImageView(c->device, g_sc_view[i], NULL);
        g_sc_view[i] = VK_NULL_HANDLE;
    }
    if (g_sc && vk_dlssg_active()) vk_dlssg_destroy_swapchain(c->device, g_sc);
    else if (g_sc) vkDestroySwapchainKHR(c->device, g_sc, NULL);
    for (uint32_t i = 0; i < g_sc_n; i++) {
        if (g_render_done[i]) vkDestroySemaphore(c->device, g_render_done[i], NULL);
        g_render_done[i] = VK_NULL_HANDLE;
    }
    g_sc = VK_NULL_HANDLE;
    g_sc_n = 0;
}

// Returns 1 with a usable swapchain, 0 when the window has no area (minimised) or the create failed, which leaves
// g_dirty set so the next present tries again. Called with g_pl held. g_dirty is cleared before the window's size is
// read: a resize that arrives while the swapchain is being made (AMD's frame generation swapchain takes tens of ms)
// sets it again, where clearing it at the end lost that resize and kept a swapchain of the old size, whose acquires
// then timed out frame after frame behind AMD's presenter (the picture stopped).
static void ng_drain(void);

// The swapchain create_swapchain just made cannot be used: its image list (gfx_guard.h gfx_sc_images_ok) or a
// semaphore for one of its images failed. A present with it would name images or semaphores that are not there, so it
// goes with what was made for it, and the next present makes one again (g_dirty). call names the step that failed, r
// its result, detail the counts; *count is that step's failures, logged as gfx_fail_log_due allows: "gfx: <call>
// failed (<r>), <n> times: <detail>". Caller holds the queue lock, which this releases. Returns 0, create_swapchain's
// failure.
static int swapchain_unusable(VkCtx *c, volatile int32_t *count, const char *call, VkResult r, const char *detail) {
    uint32_t k = (uint32_t)plat_inc32(count);
    if (gfx_fail_log_due(k))
        rt_log("gfx: %s failed (%d), %u times: %s; the swapchain is made again at the next present", call, (int)r, k,
               detail);
    vk_lowlat_swapchain(VK_NULL_HANDLE, 0);
    if (vk_fg_active()) vk_fg_destroy_swapchain(g_sc), g_sc = VK_NULL_HANDLE;   // AMD's: its own destroy
    drop_swapchain_locked(c);
    vk_queue_unlock();
    plat_xchg32(&g_dirty, 1);
    return 0;
}

// Surface-format lists can exceed 64 entries when a compositor advertises several colour spaces.
// Requery on VK_INCOMPLETE: the available list may grow between the count and data calls.
static VkSurfaceFormatKHR *surface_formats(VkPhysicalDevice phys, VkSurfaceKHR surface, uint32_t *count) {
    *count = 0;
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        uint32_t n = 0;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, NULL) != VK_SUCCESS || !n) return NULL;
        VkSurfaceFormatKHR *formats = malloc((size_t)n * sizeof *formats);
        if (!formats) return NULL;
        VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats);
        if (result == VK_SUCCESS && n) {
            *count = n;
            return formats;
        }
        free(formats);
        if (result != VK_INCOMPLETE) return NULL;
    }
    return NULL;
}

static int create_swapchain(void) {
    flip_locked();   // the acquired image goes out before its swapchain is replaced
    ng_drain();      // and what the NGX frame generation presenter still holds (nothing unless it runs)
    plat_xchg32(&g_dirty, 0);
    VkCtx *c = vk_ctx();
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->phys, g_surface, &caps) != VK_SUCCESS) return plat_xchg32(&g_dirty, 1), 0;
    int cw = 0, ch = 0;
    wsys_client_size(&cw, &ch);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent
                                                            : (VkExtent2D){(uint32_t)cw, (uint32_t)ch};
    if (!ext.width || !ext.height) return plat_xchg32(&g_dirty, 1), 0;
    uint32_t nf = 0;
    VkSurfaceFormatKHR *fmts = surface_formats(c->phys, g_surface, &nf);
    if (!fmts) return plat_xchg32(&g_dirty, 1), 0;
    rt_log("gfx: queried all %u surface formats", nf);
    // UNORM: the blit writes the bytes the game wrote (no sRGB encode). NX2SARIA_HDR: HDR10 when the surface offers it.
    VkSurfaceFormatKHR sf = fmts[0];
    g_sc_hdr_wanted = vk_hdr_wanted();
    int hdr = g_sc_hdr_wanted && vk_hdr_surface_format(fmts, nf, &sf), found = hdr;
    if (!hdr) vk_hdr_images_release();   // HDR off, or not on this display: what its encode made goes
    for (uint32_t i = 0; i < nf && !found; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; found = 1; }
    for (uint32_t i = 0; i < nf && !found; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; found = 1; }
    free(fmts);
    // NGX frame generation's presenter holds the generated frames' images too, and the rendered frame waits for it:
    // room for both
    const uint32_t ngx = vk_ngxfg_wanted() ? 1 : 0, most = ngx ? 12 : 8;
    uint32_t count = caps.minImageCount + 1 + (ngx ? 2u + NG_MAX : 0u);
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > most) count = most;

    // A rebuild holds the queue lock across the device's idle wait and the create, and every submission waits behind
    // it: the made swapchain's line says how long (the first 16 and each doubling of the count).
    const uint64_t t_ask = plat_now_ns();
    vk_queue_lock();
    const uint64_t t_held = plat_now_ns();
    vkDeviceWaitIdle(c->device);
    const uint64_t t_idle = plat_now_ns();
    vk_lowlat_swapchain(VK_NULL_HANDLE, 0);   // NVIDIA's low-latency calls name the swapchain: none while it is replaced
    VkSwapchainKHR old = g_sc;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,   // every surface supports it (the menu overlay draws into it)
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = pick_present_mode(c->phys), .clipped = VK_TRUE,
        .oldSwapchain = old};
    // Frame generation (vk_fg.cpp): AMD's swapchain in this one's place. Its swapchain is replaced only after the old
    // one is gone; a first creation that fails leaves the plain swapchain below.
    VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
    if (!vk_fg_wanted() && vk_fg_active()) {   // switched off in the menu: AMD's swapchain and contexts go, the plain one follows
        if (old) vk_fg_destroy_swapchain(old);
        vk_fg_release();
        ci.oldSwapchain = VK_NULL_HANDLE;
        old = g_sc = VK_NULL_HANDLE;
    }
    if (vk_fg_wanted()) {
        if (old && vk_fg_active()) vk_fg_destroy_swapchain(old);
        else if (old) vkDestroySwapchainKHR(c->device, old, NULL);
        ci.oldSwapchain = VK_NULL_HANDLE;
        old = g_sc = VK_NULL_HANDLE;
        r = vk_fg_create_swapchain(&ci, &g_sc);
        // once AMD's swapchain context exists every swapchain call goes through it: a plain swapchain in its place would
        // get AMD's acquire and present. Try again at the next present instead.
        if (r != VK_SUCCESS && vk_fg_active()) {
            static volatile int32_t n_fail;
            if (plat_inc32(&n_fail) <= 5) rt_log("gfx: frame generation swapchain %ux%u failed (%d); trying again", ext.width, ext.height, (int)r);
            g_sc = VK_NULL_HANDLE;
            drop_swapchain_locked(c);   // the old swapchain's views and semaphores
            vk_queue_unlock();
            plat_xchg32(&g_dirty, 1);
            return 0;
        }
    }
    // NVIDIA's low latency (vk_lowlat.c): the plain swapchain is made with the latency mode while the setting is on,
    // the only structure in its chain. A frame generation's swapchain above is not: the calls cannot name it.
    const VkSwapchainLatencyCreateInfoNV lat = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV,
                                                .latencyModeEnable = VK_TRUE};
    int ll_sc = 0;
    g_sc_ll_wanted = vk_lowlat_swapchain_wanted();
    if (r != VK_SUCCESS && vk_dlssg_active()) r = vk_dlssg_create_swapchain(c->device, &ci, &g_sc);   // Streamline's hook
    else if (r != VK_SUCCESS) {
        if (g_sc_ll_wanted) ci.pNext = &lat, ll_sc = 1;
        r = vkCreateSwapchainKHR(c->device, &ci, NULL, &g_sc);
    }
    {   // three creates in a row that fail with the latency mode asked for: the next is made without it
        static int ll_failed;
        ll_failed = r != VK_SUCCESS && ll_sc ? ll_failed + 1 : 0;
        if (ll_failed >= 3) vk_lowlat_swapchain_refused((int)r);
    }
    if (r != VK_SUCCESS) {
        static volatile int32_t n_failed;
        int32_t nf_log = plat_inc32(&n_failed);
        if (nf_log <= 8 || !(nf_log & 1023)) rt_log("gfx: vkCreateSwapchainKHR %ux%u failed (%d), %d times", ext.width, ext.height, (int)r, nf_log);
        // A create with oldSwapchain retires the old swapchain even when it fails: it can no longer acquire or be an
        // oldSwapchain again. It goes now (the device is idle), and the next present creates one from nothing.
        g_sc = old;   // NULL when a frame generation path destroyed it already: its views and semaphores still go
        drop_swapchain_locked(c);
        vk_queue_unlock();
        plat_xchg32(&g_dirty, 1);
        return 0;
    }
    VkSwapchainKHR made = g_sc;
    g_sc = old;   // as above: NULL when a frame generation path destroyed it
    drop_swapchain_locked(c);
    g_sc = made;
    g_sc_fmt = sf.format;
    static int hdr_was = -1;   // the log says when a later swapchain is of the other kind (the menu's HDR row)
    if (hdr_was >= 0 && hdr_was != hdr)
        rt_log("gfx: the swapchain is now %s (format %d)", hdr ? "HDR10" : "SDR", (int)sf.format);
    hdr_was = hdr;
    g_sc_hdr = hdr;
    vk_hdr_set_active(hdr);
    if (hdr) vk_hdr_metadata(g_sc, 1);
    g_sc_ext = ext;
    vk_lowlat_swapchain(ll_sc ? g_sc : VK_NULL_HANDLE, 1);   // the sleep mode is set on it; none: not one for that path
    // The image list, the count first and then that many images, on every path (AMD's swapchain does not cut a larger
    // capacity down to its count). An acquire returns an index below the count, and every array here has
    // MAX_IMAGES slots: a list that did not come whole (an error, VK_INCOMPLETE, fewer images than counted, more
    // than the arrays hold) leaves the swapchain unused rather than presenting with images that are not there.
    static volatile int32_t n_list_failed, n_sem_failed;
    uint32_t n_img = 0, got = 0;
    memset(g_sc_img, 0, sizeof g_sc_img);
    VkResult rc = vk_fg_active()    ? vk_fg_get_images(g_sc, &n_img, NULL)
                : vk_dlssg_active() ? vk_dlssg_get_images(c->device, g_sc, &n_img, NULL)
                                    : vkGetSwapchainImagesKHR(c->device, g_sc, &n_img, NULL);
    VkResult rl = VK_ERROR_INITIALIZATION_FAILED;
    if (rc == VK_SUCCESS && n_img > 0 && n_img <= MAX_IMAGES) {
        got = n_img;
        rl = vk_fg_active()    ? vk_fg_get_images(g_sc, &got, g_sc_img)
           : vk_dlssg_active() ? vk_dlssg_get_images(c->device, g_sc, &got, g_sc_img)
                               : vkGetSwapchainImagesKHR(c->device, g_sc, &got, g_sc_img);
    }
    char why[160];
    if (!gfx_sc_images_ok((int)rc, n_img, (int)rl, got, MAX_IMAGES)) {
        if (rc != VK_SUCCESS) snprintf(why, sizeof why, "the image count");
        else if (!n_img || n_img > MAX_IMAGES) snprintf(why, sizeof why, "%u images, room for 1 to %d", n_img, MAX_IMAGES);
        else snprintf(why, sizeof why, "the image list, %u of %u images", got, n_img);
        return swapchain_unusable(c, &n_list_failed, "vkGetSwapchainImagesKHR", rc != VK_SUCCESS ? rc : rl, why);
    }
    g_sc_n = got;
    for (uint32_t i = 0; i < g_sc_n; i++)   // a null image in a list that came whole is a broken list: an acquire
        if (!g_sc_img[i]) {                 // may name it, or one past it
            snprintf(why, sizeof why, "image %u of %u is null", i, got);
            g_sc_n = 0;
            return swapchain_unusable(c, &n_list_failed, "vkGetSwapchainImagesKHR", rl, why);
        }
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < g_sc_n; i++) {
        // Each image's render-done semaphore: the present of that image waits on it, so an image without one cannot
        // be shown at all (the handles not made are null, which drop_swapchain_locked skips).
        VkResult rs = vkCreateSemaphore(c->device, &semi, NULL, &g_render_done[i]);
        if (rs != VK_SUCCESS) {
            g_render_done[i] = VK_NULL_HANDLE;
            snprintf(why, sizeof why, "the render-done semaphore of swapchain image %u of %u", i, g_sc_n);
            return swapchain_unusable(c, &n_sem_failed, "vkCreateSemaphore", rs, why);
        }
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_sc_img[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(c->device, &vi, NULL, &g_sc_view[i]) != VK_SUCCESS) g_sc_view[i] = VK_NULL_HANDLE;
    }
    free_capture();
    {
        static uint32_t n_made;
        const uint64_t t_end = plat_now_ns();
        if (++n_made <= 16 || !(n_made & (n_made - 1)))
            rt_log("gfx: swapchain %ux%u made in %.1f ms: %.1f ms waiting for the queue lock, then the lock held "
                   "%.1f ms (%.1f ms of it waiting for the device to idle); %u made so far", ext.width, ext.height,
                   (t_end - t_ask) / 1e6, (t_held - t_ask) / 1e6, (t_end - t_held) / 1e6, (t_idle - t_held) / 1e6,
                   n_made);
    }
    vk_queue_unlock();
    return 1;
}

static void sync_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 s0,
                         VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = s0, .srcAccessMask = a0,
                               .dstStageMask = s1, .dstAccessMask = a1, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static int ensure_capture_buffer(void) {
    VkCtx *c = vk_ctx();
    VkDeviceSize need = (VkDeviceSize)g_sc_ext.width * g_sc_ext.height * 4;
    if (g_cap_buf && g_cap_size == need) return 1;
    free_capture();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = need, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &g_cap_buf, &g_cap_alloc, &info) != VK_SUCCESS) { g_cap_buf = VK_NULL_HANDLE; return 0; }
    g_cap_ptr = info.pMappedData;
    g_cap_size = need;
    return g_cap_ptr != NULL;
}

void vk_thumbs_enable(int n) {
    plat_rmutex_lock(&g_pl);
    if (!g_th_n && n > 0 && (g_th_tag = calloc((size_t)n, sizeof *g_th_tag))) g_th_n = n;
    plat_rmutex_unlock(&g_pl);
}

void vk_thumbs_frame(uint64_t frame) {
    plat_rmutex_lock(&g_pl);
    g_th_frame = frame;
    plat_rmutex_unlock(&g_pl);
}

static void free_thumbs(void) {
    VkCtx *c = vk_ctx();
    if (g_th_img && c) vmaDestroyImage(c->vma, g_th_img, g_th_img_alloc);
    if (g_th_buf && c) vmaDestroyBuffer(c->vma, g_th_buf, g_th_alloc);
    g_th_img = VK_NULL_HANDLE, g_th_buf = VK_NULL_HANDLE, g_th_ptr = NULL;
    if (g_th_tag) memset(g_th_tag, 0, sizeof *g_th_tag * (size_t)g_th_n);
    memset(g_th_pending, 0, sizeof g_th_pending);
}

// The ring and the blit target in the swapchain's format; allocated on the first present, the image again when the
// swapchain format changes. A failure turns the thumbnails off for the session.
static int ensure_thumbs(void) {
    if (!g_th_n || g_th_failed || !g_th_frame) return 0;
    VkCtx *c = vk_ctx();
    if (!g_th_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = TH_BYTES * (VkDeviceSize)g_th_n,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &g_th_buf, &g_th_alloc, &info) != VK_SUCCESS || !info.pMappedData) {
            if (g_th_buf) vmaDestroyBuffer(c->vma, g_th_buf, g_th_alloc);
            g_th_buf = VK_NULL_HANDLE;
            g_th_failed = 1;
            rt_log("gfx: flight thumbnails off: no host-visible ring of %llu MB", (unsigned long long)(bi.size >> 20));
            return 0;
        }
        g_th_ptr = info.pMappedData;
    }
    // an HDR10 swapchain blits into RGBA8: its PQ codes, flat-looking but readable as bytes
    VkFormat tf = g_sc_hdr ? VK_FORMAT_R8G8B8A8_UNORM : g_sc_fmt;
    if (g_th_img && g_th_fmt != tf) {
        vmaDestroyImage(c->vma, g_th_img, g_th_img_alloc);   // the previous present's fence was waited for: idle
        g_th_img = VK_NULL_HANDLE;
    }
    if (!g_th_img) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = tf,
                                .extent = {FLIGHT_THUMB_W, FLIGHT_THUMB_H, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &g_th_img, &g_th_img_alloc, NULL) != VK_SUCCESS) {
            g_th_img = VK_NULL_HANDLE;
            g_th_failed = 1;
            rt_log("gfx: flight thumbnails off: no %ux%u image", FLIGHT_THUMB_W, FLIGHT_THUMB_H);
            return 0;
        }
        g_th_fmt = tf;
    }
    return 1;
}

// dst (TRANSFER_SRC, the swapchain image just written) -> thumbnail image -> ring slot of g_th_frame.
static void record_thumb(VkCommandBuffer cb, VkImage dst, unsigned fence_slot) {
    uint64_t f = g_th_frame;
    unsigned ring = (unsigned)(f % (uint64_t)g_th_n);
    g_th_tag[ring] = 0;   // under g_pl: vk_thumbs_read no longer hands this slot out
    sync_barrier(cb, g_th_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)g_sc_ext.width, (int)g_sc_ext.height, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {FLIGHT_THUMB_W, FLIGHT_THUMB_H, 1}}};
    vkCmdBlitImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_th_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);
    sync_barrier(cb, g_th_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy bc = {.bufferOffset = TH_BYTES * ring, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {FLIGHT_THUMB_W, FLIGHT_THUMB_H, 1}};
    vkCmdCopyImageToBuffer(cb, g_th_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_th_buf, 1, &bc);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
    g_th_pending[fence_slot] = f;
    g_th_frame = 0;
}

int vk_thumbs_read(uint64_t frame, uint8_t *rgba) {
    int ok = 0;
    plat_rmutex_lock(&g_pl);
    if (g_th_n && g_th_ptr && frame) {
        unsigned ring = (unsigned)(frame % (uint64_t)g_th_n);
        if (g_th_tag[ring] == frame) {
            VkCtx *c = vk_ctx();
            vmaInvalidateAllocation(c->vma, g_th_alloc, TH_BYTES * ring, TH_BYTES);
            memcpy(rgba, g_th_ptr + TH_BYTES * ring, TH_BYTES);
            ok = g_th_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_th_fmt == VK_FORMAT_B8G8R8A8_SRGB ? 2 : 1;
        }
    }
    plat_rmutex_unlock(&g_pl);
    if (ok == 2)
        for (size_t i = 0; i < TH_BYTES; i += 4) {
            uint8_t t = rgba[i];
            rgba[i] = rgba[i + 2], rgba[i + 2] = t;
        }
    return ok != 0;
}

// The flight recorder's flash trigger (flight.c flight_cells) gets the cell means of frame's thumbnail, which has just
// completed in its ring slot (g_pl held). flash_cells reads a quarter of the slot's pixels, about 15 us measured on
// ordinary memory (tests/flight_flash_check.py time); the ring is mapped host memory allocated for random access
// (ensure_thumbs), which is cached memory where the device has it, as vk_thumbs_read's copy of a whole slot relies on.
static void thumb_cells(uint64_t frame) {
    unsigned ring = (unsigned)(frame % (uint64_t)g_th_n);
    uint8_t cells[FLASH_CELLS];
    vmaInvalidateAllocation(vk_ctx()->vma, g_th_alloc, TH_BYTES * ring, TH_BYTES);
    flash_cells(g_th_ptr + TH_BYTES * ring, FLIGHT_THUMB_W, FLIGHT_THUMB_H, cells);
    flight_cells(frame, cells);
}

// The memory report's swapchain (vk_memory.c outside_log): its images and their size; the bytes are the pixels alone,
// 8 a pixel for a half-float format and 4 for the others (the driver's padding and compression data are not known).
// Read without g_pl: the figures of a swapchain made again meanwhile are a report late. The images of a frame
// generation's swapchain are the ones counted (create_swapchain takes them from it).
void vk_present_swapchain_mem(uint32_t *n, uint32_t *w, uint32_t *h, uint64_t *bytes) {
    *n = __atomic_load_n(&g_sc_n, __ATOMIC_RELAXED);
    *w = __atomic_load_n(&g_sc_ext.width, __ATOMIC_RELAXED), *h = __atomic_load_n(&g_sc_ext.height, __ATOMIC_RELAXED);
    VkFormat f = __atomic_load_n(&g_sc_fmt, __ATOMIC_RELAXED);
    uint32_t px = f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    *bytes = __atomic_load_n(&g_sc, __ATOMIC_RELAXED) ? (uint64_t)*n * *w * *h * px : 0;
}

void vk_present_capture(int on) {
    plat_rmutex_lock(&g_pl);
    g_capture = on;
    if (!on) g_cap_valid = 0;
    plat_rmutex_unlock(&g_pl);
}

int vk_present_captured(const uint8_t **px, int *w, int *h, VkFormat *fmt) {
    plat_rmutex_lock(&g_pl);
    int ok = g_cap_valid;
    if (ok) { *px = g_cap_ptr; *w = g_cap_w; *h = g_cap_h; *fmt = g_sc_fmt; }
    plat_rmutex_unlock(&g_pl);
    return ok;
}

static float g_pic[4];   // gfx_picture_rect
static volatile int32_t g_pic_ok;

// ---- vk_present_grab (vk.h): one presented frame's pixels for a bug capture. The request is taken by the next
// present (g_pl held), which copies the read-back buffer and calls back; the PNG is made by the caller's own thread.
static void (*volatile g_grab_cb)(VkGrab *g, void *ctx);
static void *g_grab_ctx;

int vk_present_grab(void (*cb)(VkGrab *g, void *ctx), void *ctx) {
    plat_rmutex_lock(&g_pl);
    int ok = !g_grab_cb;
    if (ok) g_grab_ctx = ctx, g_grab_cb = cb;
    plat_rmutex_unlock(&g_pl);
    return ok;
}

void vk_present_grab_cancel(void) {
    plat_rmutex_lock(&g_pl);
    g_grab_cb = NULL;
    plat_rmutex_unlock(&g_pl);
}

void vk_present_grab_rgba(const VkGrab *g, uint8_t *out) {
    size_t n = (size_t)g->w * (size_t)g->h * 4;
    if (g->hdr) {
        vk_hdr_to_srgb8(g->px, out, n / 4, g->a2r);
        return;
    }
    for (size_t i = 0; i < n; i += 4) {
        out[i] = g->px[i + (g->bgra ? 2 : 0)];
        out[i + 1] = g->px[i + 1];
        out[i + 2] = g->px[i + (g->bgra ? 0 : 2)];
        out[i + 3] = 255;
    }
}

void vk_present_grab_free(VkGrab *g) {
    if (g) free(g->px);
    free(g);
}

// The present that took the request: the read-back buffer holds this frame (g_cap_ptr, the fence waited for).
static void grab_deliver(int image_w, int image_h) {
    void (*cb)(VkGrab *g, void *ctx) = g_grab_cb;
    void *ctx = g_grab_ctx;
    g_grab_cb = NULL;
    size_t n = (size_t)g_cap_w * (size_t)g_cap_h * 4;
    VkGrab *g = calloc(1, sizeof *g);
    if (g && (g->px = malloc(n))) {
        memcpy(g->px, g_cap_ptr, n);
        g->w = g_cap_w, g->h = g_cap_h;
        g->bgra = g_sc_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_sc_fmt == VK_FORMAT_B8G8R8A8_SRGB;
        g->hdr = g_sc_hdr != 0, g->a2r = g_sc_fmt == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        g->image_w = image_w, g->image_h = image_h, g->scaler = g_scaler;
        if (__atomic_load_n(&g_pic_ok, __ATOMIC_ACQUIRE)) memcpy(g->pic, g_pic, sizeof g->pic);
    } else {
        free(g);
        g = NULL;
    }
    cb(g, ctx);   // NULL: no memory for the copy
}

// The captured swapchain image as a PNG (the menu's test hook), RGBA with opaque alpha. From an HDR10 swapchain the
// log also gets the luminance in nits of the picture's rectangle (vk_hdr_luminance_stats: the black floor of a dark
// scene, for one), and <path>.pq10 keeps the raw pixels: "PQ10", width, height, 1 when A2R10G10B10 (u32 each), then
// the pixels as the swapchain holds them.
static void save_capture(const char *path) {
    size_t n = (size_t)g_cap_w * (size_t)g_cap_h * 4;
    uint8_t *px = malloc(n);
    if (!px) return;
    int bgra = g_sc_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_sc_fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (g_sc_hdr) vk_hdr_to_srgb8(g_cap_ptr, px, n / 4, g_sc_fmt == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
    else for (size_t i = 0; i < n; i += 4) {
        px[i] = g_cap_ptr[i + (bgra ? 2 : 0)];
        px[i + 1] = g_cap_ptr[i + 1];
        px[i + 2] = g_cap_ptr[i + (bgra ? 0 : 2)];
        px[i + 3] = 255;
    }
    rt_write_png(path, px, (uint32_t)g_cap_w, (uint32_t)g_cap_h);
    free(px);
    rt_log("gfx: menu test shot %s (%dx%d)", path, g_cap_w, g_cap_h);
    if (!g_sc_hdr) return;
    int a2r = g_sc_fmt == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    int rect[4] = {0, 0, g_cap_w, g_cap_h};
    if (__atomic_load_n(&g_pic_ok, __ATOMIC_ACQUIRE)) {
        rect[0] = (int)(g_pic[0] * (float)g_cap_w + 0.5f), rect[1] = (int)(g_pic[1] * (float)g_cap_h + 0.5f);
        rect[2] = (int)(g_pic[2] * (float)g_cap_w + 0.5f), rect[3] = (int)(g_pic[3] * (float)g_cap_h + 0.5f);
        if (!gfx_rect_inside(rect, g_cap_w, g_cap_h)) rect[0] = rect[1] = 0, rect[2] = g_cap_w, rect[3] = g_cap_h;
    }
    float st[7];
    vk_hdr_luminance_stats(g_cap_ptr, g_cap_w, rect, a2r, st);
    rt_log("gfx: shot %s, HDR10 luminance of the picture (%dx%d at %d,%d) in nits: darkest %.5f, 0.1%% %.5f, 1%% %.5f, "
           "median %.3f, 99%% %.1f, brightest %.1f; %.2f%% of pixels under 0.01", path, rect[2], rect[3], rect[0], rect[1],
           st[0], st[1], st[2], st[3], st[4], st[5], 100.0f * st[6]);
    char raw[1200];
    snprintf(raw, sizeof raw, "%s.pq10", path);
    FILE *f = fopen(raw, "wb");
    if (f) {
        uint32_t head[4] = {0x30315150u /* "PQ10" */, (uint32_t)g_cap_w, (uint32_t)g_cap_h, (uint32_t)a2r};
        fwrite(head, sizeof head, 1, f);
        fwrite(g_cap_ptr, 4, (size_t)g_cap_w * (size_t)g_cap_h, f);
        fclose(f);
    }
}

// The rectangle {x, y, w, h} of a dw x dh swapchain image the cw x ch picture goes into: the whole image with
// NX2SARIA_ASPECT=stretch, else (fit, the default) the largest one of the picture's own aspect ratio, centred (a 16:9 picture
// on the Steam Deck's 1280x800 screen gets bars of 40 rows above and below; a 16:9 picture in a 21:9 window, bars at the
// sides).
// Where the last present put the picture, as fractions of the swapchain (x, y, w, h; g_pic): menu_cursor.c's pointer
int gfx_picture_rect(float out[4]) {
    if (!__atomic_load_n(&g_pic_ok, __ATOMIC_ACQUIRE)) return 0;
    memcpy(out, g_pic, sizeof g_pic);
    return 1;
}

void gfx_fit_rect(int cw, int ch, int dw, int dh, int out[4]) {
    out[0] = out[1] = 0, out[2] = dw, out[3] = dh;
    if (cw <= 0 || ch <= 0 || !strcmp(cfg_value("NX2SARIA_ASPECT"), "stretch")) return;
    if ((int64_t)dw * ch > (int64_t)dh * cw) out[2] = (int)(((int64_t)dh * cw + ch / 2) / ch);   // window wider: pillarbox
    else out[3] = (int)(((int64_t)dw * ch + cw / 2) / cw);                                        // taller: letterbox
    // A side of the window's parity keeps the two bars equal; a picture one pixel short of the window fills it
    out[2] += (dw - out[2]) & 1, out[3] += (dh - out[3]) & 1;
    out[0] = (dw - out[2]) / 2, out[1] = (dh - out[3]) / 2;
}

// ---- Movie overlay (ov_movie2.c): the latest decoded movie frame goes over the game image, fit to the window
// with black bars, while frames keep coming (GFX_MOVIE_HOLD_NS after the last one it disappears). It made movies
// visible before the game's own shaders translated; now the game draws them itself (its YUV compute pass, then the
// upscaler fallback of vk_exec_draw.c), under its menus, so the overlay is off unless NX2SARIA_MOVIE_OVERLAY=1. One staging buffer per frame in flight: the CPU writes slot i only after
// that slot's fence (the present waits it before recording), the GPU copies it into one RGBA8 image.
#define GFX_MOVIE_HOLD_NS 400000000ull
static PlatMutex g_mv_lock = PLAT_MUTEX_INIT;
static uint8_t *g_mv_rgba;
static int g_mv_w, g_mv_h;
static uint64_t g_mv_ns, g_mv_serial;
static VkBuffer g_mvb[FRAMES];
static VmaAllocation g_mva[FRAMES];
static void *g_mvp[FRAMES];
static VkDeviceSize g_mvsz[FRAMES];
static uint64_t g_mv_slot_serial[FRAMES];
static VkImage g_mvi;
static VmaAllocation g_mvia;
static int g_mvi_w, g_mvi_h;

void gfx_movie_frame(const uint8_t *rgba, int w, int h) {
    if (w <= 0 || h <= 0) return;
    plat_mutex_lock(&g_mv_lock);
    size_t need = (size_t)w * h * 4;
    if (w != g_mv_w || h != g_mv_h || !g_mv_rgba) {
        free(g_mv_rgba);
        g_mv_rgba = malloc(need);
        g_mv_w = g_mv_rgba ? w : 0, g_mv_h = g_mv_rgba ? h : 0;
    }
    if (g_mv_rgba) {
        memcpy(g_mv_rgba, rgba, need);
        g_mv_ns = plat_now_ns();
        g_mv_serial++;
    }
    plat_mutex_unlock(&g_mv_lock);
}

void gfx_movie_frame_own(uint8_t *rgba, int w, int h) {
    if (w <= 0 || h <= 0 || !rgba) {
        free(rgba);
        return;
    }
    plat_mutex_lock(&g_mv_lock);
    uint8_t *old = g_mv_rgba;
    g_mv_rgba = rgba;
    g_mv_w = w, g_mv_h = h;
    g_mv_ns = plat_now_ns();
    g_mv_serial++;
    plat_mutex_unlock(&g_mv_lock);
    free(old);
}

void gfx_movie_stop(void) {
    plat_mutex_lock(&g_mv_lock);
    g_mv_ns = 0;
    plat_mutex_unlock(&g_mv_lock);
}

// Records the overlay into cb for swapchain image dst (in TRANSFER_DST_OPTIMAL, left so). 1 when drawn.
static int movie_overlay(VkCommandBuffer cb, int slot, VkImage dst) {
    static int off = -1;
    if (off < 0) {
        const char *v = cfg_get("NX2SARIA_MOVIE_OVERLAY");
        off = !(v && strcmp(v, "0"));
    }
    if (off) return 0;
    VkCtx *c = vk_ctx();
    plat_mutex_lock(&g_mv_lock);
    if (!g_mv_ns || plat_now_ns() - g_mv_ns > GFX_MOVIE_HOLD_NS || !g_mv_rgba) {
        plat_mutex_unlock(&g_mv_lock);
        return 0;
    }
    int w = g_mv_w, h = g_mv_h;
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    if (g_mvsz[slot] != need) {
        if (g_mvb[slot]) vmaDestroyBuffer(c->vma, g_mvb[slot], g_mva[slot]);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = need,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &g_mvb[slot], &g_mva[slot], &info) != VK_SUCCESS) {
            g_mvb[slot] = VK_NULL_HANDLE, g_mvsz[slot] = 0;
            plat_mutex_unlock(&g_mv_lock);
            return 0;
        }
        g_mvp[slot] = info.pMappedData;
        g_mvsz[slot] = need;
        g_mv_slot_serial[slot] = 0;
    }
    if (g_mv_slot_serial[slot] != g_mv_serial) {
        memcpy(g_mvp[slot], g_mv_rgba, (size_t)need);
        g_mv_slot_serial[slot] = g_mv_serial;
    }
    plat_mutex_unlock(&g_mv_lock);
    if (g_mvi_w != w || g_mvi_h != h) {
        if (g_mvi) {
            vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();
            vmaDestroyImage(c->vma, g_mvi, g_mvia);
            g_mvi = VK_NULL_HANDLE;
        }
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {(uint32_t)w, (uint32_t)h, 1},
                                .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &g_mvi, &g_mvia, NULL) != VK_SUCCESS) {
            g_mvi = VK_NULL_HANDLE, g_mvi_w = g_mvi_h = 0;
            return 0;
        }
        g_mvi_w = w, g_mvi_h = h;
    }
    sync_barrier(cb, g_mvi, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyBufferToImage(cb, g_mvb[slot], g_mvi, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    sync_barrier(cb, g_mvi, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    // the game image's blit wrote dst: black bars first, then the frame
    sync_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    sync_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int to[4];
    gfx_fit_rect(w, h, (int)g_sc_ext.width, (int)g_sc_ext.height, to);
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{0, 0, 0}, {w, h, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .dstOffsets = {{to[0], to[1], 0}, {to[0] + to[2], to[1] + to[3], 1}}};
    vkCmdBlitImage(cb, g_mvi, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);
    return 1;
}

// The game image's crop into dst at `to`: colour adjustment (or the HDR10 encode), then FSR or the bilinear blit; dst in
// TRANSFER_DST_OPTIMAL. Returns vk_fsr_record's result (0: the blit scaled it).
static int put_picture(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, int w, int h, const int rect[4], VkImage dst,
                       const int to[4], int general) {
    int cw = rect[2], ch = rect[3];
    // Colour adjustment (NX2SARIA_COLOR_*, vk_color.c), or on an HDR10 swapchain the PQ encode (vk_hdr.c; the adjustment is
    // for SDR): a copy of the crop takes the game image's place below
    VkImage graded = !general ? VK_NULL_HANDLE : g_sc_hdr ? vk_hdr_encode(cb, slot, src, fmt, rect)
                                                         : vk_color_record(cb, slot, src, fmt, rect);
    const int grect[4] = {0, 0, cw, ch};
    VkImage psrc = graded ? graded : src;
    VkFormat pfmt = graded ? VK_FORMAT_R16G16B16A16_SFLOAT : fmt;
    const int *prect = graded ? grect : rect;
    // FSR (NX2SARIA_UPSCALER): EASU when the picture's rectangle is larger than the crop, the shrink and RCAS when it is
    // smaller; else the bilinear blit
    int fsr = general ? vk_fsr_record(cb, slot, psrc, pfmt, graded ? cw : w, graded ? ch : h, prect, dst, to) : 0;
    if (!fsr && src) {
        if (general)   // a GENERAL source (vk_texture.c images): earlier submissions' writes become visible to the blit here
            sync_barrier(cb, psrc, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkImageBlit region = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{prect[0], prect[1], 0}, {prect[0] + cw, prect[1] + ch, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{to[0], to[1], 0}, {to[0] + to[2], to[1] + to[3], 1}}};
        vkCmdBlitImage(cb, psrc, general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        if (general)   // later submissions that write the source wait for the blit's read
            sync_barrier(cb, psrc, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    }
    return fsr;
}

// The display's frame rate with frame generation: the frames the display got (generated ones included: vk_fg.cpp
// vk_fg_presents, vk_dlssg.c vk_dlssg_shown) over half a second, sampled at each of the game's presents
// (gfx_output_fps).
static volatile double g_out_fps;
static uint64_t ng_presented(void);   // frames the NGX frame generation presenter showed (0: it does not run)
static int ng_running(void);          // that presenter is in use
static void output_rate(void) {
    static uint64_t t0, n0;
    uint64_t ng_n = ng_presented();
    uint64_t n = ng_n ? ng_n : vk_dlssg_active() ? vk_dlssg_shown() : vk_fg_presents(g_sc), t = plat_now_ns();
    if (!n) { g_out_fps = 0, t0 = 0; return; }
    if (!t0 || n < n0) { t0 = t, n0 = n; return; }
    if (t - t0 >= 500000000ull) g_out_fps = (double)(n - n0) * 1e9 / (double)(t - t0), t0 = t, n0 = n;
}
double gfx_output_fps(void) { return g_out_fps; }

static VkImage g_hl;                 // frame generation's HUD-less picture at the swapchain's size (hudless_record)
static VmaAllocation g_hl_mem;
static VkImageView g_hl_view;
static uint32_t g_hl_w, g_hl_h;
static VkFormat g_hl_fmt;

static VkImage hudless_record(VkCommandBuffer cb, unsigned slot, int w, int h, const int rect[4], const int to[4], int general) {
    VkImage pic;
    VkImageView pv;
    uint32_t pw, ph;
    VkFormat pf;
    if (!(vk_fg_active() || ng_running()) || !vk_dlaa_fg_hudless(&pic, &pv, &pw, &ph, &pf) || !pw || !ph || w <= 0 || h <= 0)
        return VK_NULL_HANDLE;
    // The upscaler's output can be smaller than the presented picture: the scene is 75% of it in handheld mode
    // (widescreen.c, 1440x810 in a 1920x1080 picture) and the game's last passes scale it up with the HUD. Taken only
    // at the same size, as before, that frame had no HUD-less picture and was not generated: 5 of 3600 frames generated
    // in handheld mode on an RX 580 (2026-11-11). The crop is taken at the same place of the smaller picture.
    int prect[4] = {rect[0], rect[1], rect[2], rect[3]};
    if ((int)pw != w || (int)ph != h) {
        prect[0] = (int)((int64_t)rect[0] * pw / w), prect[1] = (int)((int64_t)rect[1] * ph / h);
        prect[2] = (int)((int64_t)rect[2] * pw / w), prect[3] = (int)((int64_t)rect[3] * ph / h);
        if (prect[2] <= 0 || prect[3] <= 0) return VK_NULL_HANDLE;
    }
    VkCtx *c = vk_ctx();
    if (g_hl && (g_hl_w != g_sc_ext.width || g_hl_h != g_sc_ext.height || g_hl_fmt != g_sc_fmt)) {
        vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();
        vkDestroyImageView(c->device, g_hl_view, NULL);
        vmaDestroyImage(c->vma, g_hl, g_hl_mem);
        g_hl = VK_NULL_HANDLE;
    }
    if (!g_hl) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g_sc_fmt,
                                .extent = {g_sc_ext.width, g_sc_ext.height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &g_hl, &g_hl_mem, NULL) != VK_SUCCESS) return g_hl = VK_NULL_HANDLE;
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_hl,
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g_sc_fmt,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(c->device, &vi, NULL, &g_hl_view) != VK_SUCCESS) g_hl_view = VK_NULL_HANDLE;
        g_hl_w = g_sc_ext.width, g_hl_h = g_sc_ext.height, g_hl_fmt = g_sc_fmt;
        rt_log("gfx: frame generation's HUD-less picture at %ux%u", g_hl_w, g_hl_h);
    }
    // the steps' work images were just read for the presented picture: they are written again only after that
    sync_barrier(cb, g_hl, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    if (to[2] != (int)g_sc_ext.width || to[3] != (int)g_sc_ext.height) {
        VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, g_hl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        sync_barrier(cb, g_hl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    // the steps' per-slot views of their source: a slot of its own (the presented picture's views stay valid for this
    // command buffer); with HDR the PQ encode as for the presented float picture
    put_picture(cb, slot + FRAMES, pic, pf, (int)pw, (int)ph, prect, g_hl, to, general);
    sync_barrier(cb, g_hl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    return g_hl;
}


// ---------------------------------------------------------------- the frame generation presenter
// Two generators run through it, in every build but the Windows one (none of this is reached there): DLSS Frame
// Generation through NGX (vk_ngxfg_*, NX2SARIA_FRAMEGEN = dlss where Streamline does not run) and AMD FSR 3.1's frame
// generation through the runtime's own passes (vk_fsr3fg.c, NX2SARIA_FRAMEGEN = fsr3; one generated frame per rendered
// frame, at the middle). ng.src names the one in use; the log lines carry its name (ngxfg, fsr3fg). Each frame is composed into an image of its own (ng.bb) instead of a swapchain image.
// At the end of the frame's command buffer DLSS Frame Generation makes the generated frames from it
// (vk_ngxfg_evaluate, the multiplier less one) and the picture is copied aside. The presenter thread then shows them
// in order, each copied into a swapchain image it acquires itself, and the rendered frame last, spaced evenly over the
// time between two rendered frames, as NVIDIA's frame generation guide paces them. Two sets of images take turns: a
// set is written again two frames later, after the presenter's copies from it are done (the timeline ng.copied).
// Every frame goes through the presenter while it runs, so the timelines advance in order.
// Three things froze the picture while this was written, and the code keeps away from each:
// - a frame's own command buffer never waits for a swapchain acquire: on Wayland an acquire's semaphore can signal
//   only after a later present, and every later present would come from that command buffer;
// - a timeline wait is never submitted ahead of its signal on the same queue (ng_wait_signaled);
// - an acquire never blocks while it holds the queue lock the presenter presents with (ng_acquire).
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; } NgImg;
static struct {
    int tried, on, fresh;         // tried: 0 nothing made, 1 the objects below exist, 2 it cannot run here
    int thread_up;                // the presenter thread runs (started once, kept over a release)
    uint32_t w, h;
    VkFormat fmt;
    NgImg bb, out[2][NG_MAX], real[2];
    VkSemaphore rendered, copied;   // timelines: frame n's command buffer submitted/done, the presenter done with frame n
    uint64_t n;                     // frames handed to the presenter
    VkCommandPool pool;
    VkCommandBuffer cb[2][NG_MAX + 1];   // per shown frame: the generated ones, then the rendered one
    VkSemaphore acq[2][NG_MAX + 1];
    PlatMutex m;
    PlatCond cv;
    int busy, pending;
    struct { uint32_t idx; int count, set, shown; uint64_t n, t; VkSwapchainKHR sc; } job;
    uint64_t signaled;              // the last value of ng.copied the presenter submitted a signal for (ng.m)
    uint64_t last_t, interval;      // average time between rendered frames (ns)
    volatile uint64_t presents;     // frames presented while it runs, generated ones included
    int src;                        // the generator in use (NG_DLSS, NG_FSR3), 0 none
    // pacing, on the presenter thread: the time of the last present, and the gaps between presents against the even
    // spacing the presenter aims at (the time between rendered frames over the frames shown for each)
    uint64_t last_present, gaps, gaps_long;
    double gap_dev_ms, gap_max_ms;
} ng = {.m = PLAT_MUTEX_INIT};
enum { NG_DLSS = 1, NG_FSR3 = 2 };
static const char *ng_name(int src) { return src == NG_FSR3 ? "fsr3fg" : "ngxfg"; }

// An acquire that never blocks while it holds the queue lock: the swapchain is shared by the game thread and the
// presenter (both acquire, the presenter presents), and an image is freed only by a present, which needs that lock.
// A blocking acquire under the lock waited for the present it held off: the picture froze (1 s acquire timeouts).
static VkResult ng_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx) {
    VkDevice dev = vk_ctx()->device;
    uint64_t end = plat_now_ns() + timeout;
    for (;;) {
        vk_queue_lock();
        VkResult r = vkAcquireNextImageKHR(dev, sc, 0, sem, VK_NULL_HANDLE, idx);
        vk_queue_unlock();
        if (r != VK_NOT_READY && r != VK_TIMEOUT) return r;
        if (plat_now_ns() >= end) return VK_TIMEOUT;
        plat_sleep_ns(200000);
    }
}

static void ng_drain(void) {   // waits until the presenter has shown everything handed to it
    if (!ng.tried) return;
    plat_mutex_lock(&ng.m);
    while (ng.pending || ng.busy) plat_cond_wait(&ng.cv, &ng.m);
    plat_mutex_unlock(&ng.m);
}

static void ng_sleep_until(uint64_t t) {
    uint64_t now = plat_now_ns();
    if (t > now && t - now < 100000000ull) plat_sleep_ns(t - now);
}

static void ng_present(VkSwapchainKHR sc, uint32_t idx) {
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &g_render_done[idx], .swapchainCount = 1, .pSwapchains = &sc,
                           .pImageIndices = &idx};
    vk_queue_lock();
    VkResult r = vkQueuePresentKHR(vk_ctx()->gfx_queue, &pi);
    vk_queue_unlock();
    vk_device_lost_check(r, "the present of a generated frame");
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) plat_xchg32(&g_dirty, 1);
    __atomic_add_fetch(&ng.presents, 1, __ATOMIC_RELAXED);
}

// One present's gap from the one before against the even spacing (ideal_ns); a gap over 100 ms (a pause, a load) is
// not counted. A gap longer than 1.5 times the spacing is a pacing drop: the eye sees that frame held.
static void ng_pace(uint64_t ideal_ns) {
    uint64_t now = plat_now_ns(), last = ng.last_present;
    ng.last_present = now;
    if (!last || !ideal_ns || now - last > 100000000ull) return;
    double gap = (double)(now - last) / 1e6, ideal = (double)ideal_ns / 1e6, dev = gap > ideal ? gap - ideal : ideal - gap;
    ng.gaps++, ng.gap_dev_ms += dev;
    if (dev > ng.gap_max_ms) ng.gap_max_ms = dev;
    if (gap > 1.5 * ideal) ng.gaps_long++;
}

static void ng_signal_copied(uint64_t v) {   // after everything submitted before it (frame v's copies included)
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = ng.copied, .value = v,
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
    vk_queue_lock();
    vkQueueSubmit2(vk_ctx()->gfx_queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    plat_mutex_lock(&ng.m);
    ng.signaled = v;
    plat_cond_broadcast(&ng.cv);
    plat_mutex_unlock(&ng.m);
}

// A frame's submission waits for ng.copied >= n - 2 on the GPU. That signal must be submitted first: one queue runs
// its submissions in order, and a wait queued ahead of its signal stopped the queue for good (the picture froze).
static void ng_wait_signaled(uint64_t v) {
    plat_mutex_lock(&ng.m);
    while (ng.signaled < v) plat_cond_wait(&ng.cv, &ng.m);
    plat_mutex_unlock(&ng.m);
}

// Acquires a swapchain image, copies img (GENERAL, the swapchain's size and format) into it once frame n's command
// buffer is done (ng.rendered), and presents it. Only the presenter acquires while it runs: the frame's own command
// buffer never waits for an acquire, whose image the compositor may give back only after a later present.
static int ng_show(VkImage img, int set, int slot, uint64_t n) {
    VkCtx *c = vk_ctx();
    uint32_t j = 0;
    VkResult r = ng_acquire(ng.job.sc, 1000000000ull, ng.acq[set][slot], &j);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        static int told;
        if (told++ < 5) rt_log("%s: presenter acquire failed (%d)", ng_name(ng.src), (int)r);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_TIMEOUT) plat_xchg32(&g_dirty, 1);
        return 0;
    }
    VkCommandBuffer cb = ng.cb[set][slot];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    sync_barrier(cb, g_sc_img[j], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {ng.w, ng.h, 1}};
    vkCmdCopyImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, g_sc_img[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    sync_barrier(cb, g_sc_img[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    vkEndCommandBuffer(cb);
    VkSemaphoreSubmitInfo wait[2] = {
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = ng.acq[set][slot], .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT},
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = ng.rendered, .value = n,
         .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT}};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_render_done[j],
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 2, .pWaitSemaphoreInfos = wait,
                        .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &sig};
    vk_queue_lock();
    r = vkQueueSubmit2(c->gfx_queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    if (r != VK_SUCCESS) { vk_device_lost_check(r, "the submit of a generated frame"); return 0; }
    ng_present(ng.job.sc, j);
    return 1;
}

static void ng_run(void) {
    VkCtx *c = vk_ctx();
    uint64_t t = ng.job.t, n = ng.job.n;
    int set = ng.job.set, count = ng.job.count;
    ng.job.shown = 0;
    if (ng.last_t && t > ng.last_t) {
        uint64_t d = t - ng.last_t;
        if (d > 2000000ull && d < 100000000ull) ng.interval = ng.interval ? (ng.interval * 7 + d) / 8 : d;
    }
    ng.last_t = t;
    uint64_t step = ng.interval / (uint64_t)(count + 1);
    if (n > 2) {   // the command buffers and acquire semaphores of this set: their last use (frame n - 2) is over
        uint64_t v = n - 2;
        VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &ng.copied,
                                  .pValues = &v};
        vkWaitSemaphores(c->device, &wi, 2000000000ull);
    }
    for (int i = 0; i < count; i++) {
        if (i) ng_sleep_until(t + (uint64_t)i * step);   // the first one at once: the least latency
        if (!ng_show(ng.out[set][i].img, set, i, n)) break;
        ng_pace(step);
        ng.job.shown = i + 1;
    }
    static uint64_t jobs, gen_ok, gen_want;
    jobs++, gen_want += (uint64_t)count;
    if (count) ng_sleep_until(t + (uint64_t)count * step);
    if (ng_show(ng.real[set].img, set, NG_MAX, n)) ng_pace(step);   // the rendered frame, last
    ng_signal_copied(n);   // after the copies out of this set
    gen_ok += (uint64_t)ng.job.shown;
    if (jobs == 60 || jobs % 1200 == 0) {
        rt_log("%s: presenter: %llu rendered frames, %llu of %llu generated frames shown, %llu presents; %.2f ms between "
               "rendered frames; pacing: %llu gaps between presents, %.2f ms from the even spacing on average (%.2f at "
               "most), %llu longer than 1.5 times it",
               ng_name(ng.src), (unsigned long long)jobs, (unsigned long long)gen_ok, (unsigned long long)gen_want,
               (unsigned long long)__atomic_load_n(&ng.presents, __ATOMIC_RELAXED), (double)ng.interval / 1e6,
               (unsigned long long)ng.gaps, ng.gaps ? ng.gap_dev_ms / (double)ng.gaps : 0.0, ng.gap_max_ms,
               (unsigned long long)ng.gaps_long);
        ng.gap_max_ms = 0;   // the most of each period
        if (ng.src == NG_FSR3) {   // the generator's own counts, read here on the presenter thread: a few frames old
            VkFsr3fgStats st;
            vk_fsr3fg_stats(&st);
            rt_log("fsr3fg: %llu frames recorded, %llu generated, %llu history starts (cuts, resizes, the menu: no "
                   "frame shown), %llu frames without depth and motion, %llu generated without the picture without the "
                   "HUD; GPU %.2f ms optical flow + %.2f ms interpolation a frame (over %llu frames); %.1f MiB",
                   (unsigned long long)st.frames, (unsigned long long)st.generated, (unsigned long long)st.cuts,
                   (unsigned long long)st.no_inputs, (unsigned long long)st.no_hudless, st.gpu_ms_of, st.gpu_ms_fi,
                   (unsigned long long)st.gpu_samples, st.vram / 1048576.0);
        }
    }
}

static void ng_thread(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&ng.m);
        while (!ng.pending) {
            if (!plat_cond_wait_ms(&ng.cv, &ng.m, 1500) && ng.on && ng.n) {   // nothing for 1.5 s: where the GPU stands
                uint64_t rv = 0, cv = 0;
                vkGetSemaphoreCounterValue(vk_ctx()->device, ng.rendered, &rv);
                vkGetSemaphoreCounterValue(vk_ctx()->device, ng.copied, &cv);
                static int told;
                if (told++ < 3)
                    rt_log("%s: watchdog: no frame for 1.5 s; frames handed %llu, copied signal submitted %llu; GPU rendered %llu copied %llu",
                           ng_name(ng.src), (unsigned long long)ng.n, (unsigned long long)ng.signaled,
                           (unsigned long long)rv, (unsigned long long)cv);
            }
        }
        ng.pending = 0, ng.busy = 1;
        plat_cond_broadcast(&ng.cv);
        plat_mutex_unlock(&ng.m);
        ng_run();
        plat_mutex_lock(&ng.m);
        ng.busy = 0;
        plat_cond_broadcast(&ng.cv);
        plat_mutex_unlock(&ng.m);
    }
}

static void ng_free_image(NgImg *im) {
    VkCtx *c = vk_ctx();
    if (im->view) vkDestroyImageView(c->device, im->view, NULL);
    if (im->img) vmaDestroyImage(c->vma, im->img, im->mem);
    memset(im, 0, sizeof *im);
}

static int ng_image(NgImg *im, uint32_t w, uint32_t h, VkFormat f) {
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &im->img, &im->mem, NULL) != VK_SUCCESS) return im->img = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &im->view) == VK_SUCCESS;
}

// One line at the first present and at each change: whether AMD FSR 3.1's frame generation through the runtime's passes
// runs, at what multiplier, or why not (the setting, an HDR swapchain, a device that cannot run the passes, a failure).
static void fsr3fg_state_line(int src, int fsr_set) {
    static char last[160];
    char now[160];
    if (!vk_fsr3fg_build()) return;
    if (!fsr_set) snprintf(now, sizeof now, "frame generation is %s: FSR 3.1's frame generation does not run",
                           cfg_value("NX2SARIA_FRAMEGEN"));
    else if (src == NG_FSR3) snprintf(now, sizeof now, "on: AMD FSR 3.1 frame generation through the runtime's passes, "
                                                       "2x (one generated frame per rendered frame), no AMD library");
    else if (vk_fsr3fg_failed()) snprintf(now, sizeof now, "set, but off: it failed earlier in this run (see above)");
    else if (!vk_fsr3fg_available()) snprintf(now, sizeof now, "set, but off: this GPU cannot run its passes (see above)");
    else if (g_sc_hdr) snprintf(now, sizeof now, "set, but off while HDR is on: it generates SDR pictures only");
    else if (vk_fg_active() || vk_dlssg_active()) snprintf(now, sizeof now, "set, but off: another frame generation "
                                                                             "holds the swapchain");
    else snprintf(now, sizeof now, "set, but off: the presenter could not start (see above)");
    if (!strcmp(now, last)) return;
    snprintf(last, sizeof last, "%s", now);
    rt_log("fsr3fg: %s", now);
}

// The presenter and the images for the current swapchain (g_pl held). 0: frames go out the plain way.
static int ng_ready(void) {
    // the Windows build has neither (AMD's library and Streamline make their own swapchains there); NGX's needs a run
    // that started with DLSS upscaling (vk_ngxfg_wanted), FSR 3.1's the device's passes (vk_fsr3fg_init)
    if (!vk_ngxfg_wanted() && !vk_fsr3fg_build()) return 0;
    const int ngx = vk_ngxfg_wanted() && vk_ngxfg_on();
    const int fsr_set = !ngx && vk_fsr3fg_build() && !strcmp(cfg_value("NX2SARIA_FRAMEGEN"), "fsr3");
    const int others = !g_sc_hdr && !vk_fg_active() && !vk_dlssg_active();
    const int fsr = fsr_set && others && vk_fsr3fg_on() && vk_fsr3fg_init() == 1;
    int want = (ngx && others) || fsr;
    const int src = !want ? 0 : ngx ? NG_DLSS : NG_FSR3;
    if (!vk_fg_wanted()) vk_dlaa_hudless_wanted = want;   // the HUD-less picture both tell the HUD apart by
    if (!want) {   // off (the menu switched it off live): what the presenter holds goes out first
        if (ng.on) {
            ng_drain(), ng.on = 0;
            rt_log("%s: frame generation off", ng_name(ng.src));
            if (ng.src == NG_FSR3) vk_fsr3fg_idle();
        }
        fsr3fg_state_line(0, fsr_set);
        return 0;
    }
    if (ng.on && ng.src != src) {   // the menu switched from one generator to the other: each starts anew
        ng_drain();
        if (ng.src == NG_FSR3) vk_fsr3fg_idle();
        rt_log("%s: frame generation now through %s", ng_name(ng.src), ng_name(src));
        ng.on = 0;   // the images again, for the new generator's count of generated frames
    }
    ng.src = src;
    fsr3fg_state_line(src, fsr_set);
    if (ng.on && ng.w == g_sc_ext.width && ng.h == g_sc_ext.height && ng.fmt == g_sc_fmt) return 1;
    VkCtx *c = vk_ctx();
    if (!ng.tried) {
        ng.tried = 1;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(c->phys, g_sc_fmt, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            rt_log("%s: the swapchain format %d is no storage format here: frame generation off", ng_name(src),
                   (int)g_sc_fmt);
            return ng.on = 0, ng.tried = 2, 0;
        }
        if (!ng.thread_up) plat_cond_init(&ng.cv);
        VkSemaphoreTypeCreateInfo tl = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tl};
        // The presenter's semaphores, command pool and command buffers. One that is not made turns frame generation
        // off for the run, as the checks above do, with what was made released: the presenter would otherwise wait on
        // or submit null handles.
        VkResult rr = vkCreateSemaphore(c->device, &tci, NULL, &ng.rendered);
        const char *what = "vkCreateSemaphore";
        if (rr != VK_SUCCESS) ng.rendered = VK_NULL_HANDLE;
        if (rr == VK_SUCCESS && (rr = vkCreateSemaphore(c->device, &tci, NULL, &ng.copied)) != VK_SUCCESS)
            ng.copied = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
        if (rr == VK_SUCCESS && (rr = vkCreateCommandPool(c->device, &pci, NULL, &ng.pool)) != VK_SUCCESS)
            ng.pool = VK_NULL_HANDLE, what = "vkCreateCommandPool";
        for (int s = 0; s < 2 && rr == VK_SUCCESS; s++) {
            VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = ng.pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NG_MAX + 1};
            if ((rr = vkAllocateCommandBuffers(c->device, &ai, ng.cb[s])) != VK_SUCCESS) what = "vkAllocateCommandBuffers";
            for (int i = 0; i <= NG_MAX && rr == VK_SUCCESS; i++)
                if ((rr = vkCreateSemaphore(c->device, &semi, NULL, &ng.acq[s][i])) != VK_SUCCESS)
                    ng.acq[s][i] = VK_NULL_HANDLE, what = "vkCreateSemaphore";
        }
        if (rr != VK_SUCCESS) {
            static volatile int32_t n_ng_failed;   // once a run (ng.tried = 2), counted all the same
            rt_log("gfx: %s failed (%d), %d times: the %s presenter's; frame generation off", what, (int)rr,
                   plat_inc32(&n_ng_failed), ng_name(src));
            if (ng.rendered) vkDestroySemaphore(c->device, ng.rendered, NULL);
            if (ng.copied) vkDestroySemaphore(c->device, ng.copied, NULL);
            for (int s = 0; s < 2; s++)
                for (int i = 0; i <= NG_MAX; i++)
                    if (ng.acq[s][i]) vkDestroySemaphore(c->device, ng.acq[s][i], NULL), ng.acq[s][i] = VK_NULL_HANDLE;
            if (ng.pool) vkDestroyCommandPool(c->device, ng.pool, NULL);   // its command buffers with it
            ng.rendered = ng.copied = VK_NULL_HANDLE, ng.pool = VK_NULL_HANDLE;
            if (src == NG_FSR3) vk_fsr3fg_idle();
            return ng.on = 0, ng.tried = 2, 0;
        }
        if (!ng.thread_up && !plat_thread_spawn(ng_thread, NULL, "fg present", 0)) {
            rt_log("%s: the presenter thread did not start: frame generation off", ng_name(src));
            return ng.on = 0, ng.tried = 2, 0;
        }
        ng.thread_up = 1;
    }
    if (ng.tried == 2) return 0;
    ng_drain();
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    ng_free_image(&ng.bb);
    for (int s = 0; s < 2; s++) {
        ng_free_image(&ng.real[s]);
        for (int i = 0; i < NG_MAX; i++) ng_free_image(&ng.out[s][i]);
    }
    uint32_t w = g_sc_ext.width, h = g_sc_ext.height;
    // FSR 3.1 generates one frame per rendered frame: one image a set, where DLSS Frame Generation may need NG_MAX
    const int nout = src == NG_FSR3 ? 1 : NG_MAX;
    int ok = ng_image(&ng.bb, w, h, g_sc_fmt);
    for (int s = 0; s < 2 && ok; s++) {
        ok = ng_image(&ng.real[s], w, h, g_sc_fmt);
        for (int i = 0; i < nout && ok; i++) ok = ng_image(&ng.out[s][i], w, h, g_sc_fmt);
    }
    if (!ok) {
        rt_log("%s: frame generation images (%ux%u) could not be made: frame generation off", ng_name(src), w, h);
        if (src == NG_FSR3) vk_fsr3fg_idle();
        return ng.on = 0;
    }
    ng.w = w, ng.h = h, ng.fmt = g_sc_fmt, ng.fresh = 1, ng.on = 1;
    vk_ngxfg_reset();
    vk_fsr3fg_reset();
    rt_log("%s: presenter ready at %ux%u (up to %dx)", ng_name(src), w, h, src == NG_FSR3 ? 2 : vk_ngxfg_max() + 1);
    return 1;
}

// NX2SARIA_FG_MULT: 2 to 6, or dynamic: only the frames needed to reach the display's refresh rate at the rendered frame
// rate (240 Hz at 60 fps: 4x), between 2x and what the GPU allows, as NVIDIA describes its dynamic multi frame
// generation; a new multiplier must hold for 30 frames before it is taken, so it does not flicker.
static int ng_multiplier(int max) {
    const char *v = cfg_value("NX2SARIA_FG_MULT");
    int m;
    if (!strcmp(v, "dynamic")) {
        static int cur = 2, cand, held;
        double hz = gfx_window_refresh_hz(), ms = (double)ng.interval / 1e6;
        int fit = hz > 0 && ms > 0 ? (int)(hz * ms / 1000.0 + 0.1) : 2;   // shown frames per rendered one that fit
        if (fit < 2) fit = 2;
        if (fit > max + 1) fit = max + 1;
        if (fit != cur) {
            held = fit == cand ? held + 1 : 0, cand = fit;
            if (held >= 30) cur = fit, held = 0;
        } else held = 0;
        m = cur;
    } else {
        m = atoi(v);
    }
    if (m < 2) m = 2;
    if (m > max + 1) m = max + 1;
    return m;
}

// Records the copy of the frame's picture and the generator's frames (the end of present_image's command buffer, dst
// in layout *last; pic: the picture's rectangle in it). Returns the number of generated frames, -1 when the frame goes
// out the plain way.
static int ng_record(VkCommandBuffer cb, VkImage dst, VkImageLayout *last, VkPipelineStageFlags2 *stage, VkAccessFlags2 *write,
                     VkImage hudless, int *set, int gen, const int pic[4]) {
    if (!ng.on) return -1;
    if (ng.fresh) {   // every image to GENERAL, once
        NgImg *all[1 + 2 + 2 * NG_MAX];
        int k = 0;
        all[k++] = &ng.bb;
        for (int s = 0; s < 2; s++) {
            all[k++] = &ng.real[s];
            for (int i = 0; i < NG_MAX; i++)
                if (ng.out[s][i].img) all[k++] = &ng.out[s][i];   // FSR 3.1's sets have one
        }
        for (int i = 0; i < k; i++)
            sync_barrier(cb, all[i]->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        ng.fresh = 0;
    }
    *set = (int)((ng.n + 1) & 1);
    // the frame's picture is in dst (ng.bb): to GENERAL for DLSS-G and the copy into this set's rendered-frame image
    sync_barrier(cb, dst, *last, VK_IMAGE_LAYOUT_GENERAL, *stage, *write, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT);
    *last = VK_IMAGE_LAYOUT_GENERAL, *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, *write = 0;
    int made = 0;
    if (ng.src == NG_FSR3) vk_fsr3fg_presents(__atomic_load_n(&ng.presents, __ATOMIC_RELAXED));
    const int menu = menu_is_open();
    if (!gen || menu) {
        vk_ngxfg_reset();   // generation pauses while the menu is open
        vk_fsr3fg_reset();
        if (ng.src == NG_FSR3) vk_fsr3fg_paused(menu);
    } else if (ng.src == NG_FSR3) {
        VkNgxImage bb = {ng.bb.img, ng.bb.view, ng.fmt, ng.w, ng.h};
        VkNgxImage hl = {hudless, g_hl_view, g_hl_fmt, g_hl_w, g_hl_h};
        VkNgxImage out = {ng.out[*set][0].img, ng.out[*set][0].view, ng.fmt, ng.w, ng.h};
        int hl_ok = hudless && g_hl_w == ng.w && g_hl_h == ng.h && g_hl_fmt == ng.fmt;
        made = vk_fsr3fg_evaluate(cb, &bb, hl_ok ? &hl : NULL, &out, pic);
    } else {
        int max = vk_ngxfg_max(), want = ng_multiplier(max) - 1;
        VkNgxImage bb = {ng.bb.img, ng.bb.view, ng.fmt, ng.w, ng.h};
        VkNgxImage hl = {hudless, g_hl_view, g_hl_fmt, g_hl_w, g_hl_h};
        VkNgxImage out[NG_MAX], real = {ng.real[*set].img, ng.real[*set].view, ng.fmt, ng.w, ng.h};
        for (int i = 0; i < NG_MAX; i++) out[i] = (VkNgxImage){ng.out[*set][i].img, ng.out[*set][i].view, ng.fmt, ng.w, ng.h};
        int hl_ok = hudless && g_hl_w == ng.w && g_hl_h == ng.h && g_hl_fmt == ng.fmt;
        made = vk_ngxfg_evaluate(cb, &bb, hl_ok ? &hl : NULL, out, want, &real);
    }
    // the rendered frame for the presenter (ng.bb is drawn again by the next frame)
    sync_barrier(cb, ng.real[*set].img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {ng.w, ng.h, 1}};
    vkCmdCopyImage(cb, dst, VK_IMAGE_LAYOUT_GENERAL, ng.real[*set].img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    sync_barrier(cb, ng.real[*set].img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    *stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, *write = 0;
    return made;
}

// gfx_window_release_vk, the device idle: the presenter shows what it holds, then its images, semaphores and command
// buffers go with the device. The thread stays and waits; a later device makes the objects again (ng_ready).
static void ng_release(void) {
    if (!ng.tried) return;
    ng_drain();
    VkCtx *c = vk_ctx();
    vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();   // what the presenter submitted while it drained
    ng_free_image(&ng.bb);
    for (int s = 0; s < 2; s++) {
        ng_free_image(&ng.real[s]);
        for (int i = 0; i < NG_MAX; i++) ng_free_image(&ng.out[s][i]);
    }
    if (ng.rendered) vkDestroySemaphore(c->device, ng.rendered, NULL);
    if (ng.copied) vkDestroySemaphore(c->device, ng.copied, NULL);
    for (int s = 0; s < 2; s++)
        for (int i = 0; i <= NG_MAX; i++)
            if (ng.acq[s][i]) vkDestroySemaphore(c->device, ng.acq[s][i], NULL), ng.acq[s][i] = VK_NULL_HANDLE;
    if (ng.pool) vkDestroyCommandPool(c->device, ng.pool, NULL);   // its command buffers with it
    ng.rendered = ng.copied = VK_NULL_HANDLE, ng.pool = VK_NULL_HANDLE;
    plat_mutex_lock(&ng.m);
    ng.on = ng.tried = 0, ng.n = ng.signaled = 0;   // new timelines start at 0
    plat_mutex_unlock(&ng.m);
}

static uint64_t ng_presented(void) { return ng.on ? __atomic_load_n(&ng.presents, __ATOMIC_RELAXED) : 0; }
static int ng_running(void) { return ng.on; }

// Hands the submitted frame to the presenter (it shows count generated frames, then swapchain image idx).
static void ng_hand(uint32_t idx, int count, int set, uint64_t n) {
    plat_mutex_lock(&ng.m);
    while (ng.pending) plat_cond_wait(&ng.cv, &ng.m);
    ng.job.idx = idx, ng.job.count = count < 0 ? 0 : count, ng.job.set = set, ng.job.n = n, ng.job.t = plat_now_ns();
    ng.job.sc = g_sc;
    ng.pending = 1;
    plat_cond_broadcast(&ng.cv);
    plat_mutex_unlock(&ng.m);
}

static void present_image(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4], int defer) {
    int general = layout == VK_IMAGE_LAYOUT_GENERAL;
    {   // the presented image's format, when it changes (an HDR picture comes in another one)
        static VkFormat last = VK_FORMAT_UNDEFINED;
        if (src && fmt != last) last = fmt, rt_log("gfx: the game presents %dx%d images of format %d", w, h, (int)fmt);
    }
    if (vk_device_is_lost()) return;   // the run is ending (vk_device.c vk_device_lost_check): nothing touches the device
    gfx_window_start();
    VkCtx *c = vk_ctx();
    plat_rmutex_lock(&g_pl);
    flip_locked();
    if (!g_sc || plat_cas32(&g_dirty, 0, 0)) {
        if (!wsys_minimized() && !create_swapchain() && !g_sc) goto out;
    }
    if (wsys_minimized() || !g_sc) goto out;

    unsigned slot = g_frame % FRAMES;
    VkResult fw = vkWaitForFences(c->device, 1, &g_fence[slot], VK_TRUE, 5000000000ull);
    if (fw != VK_SUCCESS) {
        // A lost device ends the run here as anywhere else; a hung GPU times out at every present, so the line is
        // kept to the first 8 and each doubling.
        vk_device_lost_check(fw, "the present slot's fence wait");
        static uint32_t n_fw;
        if (++n_fw <= 8 || !(n_fw & (n_fw - 1)))
            rt_log("gfx: present fence wait %s (%d), the frame is skipped: %u times so far",
                   fw == VK_TIMEOUT ? "timed out" : "failed", (int)fw, n_fw);
        goto out;
    }
    pts_read(c, slot);   // the GPU timeline (vk_gtl_note): this slot's last present is done
    uint32_t idx = 0;
    // NGX frame generation: the frame is composed into ng.bb and the presenter shows it (ng_show); no acquire here
    const int ngp = ng_ready();
    vk_lowlat_fg_presents(ngp);   // the presenter presents several times a frame: a driver's low latency steps aside
    VkResult r = ngp               ? VK_SUCCESS
               : vk_dlssg_active() ? vk_dlssg_acquire(c->device, g_sc, 1000000000ull, g_acquire[slot], &idx)
               : vk_fg_active()    ? vk_fg_acquire(g_sc, 1000000000ull, g_acquire[slot], &idx)
                                   : vkAcquireNextImageKHR(c->device, g_sc, 1000000000ull, g_acquire[slot], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        plat_xchg32(&g_dirty, 1);
        if (!create_swapchain()) goto out;
        r = vk_dlssg_active() ? vk_dlssg_acquire(c->device, g_sc, 1000000000ull, g_acquire[slot], &idx)
          : vk_fg_active()    ? vk_fg_acquire(g_sc, 1000000000ull, g_acquire[slot], &idx)
                              : vkAcquireNextImageKHR(c->device, g_sc, 1000000000ull, g_acquire[slot], VK_NULL_HANDLE, &idx);
    }
    vk_device_lost_check(r, "the acquire of a swapchain image");
    // not ready: skip the frame. Acquires that keep timing out (1 s each) mean a swapchain nothing will free an
    // image of again: three in a row rebuild it, where the picture used to stop for good.
    static uint32_t n_timeouts;
    if (r == VK_TIMEOUT || r == VK_NOT_READY) {
        if (++n_timeouts >= 3) {
            rt_log("gfx: no swapchain image for %u acquires (%d): the swapchain is made again", n_timeouts, (int)r);
            n_timeouts = 0;
            plat_xchg32(&g_dirty, 1);
        }
    } else {
        n_timeouts = 0;
    }
    if (r < 0 && r != VK_ERROR_OUT_OF_DATE_KHR) {   // an error (the surface lost, say), logged once per code
        static VkResult seen[8];
        static int nseen;
        int k = 0;
        while (k < nseen && seen[k] != r) k++;
        if (k == nseen && nseen < 8) {
            seen[nseen++] = r;
            rt_log("gfx: the acquire of a swapchain image failed (%d%s): the frame is skipped", (int)r,
                   r == VK_ERROR_SURFACE_LOST_KHR ? ", the window's surface is lost" : "");
        }
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) goto out;
    if (r == VK_SUBOPTIMAL_KHR) plat_xchg32(&g_dirty, 1);
    vkResetFences(c->device, 1, &g_fence[slot]);
    if (g_sc_hdr) vk_hdr_metadata(g_sc, 0);   // a changed peak or white goes to the display
    if (g_th_pending[slot]) {   // that fence has signalled: the thumbnail its submission wrote is complete
        uint64_t done = g_th_pending[slot];
        g_th_tag[done % (uint64_t)g_th_n] = done;
        g_th_pending[slot] = 0;
        if (flight_flash_on && g_th_ptr) thumb_cells(done);
    }

    // The game's crop (nvnWindowSetCrop, guest values) cut to the image without overflow (gfx_guard.h gfx_crop_clip);
    // a crop outside the image is the whole image, not an inverted blit rectangle. A crop that had to be cut or
    // replaced is logged with its values, as gfx_fail_log_due allows.
    int src_rect[4];
    int crop_how = gfx_crop_clip(crop, w, h, src_rect);
    int cx = src_rect[0], cy = src_rect[1], cw = src_rect[2], ch = src_rect[3];
    if (crop_how == GFX_CROP_CUT || crop_how == GFX_CROP_OUTSIDE) {
        static volatile int32_t n_crop;
        uint32_t k = (uint32_t)plat_inc32(&n_crop);
        if (gfx_fail_log_due(k))
            rt_log("gfx: the game's crop %d,%d %dx%d %s the %dx%d image: %d,%d %dx%d presented, %u times", crop[0],
                   crop[1], crop[2], crop[3], crop_how == GFX_CROP_CUT ? "reaches past" : "lies outside", w, h, cx, cy,
                   cw, ch, k);
    }
    window_settings_live();
    cam_speed_tick();
    // The menu overlay: laid out before recording (its first frame starts the ImGui back end and uploads the font)
    const char *menu_shot = NULL;
    MenuTarget mt = {c->instance, c->phys, c->device, c->gfx_family, c->gfx_queue,
                     g_sc_hdr ? VK_FORMAT_R8G8B8A8_UNORM : g_sc_fmt, g_sc_ext.width,
                     g_sc_ext.height, cw, ch, g_scaler};
    // without a source image (gfx_present_boot): a black frame with the boot screen
    VkImageView dst_view = ngp ? ng.bb.view : g_sc_view[idx];
    int overlay = dst_view &&
                  (src ? menu_overlay_frame(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), &mt, &menu_shot)
                       : menu_boot_frame(&mt));
    // NX2SARIA_SHOT_EVERY=<s> with NX2SARIA_SHOT_DIR=<dir>: the presented frame as shot_<n>.png every s seconds
    // (a check of what the window shows, for runs nobody watches)
    static char shot_path[1100];
    {
        static double every = -1;
        static uint64_t next_ns;
        static int n;
        if (every < 0) {
            const char *e = cfg_get("NX2SARIA_SHOT_EVERY");
            every = e ? atof(e) : 0;
            const char *after = cfg_get("NX2SARIA_SHOT_AFTER");   // seconds after the start before the first shot
            next_ns = plat_now_ns() + (uint64_t)((every + (after ? atof(after) : 0)) * 1e9);
        }
        const char *dir = cfg_get("NX2SARIA_SHOT_DIR");
        if (!menu_shot && every > 0 && dir && plat_now_ns() >= next_ns && n < 200) {
            snprintf(shot_path, sizeof shot_path, "%s/shot_%03d.png", dir, n++);
            menu_shot = shot_path;
            next_ns = plat_now_ns() + (uint64_t)(every * 1e9);
        }
    }
    int grab = src && g_grab_cb;   // a bug capture asked for this frame (vk_present_grab)
    int cap = (g_capture || menu_shot || grab) && ensure_capture_buffer();
    int thumb = src && g_th_n && ensure_thumbs();

    VkCommandBuffer cb = g_cb[slot];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    pts_begin(c, cb, slot);
    uint64_t crumb = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    vk_crumb(cb, VK_CRUMB_PRESENT, crumb);
    VkImage dst = ngp ? ng.bb.img : g_sc_img[idx];
    sync_barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int to[4];   // where the picture goes in the swapchain image: {x, y, w, h}
    gfx_fit_rect(cw, ch, (int)g_sc_ext.width, (int)g_sc_ext.height, to);
    if (src && g_sc_ext.width && g_sc_ext.height) {
        for (int i = 0; i < 4; i++) g_pic[i] = (float)to[i] / (float)(i & 1 ? g_sc_ext.height : g_sc_ext.width);
        __atomic_store_n(&g_pic_ok, 1, __ATOMIC_RELEASE);
    }
    const int rect[4] = {cx, cy, cw, ch};
    if (!src || to[2] != (int)g_sc_ext.width || to[3] != (int)g_sc_ext.height) {   // black bars beside or above and below it
        VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        sync_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    int fsr = put_picture(cb, slot, src, fmt, w, h, rect, dst, to, general);
    if (src) g_scaler = fsr == 2 ? 3 : fsr ? 2 : cw != to[2] || ch != to[3];
    // Frame generation without the HUD in its generated frames: the picture as it would be without the HUD (vk_dlaa.c,
    // DLSS's or FSR 3.1's output before the HUD was drawn over it) goes through the same steps into an image of the
    // swapchain's size, which frame generation compares with the presented one to find the HUD and leave it unwarped
    VkImage hudless = src ? hudless_record(cb, slot, w, h, rect, to, general) : VK_NULL_HANDLE;
    static int show_hl = -1;   // NX2SARIA_FG_HUDLESS_SHOW=1 (test): the HUD-less picture on screen instead
    if (show_hl < 0) show_hl = cfg_get("NX2SARIA_FG_HUDLESS_SHOW") && !strcmp(cfg_get("NX2SARIA_FG_HUDLESS_SHOW"), "1");
    if (hudless && show_hl) {
        VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {g_sc_ext.width, g_sc_ext.height, 1}};
        vkCmdCopyImage(cb, hudless, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        sync_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    if (!g_sc_hdr && src) movie_overlay(cb, slot, dst);   // a movie frame over the game image (SDR swapchains)
    // last: the image's layout, and the stage and access of its last write, as each step leaves it
    VkImageLayout last = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkPipelineStageFlags2 last_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkAccessFlags2 last_write = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    // Centered UI and wide movies/fades use the middle 16:9. Clear the outside margins after composition, leaving the
    // HUD-less frame-generation source and menu pointer (g_pic) at the whole picture's coordinates. Tall screens fill
    // height.
    double x_margin = 0, y_margin = 0;
    if (src && dst_view) wide_menu_bars(&x_margin, &y_margin);
    int side_w = (int)lround(to[2] * x_margin), top_h = (int)lround(to[3] * y_margin);
    VkClearRect bar_rects[4];
    uint32_t bar_count = 0;
    if (side_w > 0 && 2 * side_w < to[2]) {
        bar_rects[bar_count++] = (VkClearRect){{{to[0], to[1]}, {(uint32_t)side_w, (uint32_t)to[3]}}, 0, 1};
        bar_rects[bar_count++] = (VkClearRect){{{to[0] + to[2] - side_w, to[1]}, {(uint32_t)side_w, (uint32_t)to[3]}}, 0, 1};
    }
    if (top_h > 0 && 2 * top_h < to[3]) {
        uint32_t x = (uint32_t)to[0] + (uint32_t)side_w;
        uint32_t width = (uint32_t)to[2] - 2 * (uint32_t)side_w;
        bar_rects[bar_count++] = (VkClearRect){{{x, to[1]}, {width, (uint32_t)top_h}}, 0, 1};
        bar_rects[bar_count++] = (VkClearRect){{{x, to[1] + to[3] - top_h}, {width, (uint32_t)top_h}}, 0, 1};
    }
    if (bar_count) {
        sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, last_stage, last_write,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst_view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_sc_ext}, .layerCount = 1,
                              .colorAttachmentCount = 1, .pColorAttachments = &ca};
        VkClearAttachment black = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                                   .clearValue = {.color = {.float32 = {0, 0, 0, 1}}}};
        vkCmdBeginRendering(cb, &ri);
        vkCmdClearAttachments(cb, 1, &black, bar_count, bar_rects);
        vkCmdEndRendering(cb);
        last = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        last_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    // HDR10: the menu goes into vk_hdr.c's RGBA8 image, which is then blended onto the swapchain image at the HUD's white
    VkImageView menu_view = overlay && g_sc_hdr ? vk_hdr_ui_target(cb, g_sc_ext.width, g_sc_ext.height) : VK_NULL_HANDLE;
    if (overlay && g_sc_hdr && !menu_view) overlay = 0;
    if (overlay) {
        sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, last_stage, last_write,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                        .imageView = menu_view ? menu_view : dst_view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = menu_view ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_sc_ext}, .layerCount = 1,
                              .colorAttachmentCount = 1, .pColorAttachments = &ca};
        vkCmdBeginRendering(cb, &ri);
        menu_overlay_draw(cb);
        vkCmdEndRendering(cb);
        if (menu_view) vk_hdr_ui_compose(cb, dst_view, g_sc_fmt, g_sc_ext.width, g_sc_ext.height);
        last = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        last_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (cap) {
        sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, last_stage, last_write,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {g_sc_ext.width, g_sc_ext.height, 1}};
        vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_cap_buf, 1, &bc);
        last = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        last_write = 0;
    }
    if (thumb) {
        if (last != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, last_stage, last_write,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        last = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        last_write = 0;
        record_thumb(cb, dst, slot);
    }
    if (src) vk_fg_frame(cb, g_sc, g_sc_fmt, g_sc_ext, hudless, g_hl_view, to);   // frame generation: this frame's preparation
    if (src) output_rate();
    if (src) vk_dlssg_frame(cb, g_sc_ext);                // DLSS Frame Generation: constants and tags (vk_dlssg.c)
    // through the presenter: the picture stays in ng.bb, and DLSS Frame Generation's frames and the copy for the
    // presenter are recorded; otherwise the swapchain image is ready to present
    int ng_set = 0;
    const int ng_count = ngp ? ng_record(cb, dst, &last, &last_stage, &last_write, hudless, &ng_set, src != VK_NULL_HANDLE, to) : -1;
    const int via_ng = ng_count >= 0;
    if (!via_ng) sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, last_stage, last_write, VK_PIPELINE_STAGE_2_NONE, 0);
    vk_crumb(cb, VK_CRUMB_PRESENT_END, crumb);
    pts_end(cb, slot);
    vkEndCommandBuffer(cb);

    VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_acquire[slot],
                                  .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_render_done[idx],
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait,
                        .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &sig};
    // A driver's low latency (vk_lowlat.c): the game frame this blit shows, and NVIDIA's tag with its number
    VkLatencySubmissionPresentIdNV ll_tag;
    int ll_tagged = 0;
    const uint64_t ll_frame = src ? vk_lowlat_blit_frame(&ll_tag, &ll_tagged) : 0;
    if (ll_tagged) si.pNext = &ll_tag;
    const uint64_t ng_n = ng.n + 1;   // through the presenter: this frame's number on its timelines
    if (via_ng) {
        // No acquire to wait for. The frame writes one of the presenter's two image sets: it waits on the GPU until
        // the presenter is done with the frame before last, which used that set, and that signal is submitted first
        // (ng_wait_signaled). It signals the timeline the presenter's copies wait for instead of a render-done semaphore.
        wait = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = ng.copied,
                                       .value = ng_n > 2 ? ng_n - 2 : 0, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        sig = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = ng.rendered,
                                      .value = ng_n, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        si.waitSemaphoreInfoCount = ng_n > 2 ? 1u : 0u;
        if (ng_n > 2) ng_wait_signaled(ng_n - 2);
    }
    vk_queue_lock();
    pts_sub[slot] = plat_now_ns();
    r = vkQueueSubmit2(c->gfx_queue, 1, &si, g_fence[slot]);
    vk_queue_unlock();
    if (r != VK_SUCCESS) pts_on[slot] = 0;
    if (r == VK_SUCCESS && via_ng) {   // the presenter shows the generated frames, then this one
        ng.n = ng_n;
        ng_hand(idx, ng_count, ng_set, ng_n);
    } else if (r == VK_SUCCESS) {
        g_flip_on = 1;
        g_flip_idx = idx;
        g_flip_frame = ll_frame;
        if (!defer) flip_locked();
    } else {
        vk_device_lost_check(r, "the present's submit");
        rt_log("gfx: present submit failed (%d)", (int)r);
        // The fence was reset after the acquire and the acquire's semaphore is signalled: left so, this slot's fence
        // wait would time out (5 s) at every later present and the picture stop for good. An empty batch takes the
        // semaphore and signals the fence; the acquired image goes back with the swapchain, made again next frame.
        // (through the presenter nothing was acquired: the batch only signals the fence)
        VkSubmitInfo2 settle = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = via_ng ? 0u : 1u,
                                .pWaitSemaphoreInfos = &wait};
        vk_queue_lock();
        VkResult r2 = vkQueueSubmit2(c->gfx_queue, 1, &settle, g_fence[slot]);
        vk_queue_unlock();
        if (r2 != VK_SUCCESS) rt_log("gfx: the present slot could not be settled either (%d)", (int)r2);
        plat_xchg32(&g_dirty, 1);
    }
    g_frame++;
    VkResult cap_r = cap ? vkWaitForFences(c->device, 1, &g_fence[slot], VK_TRUE, 5000000000ull) : VK_SUCCESS;
    if (cap && cap_r != VK_SUCCESS) {   // the read-back buffer may not hold this frame yet: nothing reads it
        vk_device_lost_check(cap_r, "the wait for a captured frame");
        g_cap_valid = 0;
        static int logged;
        if (logged++ < 8) rt_log("gfx: the wait for a captured frame ended with %d: no capture this frame", (int)cap_r);
        if (grab) {
            void (*cb)(VkGrab *g, void *ctx) = g_grab_cb;
            g_grab_cb = NULL;
            cb(NULL, g_grab_ctx);
        }
    } else if (cap) {
        vmaInvalidateAllocation(c->vma, g_cap_alloc, 0, VK_WHOLE_SIZE);
        g_cap_w = (int)g_sc_ext.width;
        g_cap_h = (int)g_sc_ext.height;
        g_cap_valid = 1;
        if (menu_shot) save_capture(menu_shot);
        if (grab) grab_deliver(cw, ch);
    } else if (grab) {   // no read-back buffer: the request ends without pixels
        void (*cb)(VkGrab *g, void *ctx) = g_grab_cb;
        g_grab_cb = NULL;
        cb(NULL, g_grab_ctx);
    }
out:
    plat_rmutex_unlock(&g_pl);
}

void vk_present_image(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]) {
    present_image(src, layout, fmt, w, h, crop, 0);
}

void vk_present_blit(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]) {
    present_image(src, layout, fmt, w, h, crop, 1);
}

// The boot screen on a black frame (menu_boot_frame), without a game image: present_image's paths that read a source
// are skipped. The size only places the picture rectangle, which a frame without a picture leaves as it was.
void gfx_present_boot(void) { present_image(VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_FORMAT_UNDEFINED, 1920, 1080, NULL, 0); }

void vk_present_flip(void) {
    if (vk_device_is_lost()) return;
    plat_rmutex_lock(&g_pl);
    flip_locked();
    plat_rmutex_unlock(&g_pl);
}

void gfx_window_resize(int cw, int chh) {
    gfx_window_start();
    wsys_resize(cw, chh);
    plat_rmutex_lock(&g_pl);
    create_swapchain();
    plat_rmutex_unlock(&g_pl);
}

// Called by vk_shutdown before the device goes away; the window and its thread stay.
void gfx_window_release_vk(void) {
    VkCtx *c = vk_ctx();
    plat_rmutex_lock(&g_pl);
    if (c && g_started) {
        vk_queue_lock();
        vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();
        vk_queue_unlock();
        g_flip_on = 0;   // its blit is done (device idle); the swapchain goes away unpresented
        vk_lowlat_swapchain(VK_NULL_HANDLE, 0);
        ng_release();    // NGX frame generation's presenter shows nothing more; its images go before the device
        vk_fsr3fg_release();   // FSR 3.1 frame generation's passes and images, made again for a later device
        menu_vk_release();
        vk_fsr_release();
        vk_color_release();
        vk_hdr_release();
        vk_contact_release();
        free_capture();
        free_thumbs();
        for (int i = 0; i < FRAMES; i++) {
            vkDestroySemaphore(c->device, g_acquire[i], NULL);
            vkDestroyFence(c->device, g_fence[i], NULL);
        }
        for (uint32_t i = 0; i < g_sc_n; i++) {
            vkDestroySemaphore(c->device, g_render_done[i], NULL);
            if (g_sc_view[i]) vkDestroyImageView(c->device, g_sc_view[i], NULL);
            g_sc_view[i] = VK_NULL_HANDLE;
        }
        if (g_pool) vkDestroyCommandPool(c->device, g_pool, NULL);
        if (g_sc && vk_fg_active()) vk_fg_destroy_swapchain(g_sc), vk_fg_release();
        else if (g_sc && vk_dlssg_active()) vk_dlssg_destroy_swapchain(c->device, g_sc);
        else if (g_sc) vkDestroySwapchainKHR(c->device, g_sc, NULL);
        if (g_surface) vkDestroySurfaceKHR(c->instance, g_surface, NULL);
        g_sc = VK_NULL_HANDLE; g_surface = VK_NULL_HANDLE; g_pool = VK_NULL_HANDLE; g_sc_n = 0;
        g_started = 0;     // the window and its thread stay; the next gfx_window_start makes a new surface
    }
    plat_rmutex_unlock(&g_pl);
}

void gfx_window_start(void) {
    plat_rmutex_lock(&g_start_lock);
    if (!g_started) {
        if (!vk_init()) rt_fatal("gfx: no usable Vulkan device (see the vk: lines above)");
        VkCtx *c = vk_ctx();
        static int opened;
        if (!opened && !(opened = wsys_open())) rt_fatal("gfx: window creation failed");
        g_surface = wsys_surface(c->instance);
        if (!g_surface) rt_fatal("gfx: no Vulkan surface for the window");

        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
        // The frame slots' command pool, command buffers, fences and acquire semaphores: every present uses them, so
        // one that is not made ends the start with its call and result, where a null handle used to fail later
        // inside the driver.
        VkResult r = vkCreateCommandPool(c->device, &pci, NULL, &g_pool);
        if (r != VK_SUCCESS) rt_fatal("gfx: vkCreateCommandPool for the frames failed (%d)", (int)r);
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES};
        if ((r = vkAllocateCommandBuffers(c->device, &ai, g_cb)) != VK_SUCCESS)
            rt_fatal("gfx: vkAllocateCommandBuffers for the frames failed (%d)", (int)r);
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (int i = 0; i < FRAMES; i++) {
            if ((r = vkCreateFence(c->device, &fi, NULL, &g_fence[i])) != VK_SUCCESS)
                rt_fatal("gfx: vkCreateFence for frame slot %d failed (%d)", i, (int)r);
            if ((r = vkCreateSemaphore(c->device, &semi, NULL, &g_acquire[i])) != VK_SUCCESS)
                rt_fatal("gfx: vkCreateSemaphore for frame slot %d failed (%d)", i, (int)r);
        }
        rt_log("gfx: presentation checks on (no setting): the swapchain's image list and semaphores are checked and a "
               "failed one is made again; the game's crop is cut to the image without overflow; a failed sampler "
               "falls back and is tried again");
        g_frame = 0;
        g_started = 1;
        plat_rmutex_lock(&g_pl);
        create_swapchain();
        plat_rmutex_unlock(&g_pl);
        int cw = 0, ch = 0;
        wsys_client_size(&cw, &ch);
        rt_log("gfx: window %dx%d at DPI %u, swapchain %ux%u format %d", cw, ch, gfx_window_dpi(), g_sc_ext.width,
               g_sc_ext.height, (int)g_sc_fmt);
    }
    plat_rmutex_unlock(&g_start_lock);
    vsync_ensure();
}

int gfx_window_in_front(void) { return gfx_window_focused() && !wsys_minimized(); }

// For the hang watch's report (hang_watch.c): who holds the present, queue and window-start locks now, read without
// taking them (0: free). The fields may be caught mid-change; the report only names them.
void gfx_lock_holders(GfxLockHolders *h) {
    h->present = __atomic_load_n(&g_pl.owner, __ATOMIC_RELAXED);
    h->present_depth = __atomic_load_n(&g_pl.depth, __ATOMIC_RELAXED);
    h->queue = __atomic_load_n(&g_ql.owner, __ATOMIC_RELAXED);
    h->queue_depth = __atomic_load_n(&g_ql.depth, __ATOMIC_RELAXED);
    h->start = __atomic_load_n(&g_start_lock.owner, __ATOMIC_RELAXED);
}
