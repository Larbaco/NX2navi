// HDR output (NX2NAVI_HDR=1): an HDR10 swapchain (Rec. 2020 primaries, PQ) and a game picture with highlights above the
// SDR white. Settings: NX2NAVI_HDR (read at start), NX2NAVI_HDR_PEAK_NITS (the display's peak), NX2NAVI_HDR_GAME_NITS (the
// picture's white, "paper white") and NX2NAVI_HDR_UI_NITS (the HUD's and this menu's white), the three read when the
// settings change. With NX2NAVI_HDR=0 nothing here runs and the present is what it was.
//
// A display without an HDR10 surface format gets the same: vk_hdr_surface_format switches the setting off for the run
// (vk_hdr_off) when the first swapchain finds none. rt_boot (boot.c) makes that first swapchain before the shader
// prewarm, so no shader is swapped (vk_hdr_shader_swap) for an output that does not exist. A window moved to another
// monitor later keeps the first answer: the swapped shaders are in the pipelines by then.
//
// The game's picture (docs/architecture.md, "HDR"). The game tonemaps its scene into an r11g11b10f target, upscales it
// and draws the HUD into a second one (float, nothing in these passes clamps), and one last draw copies that into the
// 8-bit sRGB window texture it presents. So:
// 1. vk_hdr_shader_swap, on each fragment shader as it loads (in memory only; the cache keeps the translator's): 1.1.1's
//    tonemap ends in the fitted ACES curve and a clamp to 0..1 per channel, and aces_extend changes those clamps so
//    values above 1 (relative to the HUD's white) survive, rolled off at the peak. Recognised by the curve's
//    constants, not by a hash, so another translator still finds it. A tonemap of the earlier shape (1 - e^-x and a
//    saturation grade) becomes shaders/hdr_tonemap.frag instead. The game's FSR 1 upscale after it would clamp the
//    picture to 1 again; fsr_unclamp drops that clamp in its SPIR-V.
// 2. vk_hdr_draw / vk_hdr_present_source: a draw into a presented window texture that samples one float texture of
//    its size is that copy; the present then reads the float texture instead of the window texture. Any other last
//    draw into it leaves the window texture, decoded from sRGB: menus and movies stay SDR at the HUD's white.
//    The present is queued before the next frame's work (vk_backend.c), so the float target is read before the game
//    draws into it again.
// 3. vk_hdr_encode: on the present, the crop rectangle goes to PQ code values in an RGBA16F image (shaders/hdr_encode.comp)
//    that FSR 1 or the blit scale into the swapchain. vk_hdr_ui_*: the in-game menu draws into an RGBA8 image and
//    shaders/hdr_ui.frag blends it onto the swapchain at the HUD's white.
// The tonemap's approach: the game's curve up to near its white, then its tangent ("extended vanilla").
#include "vk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "hdr_encode_spv.h"
#include "hdr_fullscreen_spv.h"
#include "hdr_tonemap_spv.h"
#include "hdr_ui_spv.h"
#include "host.h"
#include "nvn_api.h"
#include "vk_exec.h"
#include "vk_fg.h"
#include "platform/platform.h"

#define SLOTS 8

// ---- settings

static int g_wanted = -1;

int vk_hdr_wanted(void) {
    if (g_wanted < 0) g_wanted = cfg_bool("NX2NAVI_HDR");
    return g_wanted;
}

// NX2NAVI_HDR = native: the game's own HDR. The display is reported as HDR10 (ov_gfx.c QueryDisplayHdrStatus, with
// the peak and paper white of the settings), and the game then renders its HDR picture itself and presents it as
// PQ-encoded Rec. 2020 in A2B10G10R10 (format 64), which goes to the HDR10 swapchain as it is: no tonemap swap, no
// float picture, no encode (pictures still presented in 8 bits, before the switch, are encoded as for 1).
static int g_native = -1;
int vk_hdr_native(void) {
    if (g_native < 0) g_native = !strcmp(cfg_value("NX2NAVI_HDR"), "native");
    return g_native && vk_hdr_wanted();
}

void vk_hdr_off(void) { g_wanted = 0; }

// Set when vk_hdr_surface_format turned the setting off because the display has no HDR10 format (the menu says so).
static int g_no_display;
int vk_hdr_no_display(void) { return g_no_display; }

// The three nits, read again when the settings change. The replay (every draw's support buffer) and the present
// thread read them: an update fills the snapshot not in use and then publishes it.
typedef struct { float peak, game, ui, hue; } Nits;
static struct { unsigned seen; int cur; Nits snap[2]; PlatMutex lock; } N = {.seen = ~0u, .lock = PLAT_MUTEX_INIT};

