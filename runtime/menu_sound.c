// The settings menu's sounds: the game's own interface sounds (Sound/Resource/Stream/SE_Sys_*.bwav in the user's RomFS:
// cursor, decide, cancel, the Plus menu's open and close, the tab moves), decoded once and mixed over the game's output
// in the audio device callback (audio_mixer.c sdl_pull). Nothing is shipped: a RomFS without them stays silent.
//
// BWAV: "BWAV", byte order mark, version, CRC32, a prefetch flag (u16) and the channel count (u16), then 0x4C bytes per
// channel: codec (u16, 0 PCM16, 1 DSP-ADPCM), pan (u16, 0 left, 1 right, 2 middle), sample rate, sample count (u32),
// the count again, 16 DSP coefficients (s16), the absolute offset of the samples (u32), ... The interface sounds are
// 48 kHz DSP-ADPCM: 8-byte frames of a header (coefficient pair index, scale exponent) and 14 four-bit samples.
//
// Menu thread: menu_sound_play starts a voice (a free one, else the oldest). Audio thread: menu_sound_mix adds the
// voices to a block. A voice is free while its position is negative; the menu thread sets its sound first and its
// position last (release), the audio thread reads them in the other order (acquire).
//
// NX2NAVI_MENU_SOUND_VOLUME (menu_sound_volume, 0 to 100, default 80; 0 is silent): the level, under the output volume.
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "menu_sound.h"
#include "platform/platform.h"

typedef struct {
    float *pcm;          // interleaved stereo at 48 kHz
    uint32_t frames;
} Sound;

static const char *const kFiles[MENU_SOUND_COUNT] = {
    [MENU_SOUND_OPEN] = "Sound/Resource/Stream/SE_Sys_PlusMenu_Open00.bwav",
    [MENU_SOUND_CLOSE] = "Sound/Resource/Stream/SE_Sys_PlusMenu_Close00.bwav",
    [MENU_SOUND_CURSOR] = "Sound/Resource/Stream/SE_Sys_Cursor00.bwav",
    [MENU_SOUND_DECIDE] = "Sound/Resource/Stream/SE_Sys_Do00.bwav",
    [MENU_SOUND_CANCEL] = "Sound/Resource/Stream/SE_Sys_Cancel00.bwav",
    [MENU_SOUND_TAB_LEFT] = "Sound/Resource/Stream/SE_Sys_MinusMenu_TabMove_L00.bwav",
    [MENU_SOUND_TAB_RIGHT] = "Sound/Resource/Stream/SE_Sys_MinusMenu_TabMove_R00.bwav",
};

static Sound sounds[MENU_SOUND_COUNT];
static atomic_int loaded;   // 0 not yet, 1 loading or done (sounds[] written before the release store of 2), 2 ready

#define VOICES 8
static struct {
    atomic_int pos;      // frame, -1 free
    atomic_int sound;
    uint64_t started;
} voices[VOICES];
static uint64_t started_n;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

// One channel into dst[2 * i + ch] (ch 0 or 1; mono pan writes both). 0 when the data does not fit the file.
static int decode_channel(const uint8_t *b, uint64_t size, const uint8_t *info, float *dst, uint32_t frames) {
    uint16_t codec = u16(info), pan = u16(info + 2);
    uint32_t count = u32(info + 8), off = u32(info + 0x30);
    if (count > frames) count = frames;
    int16_t coef[16];
    for (int i = 0; i < 16; i++) coef[i] = (int16_t)u16(info + 0x10 + 2 * i);
    float scale = 1.0f / 32768.0f;
    if (codec == 0) {   // PCM16
        if ((uint64_t)off + (uint64_t)count * 2 > size) return 0;
        for (uint32_t i = 0; i < count; i++) {
            float v = (int16_t)u16(b + off + 2 * i) * scale;
            if (pan != 1) dst[2 * i] += v;
            if (pan != 0) dst[2 * i + 1] += v;
        }
        return 1;
    }
    if (codec != 1) return 0;
    uint64_t need = ((uint64_t)count + 13) / 14 * 8;
    if ((uint64_t)off + need > size) return 0;
    int32_t h1 = 0, h2 = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *fr = b + off + (i / 14) * 8;
        uint32_t k = i % 14;
        int idx = fr[0] >> 4 & 7, sh = fr[0] & 15;
        int nib = fr[1 + k / 2] >> (k & 1 ? 0 : 4) & 15;
        if (nib >= 8) nib -= 16;
        int32_t s = ((nib << sh) << 11) + coef[2 * idx] * h1 + coef[2 * idx + 1] * h2;
        int16_t v = clamp16((s + 1024) >> 11);
        h2 = h1, h1 = v;
        if (pan != 1) dst[2 * i] += v * scale;
        if (pan != 0) dst[2 * i + 1] += v * scale;
    }
    return 1;
}

static void load_one(int id) {
    uint64_t size = 0;
    uint8_t *b = romfs_load(kFiles[id], &size);
    if (!b) return;
    if (size < 0x10 || memcmp(b, "BWAV", 4)) { free(b); return; }
    uint16_t nch = u16(b + 0x0e);
    if (!nch || nch > 2 || 0x10 + (uint64_t)nch * 0x4c > size) { free(b); return; }
    uint32_t rate = u32(b + 0x10 + 4), frames = u32(b + 0x10 + 8);
    if (rate != 48000 || !frames || frames > 48000 * 10) {   // the interface sounds are 48 kHz and short
        rt_log("menu_sound: %s: %u Hz, %u samples: not played", kFiles[id], rate, frames);
        free(b);
        return;
    }
    float *pcm = calloc((size_t)frames * 2, sizeof *pcm);
    int ok = pcm != NULL;
    for (uint16_t c = 0; ok && c < nch; c++) ok = decode_channel(b, size, b + 0x10 + c * 0x4c, pcm, frames);
    free(b);
    if (!ok) { free(pcm); return; }
    if (nch == 1) for (uint32_t i = 0; i < frames * 2; i++) pcm[i] *= 0.7071f;   // a middle-panned mono sound in both
    sounds[id] = (Sound){pcm, frames};
}

