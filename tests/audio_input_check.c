/* No game data or physical microphone: exercise conversion, bounded capture and the polling ABI. */
#include "../runtime/audio_capture.c"
#include "../runtime/ov_audio_input.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static SDL_Mutex *registry_lock;
void plat_mutex_lock(PlatMutex *m) {
    (void)m;
    SDL_LockMutex(registry_lock);
}
void plat_mutex_unlock(PlatMutex *m) {
    (void)m;
    SDL_UnlockMutex(registry_lock);
}
void *ipc_guest(uint64_t addr, uint64_t size) {
    assert(addr && size);
    return (void *)(uintptr_t)addr;
}
void rt_log(const char *fmt, ...) { (void)fmt; }

static void ring_check(void) {
    assert(SDL_InitSubSystem(SDL_INIT_AUDIO));
    MicCapture *m = calloc(1, sizeof *m);
    m->bpf = 2;
    m->cap = 16;
    m->ring = malloc(m->cap);
    SDL_AudioSpec spec = {.format = SDL_AUDIO_S16LE, .channels = 1, .freq = 48000};
    m->stream = SDL_CreateAudioStream(&spec, &spec);
    assert(m->stream);
    assert(SDL_SetAudioStreamPutCallback(m->stream, received, m));
    int16_t first[] = {1, 2, 3, 4, 5, 6, 7, 8}, second[] = {9, 10, 11, 12, 13, 14, 15, 16}, out[16] = {0};
    assert(SDL_PutAudioStreamData(m->stream, first, sizeof first));
    assert(mic_available(m) == 8);
    assert(mic_read(m, out, 3) == 3);
    assert(out[0] == 1 && out[2] == 3);
    assert(SDL_PutAudioStreamData(m->stream, second, sizeof second));
    assert(mic_available(m) == 8); /* oldest unread samples were dropped */
    assert(mic_read(m, out, 16) == 8);
    assert(memcmp(out, second, sizeof second) == 0);
    assert(mic_read(m, out, -1) == 0);
    assert(SDL_PutAudioStreamData(m->stream, first, sizeof first));
    mic_purge(m);
    assert(mic_available(m) == 0);
    mic_close(m);
}

static void polling_check(void) {
    uint64_t opaque[128] = {0};
    int format[3] = {2, 1, 48000};
    int16_t samples[4800];
    Cpu c = {0};
    uint64_t key = (uintptr_t)opaque;
    for (int repeat = 0; repeat < 3; repeat++) {
        c.x[0] = key;
        c.x[4] = (uintptr_t)format;
        c.x[5] = 4800;
        open_input_locked(&c);
        assert(c.x[0] == 0);
        c.x[0] = key;
        state_locked(&c);
        assert(c.x[0] == 1);
        c.x[0] = key;
        start_input_locked(&c);
        assert(c.x[0] == 0);
        c.x[0] = key;
        state_locked(&c);
        assert(c.x[0] == 2);
        SDL_Delay(350); /* overflow the 100 ms ring, not system memory */
        c.x[0] = key;
        filled_locked(&c);
        assert(c.x[0] > 0 && c.x[0] <= 4800);
        c.x[0] = key;
        c.x[1] = (uintptr_t)samples;
        c.x[2] = 4800;
        read_input_locked(&c);
        assert(c.x[0] > 0 && c.x[0] <= 4800);
        c.x[0] = key;
        stop_input_locked(&c);
        c.x[0] = key;
        purge_input_locked(&c);
        c.x[0] = key;
        filled_locked(&c);
        assert(c.x[0] == 0);
        c.x[0] = key;
        close_input_locked(&c);
        assert(opaque[0] == 0);
        c.x[0] = key;
        purge_input_locked(&c); /* regression: Purge after failed/closed Open */
    }
    format[0] = 99;
    c.x[0] = key;
    c.x[4] = (uintptr_t)format;
    c.x[5] = 4800;
    open_input_locked(&c);
    assert(c.x[0] == BAD_INPUT);
    c.x[0] = key;
    purge_input_locked(&c);
    c.x[0] = key;
    state_locked(&c);
    assert(c.x[0] == 0);
    c.x[0] = key;
    c.x[1] = (uintptr_t)samples;
    c.x[2] = 4800;
    read_input_locked(&c);
    assert(c.x[0] == 0);
    unsigned char stats[32];
    memset(stats, 0xa5, sizeof stats);
    c.x[8] = (uintptr_t)stats;
    statistics_locked(&c);
    for (int i = 0; i < 24; i++)
        assert(stats[i] == 0);
    for (int i = 24; i < 32; i++)
        assert(stats[i] == 0xa5);
    mic_stop(NULL);
    mic_purge(NULL);
    mic_close(NULL);
    assert(mic_available(NULL) == 0);
}

int main(void) {
    assert(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"));
    registry_lock = SDL_CreateMutex();
    assert(registry_lock);
    ring_check();
    polling_check();
    SDL_DestroyMutex(registry_lock);
    puts("PASS: sample order, bounded overflow, polling frame units, reopen, purge and failed-open regression");
    return 0;
}