static Nits nits(void) {
    unsigned v = cfg_version();
    if (__atomic_load_n(&N.seen, __ATOMIC_ACQUIRE) != v) {
        plat_mutex_lock(&N.lock);
        if (N.seen != v) {
            Nits *n = &N.snap[!N.cur];
            n->peak = fmaxf((float)cfg_double("NX2NAVI_HDR_PEAK_NITS"), 80.0f);
            n->game = fmaxf((float)cfg_double("NX2NAVI_HDR_GAME_NITS"), 80.0f);
            n->ui = fmaxf((float)cfg_double("NX2NAVI_HDR_UI_NITS"), 80.0f);
            n->hue = fminf(fmaxf((float)cfg_double("NX2NAVI_HDR_HUE"), 0.0f), 100.0f) / 100.0f;
            __atomic_store_n(&N.cur, !N.cur, __ATOMIC_RELEASE);
            __atomic_store_n(&N.seen, v, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&N.lock);
    }
    return N.snap[__atomic_load_n(&N.cur, __ATOMIC_ACQUIRE)];
}

void vk_hdr_support(float out[4]) {
    Nits n = nits();
    out[0] = n.game / n.ui;                              // the picture's white against the HUD's
    out[1] = n.peak > n.game ? n.peak / n.game : 1.0f;   // the peak against the picture's white
    out[2] = n.hue;                                      // hue correction of highlights (hdr_tonemap.frag)
    out[3] = 0;
}

// ---- 1. the tonemap shader

enum { SPV_OP_NAME = 5, SPV_OP_TYPE_FLOAT = 22, SPV_OP_CONSTANT = 43, SPV_OP_DECORATE = 71, SPV_DEC_BINDING = 33,
       SPV_DEC_SET = 34 };

// The variables of the game's shader, in the order of hdr_tonemap.frag's bindings 0..4.
static const char *const tm_names[5] = {"support_buffer", "fp_t_tcb_8", "fp_t_tcb_A", "fp_c1", "fp_c3"};

static int name_is(const uint32_t *a, uint32_t len, const char *s) {   // OpName operands after the id
    size_t n = strlen(s);
    return (len - 2) * 4 >= n + 1 && !memcmp(a + 1, s, n + 1);
}

// The game's FSR 1 pass after the tonemap (some frames take it in place of the plain pass, at 1:1 as well as when it
// scales up; what picks it is not known yet) ends with clamp(colour, 0.0, 1.0) per channel, which cuts the HDR
// highlights back to the HUD's white for as long as the game takes it. Its own 0..1 clamps of the filter weights each
// feed a float bit trick (OpBitcast), so only the clamps whose result is stored to the shader's output change, to
// max(x, 0.0): one word shorter, so the words after it move down and the module shrinks. Recognised by FSR's two
// reciprocal constants (0x7ef07ebb, 0x7ef19fff) and its one texture fp_t_tcb_8. Returns the clamps changed, -1 for
// that shader with no output clamp found (a translator that stores the colour another way), 0 for any other shader.
enum { SPV_OP_EXT_INST_IMPORT = 11, SPV_OP_EXT_INST = 12, SPV_OP_VARIABLE = 59, SPV_OP_STORE = 62,
       SPV_OP_ACCESS_CHAIN = 65, SPV_SC_OUTPUT = 3, GLSL_FMAX = 40, GLSL_FCLAMP = 43, GLSL_NMAX = 80,
       GLSL_NCLAMP = 81 };
// A 0..1 clamp: translators write FClamp, or NClamp (NaN gives the low bound, as the GPU's saturate does); its
// replacement keeps the kind (NMax for NClamp).
static int is_clamp(uint32_t inst) { return inst == GLSL_FCLAMP || inst == GLSL_NCLAMP; }

static int fsr_unclamp(uint32_t *w, uint64_t *words) {
    uint64_t n = *words;
    uint32_t bound = w[3], glsl = 0, float_type = 0, zero = 0, one = 0;
    int magic = 0, tex = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_EXT_INST_IMPORT && len >= 3 && name_is(a, len, "GLSL.std.450")) glsl = a[0];
        else if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) float_type = a[0];
        else if (op == SPV_OP_CONSTANT && len == 4) {
            magic |= (a[2] == 0x7ef07ebbu) | (a[2] == 0x7ef19fffu) << 1;
            if (a[0] == float_type && float_type && a[2] == 0) zero = a[1];
            if (a[0] == float_type && float_type && a[2] == 0x3f800000u) one = a[1];
        } else if (op == SPV_OP_NAME && len >= 3 && name_is(a, len, "fp_t_tcb_8")) tex = 1;
        i += len;
    }
    if (magic != 3 || !tex) return 0;
    if (!glsl || !zero || !one || !bound) return -1;
    uint8_t *out = calloc(bound, 1);   // id -> 1: a pointer into an Output variable, 2: a value stored through one
    if (!out) return 0;
    for (uint64_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_VARIABLE && len >= 4 && a[2] == SPV_SC_OUTPUT && a[1] < bound) out[a[1]] = 1;
        else if (op == SPV_OP_ACCESS_CHAIN && len >= 4 && a[2] < bound && out[a[2]] == 1 && a[1] < bound) out[a[1]] = 1;
        else if (op == SPV_OP_STORE && len >= 3 && a[0] < bound && out[a[0]] == 1 && a[1] < bound) out[a[1]] = 2;
    }
    int changed = 0;
    uint64_t k = 5;   // the write position: never past the read position, as instructions only shrink
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;   // result type, result, set, instruction, x, min, max
        if (op == SPV_OP_EXT_INST && len == 8 && a[2] == glsl && is_clamp(a[3]) && a[1] < bound && out[a[1]] == 2 &&
            a[5] == zero && a[6] == one) {
            uint32_t ins[7] = {7u << 16 | SPV_OP_EXT_INST, a[0], a[1], a[2], a[3] == GLSL_NCLAMP ? GLSL_NMAX : GLSL_FMAX,
                               a[4], zero};
            memmove(w + k, ins, sizeof ins);
            k += 7, changed++;
        } else {
            memmove(w + k, w + i, len * sizeof *w);
            k += len;
        }
        i += len;
    }
    free(out);
    *words = k;
    return changed ? changed : -1;
}

