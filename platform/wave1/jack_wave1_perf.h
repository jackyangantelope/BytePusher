#pragma once
#include <cstdio>
#include "pico/stdlib.h"
#include "jack_wave1_runtime.h"

#ifndef JACK_WAVE1_PERF_INTERVAL_US
#define JACK_WAVE1_PERF_INTERVAL_US 1000000u
#endif

// Sample only from the foreground thread.
struct JackWave1Perf {
    uint32_t start = 0, video = 0, frames = 0;
    uint64_t core = 0, draw = 0, wait = 0;
    void begin() { start = time_us_32(); video = jack_wave1_frame_counter(); }
    void sample(const char *name, uint32_t core_us, uint32_t draw_us,
                uint32_t wait_us, uint16_t buttons, const char *state,
                void (*sink)(const char *)) {
        ++frames; core += core_us; draw += draw_us; wait += wait_us;
        const uint32_t now = time_us_32(), elapsed = now - start;
        if (elapsed < JACK_WAVE1_PERF_INTERVAL_US) return;
        const uint32_t current_video = jack_wave1_frame_counter();
        const uint32_t fps100 = static_cast<uint32_t>(uint64_t(frames) * 100000000u / elapsed);
        const uint32_t video_fps100 = static_cast<uint32_t>(uint64_t(current_video - video) * 100000000u / elapsed);
        char line[320];
        snprintf(line, sizeof(line),
            "%s_PERF state=%s updates=%lu elapsed_us=%lu fps=%lu.%02lu video_fps=%lu.%02lu video_frames=%lu core_mean_us=%lu draw_mean_us=%lu wait_mean_us=%lu buttons=%04x usb_reports=%lu\n",
            name, state, (unsigned long)frames, (unsigned long)elapsed,
            (unsigned long)(fps100 / 100), (unsigned long)(fps100 % 100),
            (unsigned long)(video_fps100 / 100), (unsigned long)(video_fps100 % 100),
            (unsigned long)(current_video - video), (unsigned long)(core / frames),
            (unsigned long)(draw / frames), (unsigned long)(wait / frames), buttons,
            (unsigned long)jack_wave1_usb_report_count());
        sink(line);
        start = now; video = current_video; frames = 0; core = draw = wait = 0;
    }
};
inline void jack_wave1_perf_stdout(const char *line) { printf("%s", line); }
