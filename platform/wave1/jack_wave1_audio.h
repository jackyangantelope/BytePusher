#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool jack_wave1_audio_begin(unsigned sample_rate);
size_t jack_wave1_audio_submit(const int16_t *samples, size_t frames);
bool jack_wave1_audio_wait(void);
void jack_wave1_audio_mute(bool mute);
void jack_wave1_audio_report(void);
#ifdef __cplusplus
}
#endif