// Ocarina 1.1.1's tonemap (one draw from the scene light target, R11G11B10F, into a target of the same size, before
// anti-aliasing, DLSS and the HUD): the game's exposure, then per channel the fitted ACES filmic curve
// x (2.51 x + 0.03) / (x (2.43 x + 0.59) + 0.14) and clamp(.., 0, 1), the value it stores. aces_extend changes each such
// clamp in place, so the game's exposure and look stay its own:
//   y = aces(x) + d^2 / (1 + d), d = max(x - ACES_XP, 0): the game's curve up to ACES_XP (0.67 of the SDR white
//   there), then a boost that starts flat (no crease in a gradient across ACES_XP) and grows to one picture white per
//   unit of x, where the game's curve has flattened out near 1: bright light stays bright instead of all ending near
//   white (x = 1.4, the sunlit leaves of a forest by day: 1.22 against the game's 0.86; x = 3: 2.6),
//   a roll-off of y from half the peak to the peak (support +1440 .y: peak / picture white, at least 1),
//   times the picture's white against the HUD's (+1440 .x).
// The values are relative to the HUD's white, as the present's encode expects. The support buffer gets a member at
// 1440 (vk_exec_draw.c support_fill writes it), which the translator's SupportBuffer ends before. Recognised by the
// curve's 2.51 / 0.03 pair feeding a clamp to 0..1: any shader that ends with that curve keeps its highlights.
// Returns the clamps changed (0: not that shader).
enum { SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_BOOL = 20, SPV_OP_TYPE_VECTOR = 23, SPV_OP_TYPE_STRUCT = 30,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_FUNCTION = 54, SPV_OP_LOAD = 61, SPV_OP_COMPOSITE_EXTRACT = 81,
       SPV_OP_FNEGATE = 127, SPV_OP_FADD = 129, SPV_OP_FSUB = 131, SPV_OP_FMUL = 133, SPV_OP_FDIV = 136,
       SPV_OP_SELECT = 169, SPV_OP_FORD_GREATER = 186, SPV_OP_MEMBER_DECORATE = 72, SPV_DEC_OFFSET = 35,
       SPV_SC_UNIFORM = 2, GLSL_EXP = 27, GLSL_FMA = 50 };
#define ACES_XP 0.6f

static int aces_extend(uint32_t **spirv, uint64_t *words) {
    uint32_t *w = *spirv;
    uint64_t n = *words;
    uint32_t bound = w[3], glsl = 0, ft = 0, bt = 0, it = 0, v4 = 0, c0 = 0, c1 = 0, c251 = 0, c003 = 0, sb = 0, sb_ptr = 0,
             st = 0, st_len = 0, ptr_v4 = 0;
    const float f251 = 2.51f, f003 = 0.03f, fone = 1.0f;
    uint32_t b251, b003, bone;
    memcpy(&b251, &f251, 4), memcpy(&b003, &f003, 4), memcpy(&bone, &fone, 4);
    uint32_t last_offset = 0;
    uint32_t *def = calloc(bound ? bound : 1, sizeof *def);   // result id -> word index of its instruction
    if (!def) return 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) { free(def); return 0; }
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_EXT_INST_IMPORT && len >= 3 && name_is(a, len, "GLSL.std.450")) glsl = a[0];
        else if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) ft = a[0];
        else if (op == SPV_OP_TYPE_BOOL && len == 2) bt = a[0];
        else if (op == SPV_OP_TYPE_INT && len == 4 && a[1] == 32 && a[2] == 1) it = a[0];
        else if (op == SPV_OP_TYPE_VECTOR && len == 4 && a[1] == ft && ft && a[2] == 4) v4 = a[0];
        else if (op == SPV_OP_CONSTANT && len == 4 && a[0] == ft && ft) {
            if (a[2] == 0) c0 = a[1];
            else if (a[2] == bone) c1 = a[1];
            else if (a[2] == b251) c251 = a[1];
            else if (a[2] == b003) c003 = a[1];
        } else if (op == SPV_OP_NAME && len >= 3 && name_is(a, len, "support_buffer")) sb = a[0];
        else if (op == SPV_OP_TYPE_POINTER && len == 4 && a[1] == SPV_SC_UNIFORM && a[2] == v4 && v4) ptr_v4 = a[0];
        else if (op == SPV_OP_MEMBER_DECORATE && len == 5 && a[1] == 10 && a[2] == SPV_DEC_OFFSET && a[3] == 1424)
            last_offset = a[0];   // the struct whose member 10 is at 1424
        if (op == SPV_OP_VARIABLE && len >= 4 && a[1] == sb && sb) sb_ptr = a[0];
        // results: [0] type, [1] id for the instructions that have both
        if ((op == SPV_OP_FMUL || op == SPV_OP_FDIV || op == SPV_OP_EXT_INST || op == SPV_OP_LOAD) && len >= 3 && a[1] < bound)
            def[a[1]] = (uint32_t)i;
        i += len;
    }
    for (uint64_t i = 5; i < n && sb_ptr; i += w[i] >> 16)   // the support buffer's block: pointer -> struct
        if ((w[i] & 0xffff) == SPV_OP_TYPE_POINTER && (w[i] >> 16) == 4 && w[i + 1] == sb_ptr) st = w[i + 3];
    for (uint64_t i = 5; i < n && st; i += w[i] >> 16)
        if ((w[i] & 0xffff) == SPV_OP_TYPE_STRUCT && w[i + 1] == st) st_len = w[i] >> 16;
    // the clamps: FClamp(FMul(FMul(x, Fma(x, 2.51, 0.03)), rcp), 0, 1)
    enum { MAX_SITES = 8 };
    uint64_t site[MAX_SITES];
    uint32_t site_x[MAX_SITES];
    int nsite = 0;
    if (glsl && ft && bt && it && v4 && c0 && c1 && c251 && c003 && st_len == 13 && last_offset == st)
        for (uint64_t i = 5; i < n && nsite < MAX_SITES; i += w[i] >> 16) {
            const uint32_t *a = w + i + 1;
            if ((w[i] & 0xffff) != SPV_OP_EXT_INST || (w[i] >> 16) != 8 || a[2] != glsl || !is_clamp(a[3]) ||
                a[5] != c0 || a[6] != c1 || a[4] >= bound || !def[a[4]])
                continue;
            const uint32_t *m = w + def[a[4]];   // FMul(num, rcp)
            if ((m[0] & 0xffff) != SPV_OP_FMUL || m[3] >= bound || !def[m[3]]) continue;
            const uint32_t *num = w + def[m[3]];   // FMul(x, Fma(x, 2.51, 0.03))
            if ((num[0] & 0xffff) != SPV_OP_FMUL || num[4] >= bound || !def[num[4]]) continue;
            const uint32_t *f = w + def[num[4]];
            if ((f[0] & 0xffff) != SPV_OP_EXT_INST || (f[0] >> 16) != 8 || f[4] != GLSL_FMA || f[6] != c251 || f[7] != c003)
                continue;
            site[nsite] = i, site_x[nsite++] = num[3];
        }
    free(def);
    if (!nsite) return 0;
    float fxp = ACES_XP, fhalf = 0.5f;
    uint32_t id = bound;
    uint32_t new_ptr = ptr_v4 ? 0 : id++, ptr = ptr_v4 ? ptr_v4 : new_ptr;
    uint32_t i11 = id++, kxp = id++, khalf = id++;
    // the struct's extra word, its member decoration, four global instructions, 25 instructions of at most 8 words a site
    uint64_t cap = n + 1 + 5 + 4 * 5 + (uint64_t)nsite * 25 * 8;
    uint32_t *o = malloc(cap * sizeof *o);
    if (!o) return 0;
    uint64_t k = 0;
    memcpy(o, w, 5 * sizeof *w), k = 5;
    // one instruction: its opcode, then its operands
