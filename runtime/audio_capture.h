#pragma once
#include <stdint.h>
typedef struct MicCapture MicCapture;
int mic_device_count(void);
MicCapture *mic_open(int format, int channels, int rate, int frames);
int mic_start(MicCapture *m);
void mic_stop(MicCapture *m);
void mic_close(MicCapture *m);
void mic_purge(MicCapture *m);
int mic_available(MicCapture *m);
int mic_read(MicCapture *m, void *out, int frames);
int mic_capacity(MicCapture *m);
void mic_gain(MicCapture *m, float gain);
const char *mic_error(void);