static void load_all(void) {
    int expect = 0;
    if (!atomic_compare_exchange_strong(&loaded, &expect, 1)) return;
    int n = 0;
    for (int i = 0; i < MENU_SOUND_COUNT; i++) {
        load_one(i);
        n += sounds[i].pcm != NULL;
    }
    for (int i = 0; i < VOICES; i++) atomic_store(&voices[i].pos, -1);
    rt_log("menu_sound: %d of %d interface sounds from the RomFS", n, MENU_SOUND_COUNT);
    atomic_store_explicit(&loaded, 2, memory_order_release);
}

// The menu's level times the output volume (NX2NAVI_VOLUME, as the game's sound has it).
static float volume(void) {
    const char *v = cfg_value("NX2NAVI_MENU_SOUND_VOLUME");
    int n = v && *v ? atoi(v) : 80;
    double master = cfg_double("NX2NAVI_VOLUME");
    if (master < 0) master = 0;
    if (master > 100) master = 100;
    return n <= 0 ? 0.0f : (float)((n >= 100 ? 1.0 : n / 100.0) * master / 100.0);
}

static _Atomic uint64_t last_start_ns;

// The least time between a cursor tick and the sound before it. Moving down a list or changing a value asks for a tick
// at every step, about 20 a second while a direction is held, and the focus moving with a press or a tab change is part
// of that sound: at most five ticks a second are heard.
#define CURSOR_GAP_NS 180000000ull
#define CURSOR_GAIN 0.6f   // the tick under the sounds of a decision

void menu_sound_play(MenuSound id) {
    if ((unsigned)id >= MENU_SOUND_COUNT || volume() <= 0) return;
    uint64_t now = plat_now_ns();
    if (id == MENU_SOUND_CURSOR && now - atomic_load(&last_start_ns) < CURSOR_GAP_NS) return;
    atomic_store(&last_start_ns, now);
    if (atomic_load_explicit(&loaded, memory_order_acquire) != 2) load_all();
    if (atomic_load_explicit(&loaded, memory_order_acquire) != 2 || !sounds[id].pcm) return;
    int pick = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < VOICES; i++) {
        if (atomic_load_explicit(&voices[i].pos, memory_order_acquire) < 0) { pick = i; break; }
        if (voices[i].started < oldest) oldest = voices[i].started, pick = i;
    }
    // A sound that replaces itself restarts instead of stacking up, and a decision replaces the tick of the same press
    // (a row changes its value in the frame the press is seen).
    for (int i = 0; i < VOICES; i++)
        if (atomic_load_explicit(&voices[i].pos, memory_order_acquire) >= 0 &&
            (atomic_load(&voices[i].sound) == (int)id || atomic_load(&voices[i].sound) == MENU_SOUND_CURSOR) &&
            atomic_load(&voices[i].pos) < 2400) {   // started under 50 ms ago
            pick = i;
            break;
        }
    atomic_store_explicit(&voices[pick].pos, -1, memory_order_release);
    atomic_store_explicit(&voices[pick].sound, (int)id, memory_order_relaxed);
    voices[pick].started = __atomic_add_fetch(&started_n, 1, __ATOMIC_RELAXED);
    atomic_store_explicit(&voices[pick].pos, 0, memory_order_release);
}

void menu_sound_mix(float *out, uint32_t frames, int channels, int rate) {
    if (atomic_load_explicit(&loaded, memory_order_acquire) != 2 || rate != 48000 || channels < 2) return;
    float g = -1;
    for (int v = 0; v < VOICES; v++) {
        int pos = atomic_load_explicit(&voices[v].pos, memory_order_acquire);
        if (pos < 0) continue;
        int which = atomic_load_explicit(&voices[v].sound, memory_order_acquire);
        const Sound *s = &sounds[which];
        // menu_sound_play may restart the voice between the two loads: pos would then belong to the old sound, past
        // the end of a shorter new one; such a voice is mixed from the next call on
        if (atomic_load_explicit(&voices[v].pos, memory_order_acquire) != pos || (uint32_t)pos >= s->frames) continue;
        if (g < 0) g = volume();
        float vg = which == MENU_SOUND_CURSOR ? g * CURSOR_GAIN : g;
        uint32_t n = s->frames - (uint32_t)pos < frames ? s->frames - (uint32_t)pos : frames;
        const float *src = s->pcm + (size_t)pos * 2;
        for (uint32_t i = 0; i < n; i++) {
            out[(size_t)i * channels] += src[2 * i] * vg;
            out[(size_t)i * channels + 1] += src[2 * i + 1] * vg;
        }
        int next = pos + (int)n;
        // Only advance if the menu thread did not restart the voice meanwhile.
        atomic_compare_exchange_strong(&voices[v].pos, &pos, next >= (int)s->frames ? -1 : next);
    }
}

void menu_sound_preload(void) {
    if (atomic_load_explicit(&loaded, memory_order_acquire) == 0) load_all();
}