#define EMIT(...) do { const uint32_t ins_[] = {__VA_ARGS__}; const uint32_t n_ = sizeof ins_ / sizeof *ins_; \
        o[k] = n_ << 16 | ins_[0]; memcpy(o + k + 1, ins_ + 1, (n_ - 1) * sizeof *ins_); k += n_; } while (0)
    int globals_done = 0, s = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_FUNCTION && !globals_done) {   // new types and constants close the global section
            if (new_ptr) EMIT(SPV_OP_TYPE_POINTER, new_ptr, SPV_SC_UNIFORM, v4);
            uint32_t b[2];
            memcpy(&b[0], &fxp, 4), memcpy(&b[1], &fhalf, 4);
            EMIT(SPV_OP_CONSTANT, it, i11, 11);
            EMIT(SPV_OP_CONSTANT, ft, kxp, b[0]);
            EMIT(SPV_OP_CONSTANT, ft, khalf, b[1]);
            globals_done = 1;
        }
        if (op == SPV_OP_TYPE_STRUCT && a[0] == st) {   // the support block gains a vec4 at 1440
            memcpy(o + k, w + i, len * sizeof *w);
            o[k] = (len + 1) << 16 | op;
            o[k + len] = v4;
            k += len + 1;
        } else if (op == SPV_OP_MEMBER_DECORATE && len == 5 && a[0] == st && a[1] == 10 && a[2] == SPV_DEC_OFFSET &&
                   a[3] == 1424) {
            memcpy(o + k, w + i, len * sizeof *w), k += len;
            EMIT(SPV_OP_MEMBER_DECORATE, st, 11, SPV_DEC_OFFSET, 1440);
        } else if (s < nsite && i == site[s]) {
            uint32_t x = site_x[s], r = a[4], res = a[1];
            uint32_t pv = id++, hv = id++, g = id++, p = id++, r0 = id++, dx = id++, d0 = id++, sq = id++, den = id++,
                     bo = id++, y = id++, pm = id++, kn = id++, rng = id++, d = id++, q = id++, nq = id++, e = id++,
                     om = id++, rl = id++, gk = id++, y2 = id++, gpos = id++, gs = id++;
            EMIT(SPV_OP_ACCESS_CHAIN, ptr, pv, sb, i11);
            EMIT(SPV_OP_LOAD, v4, hv, pv);
            EMIT(SPV_OP_COMPOSITE_EXTRACT, ft, g, hv, 0);
            EMIT(SPV_OP_COMPOSITE_EXTRACT, ft, p, hv, 1);
            EMIT(SPV_OP_EXT_INST, ft, r0, glsl, GLSL_NMAX, r, c0);                // NaN: 0, as the clamp gave
            EMIT(SPV_OP_FSUB, ft, dx, x, kxp);                                   // the boost above ACES_XP
            EMIT(SPV_OP_EXT_INST, ft, d0, glsl, GLSL_NMAX, dx, c0);
            EMIT(SPV_OP_FMUL, ft, sq, d0, d0);
            EMIT(SPV_OP_FADD, ft, den, d0, c1);
            EMIT(SPV_OP_FDIV, ft, bo, sq, den);
            EMIT(SPV_OP_FADD, ft, y, r0, bo);
            EMIT(SPV_OP_EXT_INST, ft, pm, glsl, GLSL_FMAX, p, c1);              // the peak, in picture whites
            EMIT(SPV_OP_FMUL, ft, kn, pm, khalf);                                // the knee: half of it
            EMIT(SPV_OP_FSUB, ft, rng, pm, kn);
            EMIT(SPV_OP_FSUB, ft, d, y, kn);
            EMIT(SPV_OP_FDIV, ft, q, d, rng);
            EMIT(SPV_OP_FNEGATE, ft, nq, q);
            EMIT(SPV_OP_EXT_INST, ft, e, glsl, GLSL_EXP, nq);
            EMIT(SPV_OP_FSUB, ft, om, c1, e);
            EMIT(SPV_OP_EXT_INST, ft, rl, glsl, GLSL_FMA, rng, om, kn);
            EMIT(SPV_OP_FORD_GREATER, bt, gk, y, kn);
            EMIT(SPV_OP_SELECT, ft, y2, gk, rl, y);
            EMIT(SPV_OP_FORD_GREATER, bt, gpos, g, c0);                          // the picture's white, 1 when unset
            EMIT(SPV_OP_SELECT, ft, gs, gpos, g, c1);
            EMIT(SPV_OP_FMUL, ft, res, y2, gs);
            s++;
        } else {
            memcpy(o + k, w + i, len * sizeof *w), k += len;
        }
        i += len;
    }
#undef EMIT
    if (!globals_done || s != nsite || k > cap) {
        free(o);
        return 0;
    }
    o[3] = id;
    free(*spirv);
    *spirv = o;
    *words = k;
    return nsite;
}

void vk_hdr_shader_swap(uint32_t **spirv, uint64_t *words) {
    if (!vk_hdr_wanted() || vk_hdr_native() || !*spirv || *words < 5) return;
    int extended = aces_extend(spirv, words);
    if (extended > 0) {
        static volatile int32_t exts;
        rt_log("hdr: the game's tonemap keeps highlights above the picture's white (%d channels; %d shaders so far)",
               extended, plat_inc32(&exts));
    }
    int unclamped = fsr_unclamp(*spirv, words);
    if (unclamped > 0) {
        static volatile int32_t fsrs;
        rt_log("hdr: the game's FSR pass keeps values above 1 (%d output clamps; %d so far)", unclamped,
               plat_inc32(&fsrs));
        return;
    }
    if (unclamped < 0) {
        static volatile int32_t missed;
        if (plat_inc32(&missed) == 1)
            rt_log("hdr: the game's FSR pass was found but no clamp of its output; it still clips HDR highlights");
        return;
    }
    const uint32_t *w = *spirv;
    uint64_t n = *words;
    // the game's constants: the BT.601 red weight, -1/ln 2 (e^-x as exp2), 2/3 (the grade's red weight), the bloom clamp
    static const float want[4] = {0.298900008f, -1.44269502f, 0.666666687f, 10000.0f};
    uint32_t float_type = 0, found = 0, ids[5] = {0}, bind[5], set[5];
    int has_bind[5] = {0};
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) float_type = a[0];
        else if (op == SPV_OP_CONSTANT && len == 4 && a[0] == float_type && float_type)
            for (int k = 0; k < 4; k++) {
                float f;
                memcpy(&f, &a[2], 4);
                if (f == want[k]) found |= 1u << k;
            }
        else if (op == SPV_OP_NAME && len >= 3)
            for (int k = 0; k < 5; k++)
                if (name_is(a, len, tm_names[k])) ids[k] = a[0];
        i += len;
    }
    if (found != 15) return;
    for (int k = 0; k < 5; k++)
        if (!ids[k]) return;
    for (uint64_t i = 5; i < n;) {   // the bindings and sets of those variables
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_DECORATE && len == 4)
            for (int k = 0; k < 5; k++)
                if (a[0] == ids[k]) {
                    if (a[1] == SPV_DEC_BINDING) bind[k] = a[2], has_bind[k] |= 1;
                    else if (a[1] == SPV_DEC_SET) set[k] = a[2], has_bind[k] |= 2;
                }
        i += len;
    }
    for (int k = 0; k < 5; k++)
        if (has_bind[k] != 3) return;
    uint64_t m = sizeof hdr_tonemap_spv / 4;
    uint32_t *out = malloc(sizeof hdr_tonemap_spv);
    if (!out) return;
    memcpy(out, hdr_tonemap_spv, sizeof hdr_tonemap_spv);
    // ours declares binding k for tm_names[k] in set 0: each gets the game's binding and set
    uint32_t bound = out[3], *ours = calloc(bound ? bound : 1, sizeof *ours);   // id -> our binding + 1
    if (!ours) {
        free(out);
        return;
    }
    for (uint64_t i = 5; i < m;) {
        uint32_t op = out[i] & 0xffff, len = out[i] >> 16;
        uint32_t *a = out + i + 1;
        if (op == SPV_OP_DECORATE && len == 4 && a[1] == SPV_DEC_BINDING && a[2] < 5) {
            if (a[0] < bound) ours[a[0]] = a[2] + 1;
            a[2] = bind[a[2]];
        }
        i += len;
    }
    for (uint64_t i = 5; i < m;) {
        uint32_t op = out[i] & 0xffff, len = out[i] >> 16;
        uint32_t *a = out + i + 1;
        if (op == SPV_OP_DECORATE && len == 4 && a[1] == SPV_DEC_SET && a[0] < bound && ours[a[0]]) a[2] = set[ours[a[0]] - 1];
        i += len;
    }
    free(ours);
    free(*spirv);
    *spirv = out;
    *words = m;
    static volatile int32_t swaps;
    rt_log("hdr: the game's tonemap shader runs as shaders/hdr_tonemap.frag (%d so far)", plat_inc32(&swaps));
}

// ---- 2. the picture the window texture is copied from

int vk_hdr_track;                 // vk_exec_draw.c calls vk_hdr_draw: set at the first present with NX2NAVI_HDR=1
static volatile int32_t g_active;   // the swapchain is HDR10 (gfx_present.c vk_hdr_set_active)
static PlatMutex rec_lock = PLAT_MUTEX_INIT;
static struct { VkImage win, src; VkFormat fmt; } recs[4];   // per presented window image: what the last draw copied
static int nrecs;

void vk_hdr_set_active(int on) { plat_xchg32(&g_active, on); }
int vk_hdr_active(void) { return plat_cas32(&g_active, 0, 0); }

static int is_float(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT ||
           f == VK_FORMAT_R32G32B32A32_SFLOAT;
}

void vk_hdr_draw(VkExec *ex, const VkPipeInfo *info) {
    const VkExecTarget *t = &ex->color[0];
    if (!t->image) return;
    int known = 0;
    for (int i = 0; i < 4; i++)   // unlocked first look: most draws go elsewhere (a stale slot only costs the lock below)
        known |= __atomic_load_n(&recs[i].win, __ATOMIC_RELAXED) == t->image;
    if (!known) return;
    VkImage src = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    int ntex = 0, slot = -1;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (bd->kind != GFX_RES_TEX || bd->stage != GFX_STAGE_FS || !bd->res) continue;
        ntex += bd->count ? bd->count : 1;
        if ((bd->res->type & 0x7ff) == 3 && !bd->res->separate) slot = bd->res->slot;   // a plain 2D texture
    }
    if (ntex == 1 && slot >= 0 && slot < 128 && ex->tex[1][slot]) {
        const VkTexImage *s = vk_texture_by_id(NVN_HANDLE_TEX(ex->tex[1][slot]), NULL, NULL);
        if (s && is_float(s->format) && s->width == t->width && s->height == t->height && s->samples == 1)
            src = s->image, fmt = s->format;
    }
    plat_mutex_lock(&rec_lock);
    for (int i = 0; i < nrecs; i++)
        if (recs[i].win == t->image) recs[i].src = src, recs[i].fmt = fmt;
    plat_mutex_unlock(&rec_lock);
}

void vk_hdr_present_source(const VkTexImage *win, VkImage *image, VkFormat *format) {
    if (!vk_hdr_wanted() || vk_hdr_native()) return;
    vk_hdr_track = 1;
    plat_mutex_lock(&rec_lock);
    int r = -1;
    for (int i = 0; i < nrecs; i++)
        if (recs[i].win == win->image) r = i;
    if (r < 0 && nrecs < 4) r = nrecs++, recs[r].win = win->image, recs[r].src = VK_NULL_HANDLE;
    // kept until the next draw into the window texture: a present again without a new copy shows the same picture
    if (r >= 0 && recs[r].src && vk_hdr_active()) *image = recs[r].src, *format = recs[r].fmt;
    plat_mutex_unlock(&rec_lock);
    static int logged;
    if (!logged && *image != win->image) logged = 1, rt_log("hdr: the present reads the game's float picture");
}

void vk_hdr_image_gone(VkImage image) {   // a texture image went away: forget it as window or source
    if (!vk_hdr_track) return;   // no records before the first HDR present (vk_hdr_present_source): no lock per image
    plat_mutex_lock(&rec_lock);
    for (int i = 0; i < nrecs; i++) {
        if (recs[i].src == image) recs[i].src = VK_NULL_HANDLE;
        if (recs[i].win == image) recs[i] = recs[--nrecs], recs[nrecs] = (typeof(recs[0])){0}, i--;
    }
    plat_mutex_unlock(&rec_lock);
}

// ---- swapchain

// With NX2NAVI_HDR=1, the HDR10 format among the surface's (10-bit UNORM, ST 2084): 1 with *out set, else 0. The first
// call decides for the run: with no such format the setting goes off here (vk_hdr_off), the reason logged once.
int vk_hdr_surface_format(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out) {
    if (!vk_hdr_wanted()) return 0;
    static int logged;
    if (!logged) {
        char b[1024];
        int o = 0;
        for (uint32_t i = 0; i < n && o < (int)sizeof b - 32; i++)
            o += snprintf(b + o, sizeof b - (size_t)o, " %d/%d", (int)f[i].format, (int)f[i].colorSpace);
        rt_log("hdr: surface formats (VkFormat/VkColorSpaceKHR):%s", b);
    }
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++)
            if (f[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                f[i].format == (pass ? VK_FORMAT_A2R10G10B10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
                *out = f[i];
                if (!logged) rt_log("hdr: HDR10 swapchain (format %d)", (int)f[i].format);
                logged = 1;
                return 1;
            }
    if (!logged) {
        rt_log("hdr: the display offers no HDR10 format, so Enhanced HDR is off for this run and the picture is SDR as "
               "with HDR = 0 (Windows: turn on \"Use HDR\" in the display settings; Linux: a Wayland compositor with "
               "colour management and HDR on)");
        g_no_display = 1;
        vk_hdr_off();   // the tonemap swap, the support buffer and the native-HDR report all read vk_hdr_wanted
    }
    logged = 1;
    return 0;
}

// The display's peak and the picture's white for sc; again when the settings change (gfx_present.c calls it on every
// present to an HDR10 swapchain, with new = 1 for a new swapchain).
void vk_hdr_metadata(VkSwapchainKHR sc, int new_sc) {
    static unsigned sent = ~0u;
    VkCtx *c = vk_ctx();
    if (!c->has_hdr_metadata || (!new_sc && cfg_version() == sent)) return;
    sent = cfg_version();
    // AMD's frame generation swapchain takes it through its own call
    PFN_vkSetHdrMetadataEXT set = vk_fg_hdr_metadata_fn();
    if (!set) set = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(c->device, "vkSetHdrMetadataEXT");
    if (!set) return;
    Nits n = nits();
    VkHdrMetadataEXT m = {.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                          .displayPrimaryRed = {0.708f, 0.292f}, .displayPrimaryGreen = {0.170f, 0.797f},
                          .displayPrimaryBlue = {0.131f, 0.046f}, .whitePoint = {0.3127f, 0.3290f},
                          .maxLuminance = n.peak, .minLuminance = 0.0f, .maxContentLightLevel = n.peak,
                          .maxFrameAverageLightLevel = n.game};
    set(c->device, 1, &sc, &m);
}

// ---- 3. present passes

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

typedef struct { VkImage img; VmaAllocation alloc; VkImageView view; uint32_t w, h; } Img;

static void img_free(VkCtx *c, Img *m) {
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->alloc);
    *m = (Img){0};
}

// m at w x h in fmt; another size waits for the device first (earlier presents may still read the old image).
static int img_ensure(VkCtx *c, Img *m, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    if (m->img && m->w == w && m->h == h) return 1;
    if (m->img) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        img_free(c, m);
    }
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &m->img, &m->alloc, NULL) != VK_SUCCESS) {
        m->img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &m->view) != VK_SUCCESS) {
        m->view = VK_NULL_HANDLE;
        img_free(c, m);
        return 0;
    }
    m->w = w, m->h = h;
    return 1;
}

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkSampler sampler;
    VkDescriptorSetLayout dsl_enc, dsl_ui;
    VkPipelineLayout enc_layout, ui_layout;
    VkPipeline enc;
    VkPipeline ui;                  // for ui_fmt
    VkFormat ui_fmt;
    Img out;                        // PQ code values of the crop, RGBA16F
    Img menu;                       // the in-game menu, RGBA8
    VkImageView src_view[SLOTS];
} H;

static VkShaderModule module(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static VkDescriptorSetLayout set_layout(VkCtx *c, const VkDescriptorSetLayoutBinding *b, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                          .bindingCount = n, .pBindings = b};
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    return vkCreateDescriptorSetLayout(c->device, &ci, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static VkPipelineLayout pipe_layout(VkCtx *c, VkDescriptorSetLayout *dsl, VkShaderStageFlags st, uint32_t pc) {
    VkPushConstantRange pr = {st, 0, pc};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkPipelineLayout l = VK_NULL_HANDLE;
    return vkCreatePipelineLayout(c->device, &li, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static int init(VkCtx *c) {
    H.tried = 1;
    H.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding enc[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutBinding ui = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (!H.push || vkCreateSampler(c->device, &si, NULL, &H.sampler) != VK_SUCCESS ||
        !(H.dsl_enc = set_layout(c, enc, 2)) || !(H.dsl_ui = set_layout(c, &ui, 1)) ||
        !(H.enc_layout = pipe_layout(c, &H.dsl_enc, VK_SHADER_STAGE_COMPUTE_BIT, 32)) ||
        !(H.ui_layout = pipe_layout(c, &H.dsl_ui, VK_SHADER_STAGE_FRAGMENT_BIT, 16))) {
        rt_log("hdr: the present passes could not be created; the picture goes out unconverted");
        return 0;
    }
    VkShaderModule m = module(c, hdr_encode_spv, sizeof hdr_encode_spv);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = H.enc_layout};
    VkResult r = m ? vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &H.enc) : VK_ERROR_UNKNOWN;
    if (m) vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        rt_log("hdr: the present passes could not be created; the picture goes out unconverted");
        return 0;
    }
    return H.ok = 1;
}

// The game's own HDR picture: PQ codes in A2B10G10R10, the format of NVN's RGB10A2 (vk_formats.c); no guest format
// becomes A2R10G10B10.
int vk_hdr_native_picture(VkFormat f) { return vk_hdr_native() && f == VK_FORMAT_A2B10G10R10_UNORM_PACK32; }

VkImage vk_hdr_encode(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4]) {
    VkCtx *c = vk_ctx();
    // native HDR: the game's picture holds PQ codes already; the present scales it into the swapchain as it is
    if (vk_hdr_native_picture(fmt)) {
        static int told;
        if (!told++) rt_log("hdr: native: the game's own HDR10 picture goes to the swapchain as it is");
        return VK_NULL_HANDLE;
    }
    if (!H.tried) init(c);
    if (!H.ok || !img_ensure(c, &H.out, (uint32_t)crop[2], (uint32_t)crop[3], VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return VK_NULL_HANDLE;
    slot %= SLOTS;
    if (H.src_view[slot]) vkDestroyImageView(c->device, H.src_view[slot], NULL);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = src,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &H.src_view[slot]) != VK_SUCCESS) {
        H.src_view[slot] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    Nits n = nits();
    // an sRGB format decodes itself through the view; 8-bit UNORM holds sRGB codes the shader decodes
    int decode = fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_UNORM;
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, H.out.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, ALL, 0, CS,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VkDescriptorImageInfo ii = {H.sampler, H.src_view[slot], VK_IMAGE_LAYOUT_GENERAL},
                          oi = {VK_NULL_HANDLE, H.out.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.enc);
    rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.enc_layout, 0, 2, w);
    struct { int32_t offset[2], size[2]; float p[4]; } pc = {{crop[0], crop[1]}, {crop[2], crop[3]},
                                                            {n.ui, n.peak, decode ? 1.0f : 0.0f, 0}};
    vkCmdPushConstants(cb, H.enc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, ((uint32_t)crop[2] + 7) / 8, ((uint32_t)crop[3] + 7) / 8, 1);
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, 0, ALL, 0);
    barrier(cb, H.out.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL,
            VK_ACCESS_2_MEMORY_READ_BIT);
    return H.out.img;
}

// The in-game menu's target: an RGBA8 image of the swapchain's size in COLOR_ATTACHMENT_OPTIMAL (its old content
// dropped); the caller renders the menu into the view with a clear to 0.
VkImageView vk_hdr_ui_target(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.tried) init(c);
    if (!H.ok || !img_ensure(c, &H.menu, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return VK_NULL_HANDLE;
    barrier(cb, H.menu.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    return H.menu.view;
}

static int ui_pipeline(VkCtx *c, VkFormat fmt) {
    if (H.ui && H.ui_fmt == fmt) return 1;
    if (H.ui) {   // another swapchain format: earlier presents may still use the old pipeline
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        vkDestroyPipeline(c->device, H.ui, NULL);
        H.ui = VK_NULL_HANDLE;
    }
    VkShaderModule vs = module(c, hdr_fullscreen_spv, sizeof hdr_fullscreen_spv),
                   fs = module(c, hdr_ui_spv, sizeof hdr_ui_spv);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                                       .layout = H.ui_layout};
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &H.ui) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(c->device, vs, NULL);
    if (fs) vkDestroyShaderModule(c->device, fs, NULL);
    if (r != VK_SUCCESS) {
        H.ui = VK_NULL_HANDLE;
        rt_log("hdr: the menu pass could not be created; the menu is not shown");
        return 0;
    }
    H.ui_fmt = fmt;
    return 1;
}

// The menu image onto dst (a swapchain image in COLOR_ATTACHMENT_OPTIMAL, view dst_view of format fmt).
void vk_hdr_ui_compose(VkCommandBuffer cb, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.ok || !H.menu.img || !ui_pipeline(c, fmt)) return;
    barrier(cb, H.menu.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.ui);
    VkViewport v = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D s = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
    VkDescriptorImageInfo ii = {H.sampler, H.menu.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.ui_layout, 0, 1, &wr);
    float pc[4] = {nits().ui, 0, 0, 0};
    vkCmdPushConstants(cb, H.ui_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

void vk_hdr_release(void) {
    VkCtx *c = vk_ctx();
    for (int i = 0; i < SLOTS; i++)
        if (H.src_view[i]) vkDestroyImageView(c->device, H.src_view[i], NULL), H.src_view[i] = VK_NULL_HANDLE;
    img_free(c, &H.out);
    img_free(c, &H.menu);
    if (H.enc) vkDestroyPipeline(c->device, H.enc, NULL);
    if (H.ui) vkDestroyPipeline(c->device, H.ui, NULL);
    if (H.enc_layout) vkDestroyPipelineLayout(c->device, H.enc_layout, NULL);
    if (H.ui_layout) vkDestroyPipelineLayout(c->device, H.ui_layout, NULL);
    if (H.dsl_enc) vkDestroyDescriptorSetLayout(c->device, H.dsl_enc, NULL);
    if (H.dsl_ui) vkDestroyDescriptorSetLayout(c->device, H.dsl_ui, NULL);
    if (H.sampler) vkDestroySampler(c->device, H.sampler, NULL);
    memset(&H, 0, sizeof H);
}

// PQ code (0..1023) of a 10-bit channel to the sRGB byte of the same colour at the HUD's white (menu test shots and
// flight thumbnails of an HDR10 swapchain).
static float pq_nits(float e) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float p = powf(e, 1.0f / m2);
    return 10000.0f * powf(fmaxf(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

void vk_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r) {
    float ui = nits().ui;
    // Rec. 2020 to Rec. 709 (the inverse of ITU-R BT.2087's matrix)
    static const float m[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f, 1.1187f};
    for (size_t i = 0; i < px; i++) {
        uint32_t v;
        memcpy(&v, in + 4 * i, 4);
        float c[3] = {(float)(v & 1023), (float)(v >> 10 & 1023), (float)(v >> 20 & 1023)};   // A2B10G10R10: R low
        if (a2r) { float t = c[0]; c[0] = c[2]; c[2] = t; }
        for (int k = 0; k < 3; k++) c[k] = pq_nits(c[k] / 1023.0f) / ui;
        for (int k = 0; k < 3; k++) {
            float l = fminf(fmaxf(m[3 * k] * c[0] + m[3 * k + 1] * c[1] + m[3 * k + 2] * c[2], 0.0f), 1.0f);
            float s = l <= 0.0031308f ? 12.92f * l : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
            out[4 * i + k] = (uint8_t)(s * 255.0f + 0.5f);
        }
        out[4 * i + 3] = 255;
    }
}
