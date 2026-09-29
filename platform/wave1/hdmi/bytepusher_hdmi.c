
#include "hstx.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "jack_wave1_audio.h"
#include "jack_wave1_runtime.h"
#include "jack_pcm_ring.h"
#include <stdio.h>

#if defined(JACK_MEGADUCK_CONTINUOUS031) || defined(JACK_WAVE1_CONTINUOUS_SILENCE)
#define JACK_WAVE1_CONTINUOUS_TRANSPORT 1
#endif

#ifdef JACK_MEGADUCK_CORE1_PROBE026
#include "hardware/exception.h"
#include "hardware/structs/scb.h"
#endif

static uint16_t framebuffer[320 * 240] __attribute__((aligned(4)));
static jack_pcm_ring_t pcm;
static _Atomic bool producer_started, muted;
#ifndef JACK_WAVE1_CONTINUOUS_TRANSPORT
static bool consumer_started;
#endif
static int audio_frame_counter;
static uint32_t submitted, dropped, nonzero, max_abs, wait_timeouts;
static _Atomic uint32_t encoded;
static uint32_t last_submit, last_drop, last_nonzero, last_encoded, last_under;
static uint32_t report_start;
#ifdef JACK_WAVE1_CONTINUOUS_TRANSPORT
static _Atomic uint32_t audio031_padded, audio031_total, audio031_push_failures;
static uint32_t audio031_last_padded, audio031_last_total;
#endif
#if defined(JACK_MEGADUCK_AUDIO034)

#define JACK_AUDIO034_TARGET_PCM 2940u
#define JACK_AUDIO034_WAIT_HIGH (JACK_AUDIO034_TARGET_PCM + 896u)
#define JACK_AUDIO034_STAGE_CAPACITY 2048u
static int16_t audio034_stage[JACK_AUDIO034_STAGE_CAPACITY * 2u];
static uint32_t audio034_stage_frames;
static uint32_t audio034_source_frames, audio034_output_frames;
static uint32_t audio034_prime_silence, audio034_frame_flushes;
static uint32_t audio034_batch_calls, audio034_clamped, audio034_overflow;
static uint32_t audio034_wire_frames, audio034_last_wire_packets;
static uint32_t audio034_report_due;
static uint32_t audio034_last_source, audio034_last_output;
static uint32_t audio034_last_prime, audio034_last_flushes;
static uint32_t audio034_last_batches, audio034_last_clamped;
static uint32_t audio034_last_overflow, audio034_last_wire;
#elif defined(JACK_MEGADUCK_AUDIO033)

#define JACK_AUDIO033_TARGET_PCM 1470u
#define JACK_AUDIO033_WAIT_HIGH (JACK_AUDIO033_TARGET_PCM + 896u)
static bool audio033_primed;
static uint32_t audio033_last_level;
static uint32_t audio033_source_frames, audio033_output_frames;
static uint32_t audio033_prime_silence, audio033_stretch_up;
static uint32_t audio033_stretch_down, audio033_clamped;
static uint32_t audio033_last_source, audio033_last_output;
static uint32_t audio033_last_prime, audio033_last_up;
static uint32_t audio033_last_down, audio033_last_clamped;
#endif
volatile bool HSTX_vblank;
extern uint32_t __StackOneBottom[], __StackOneTop[];

#ifdef JACK_MEGADUCK_STACK_ISOLATE030
#define MEGADUCK_CORE1_STACK_WORDS 2048u
#define MEGADUCK_STACK_PATTERN 0x5a17c0deu
#define MEGADUCK_STACK_GUARD_WORDS 32u
uint32_t megaduck_core1_stack[MEGADUCK_CORE1_STACK_WORDS]
    __attribute__((aligned(32), used));
static volatile uint32_t jack_core1_stack_limit;
static volatile uint32_t jack_core1_fault_stage;
#endif

#ifdef JACK_MEGADUCK_CORE1_PROBE026

static _Atomic uint32_t jack_core1_probe_stage;
static _Atomic uint32_t jack_core1_probe_heartbeat;
static _Atomic uint32_t jack_core1_probe_packets;
static _Atomic uint32_t jack_core1_probe_packet_sets;
static _Atomic uint32_t jack_core1_probe_islands;
static _Atomic uint32_t jack_core1_probe_pushes;
static uint32_t jack_core1_probe_last_heartbeat;
static volatile uint32_t jack_core1_fault_core = UINT32_MAX;
static volatile uint32_t jack_core1_fault_sp;
static volatile uint32_t jack_core1_fault_exc_return;
static volatile uint32_t jack_core1_fault_pc;
static volatile uint32_t jack_core1_fault_lr;
static volatile uint32_t jack_core1_fault_xpsr;
static volatile uint32_t jack_core1_fault_cfsr;
static volatile uint32_t jack_core1_fault_hfsr;
static volatile uint32_t jack_core1_fault_bfar;
static volatile uint32_t jack_core1_fault_mmfar;

#ifdef JACK_MEGADUCK_STACK_ISOLATE030
static inline uint32_t *jack_core1_stack_bottom(void)
{
    return megaduck_core1_stack;
}
static inline uint32_t *jack_core1_stack_top(void)
{
    return megaduck_core1_stack + MEGADUCK_CORE1_STACK_WORDS;
}
#else
static inline uint32_t *jack_core1_stack_bottom(void) { return __StackOneBottom; }
static inline uint32_t *jack_core1_stack_top(void) { return __StackOneTop; }
#endif

#if defined(JACK_MEGADUCK_LOW_NOISE029) || defined(JACK_MEGADUCK_STACK_ISOLATE030)
static inline void jack_core1_stage_set(uint32_t value)
{
    if (atomic_load_explicit(&jack_core1_probe_stage, memory_order_relaxed) != value)
        atomic_store_explicit(&jack_core1_probe_stage, value, memory_order_relaxed);
}
static inline void jack_core1_probe_increment(_Atomic uint32_t *value)
{
    atomic_store_explicit(value,
        atomic_load_explicit(value, memory_order_relaxed) + 1u,
        memory_order_relaxed);
}
#endif

void __not_in_flash_func(jack_core1_fault_capture)(uint32_t *stack,
                                                   uint32_t exc_return)
    __attribute__((noreturn, used));
void __not_in_flash_func(jack_core1_fault_capture)(uint32_t *stack,
                                                   uint32_t exc_return)
{
    uintptr_t sp = (uintptr_t)stack;
#ifdef JACK_MEGADUCK_STACK_ISOLATE030
    jack_core1_fault_stage = atomic_load_explicit(&jack_core1_probe_stage,
                                                  memory_order_relaxed);
#endif
    jack_core1_fault_core = get_core_num();
    jack_core1_fault_sp = (uint32_t)sp;
    jack_core1_fault_exc_return = exc_return;
    if (sp >= (uintptr_t)jack_core1_stack_bottom() &&
        sp + 8u * sizeof(uint32_t) <= (uintptr_t)jack_core1_stack_top()) {
        jack_core1_fault_lr = stack[5];
        jack_core1_fault_pc = stack[6];
        jack_core1_fault_xpsr = stack[7];
    }
    jack_core1_fault_cfsr = scb_hw->cfsr;
    jack_core1_fault_hfsr = scb_hw->hfsr;
    jack_core1_fault_bfar = scb_hw->bfar;
    jack_core1_fault_mmfar = scb_hw->mmfar;
    atomic_store_explicit(&jack_core1_probe_stage, 0xf0000000u,
                          memory_order_release);
    for (;;) tight_loop_contents();
}

static void __attribute__((naked)) jack_core1_hardfault(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b jack_core1_fault_capture\n");
}

static void jack_video_core1_entry(void)
{
#ifdef JACK_MEGADUCK_STACK_ISOLATE030
    uint32_t limit = (uint32_t)(uintptr_t)(megaduck_core1_stack +
                                           MEGADUCK_STACK_GUARD_WORDS);
    __asm volatile("msr msplim, %0\n"
                   "isb\n" : : "r"(limit) : "memory");
    jack_core1_stack_limit = limit;
#endif
    atomic_store_explicit(&jack_core1_probe_stage, 1u, memory_order_release);
#ifndef JACK_MEGADUCK_STACK_FIX032
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, jack_core1_hardfault);
#endif
    atomic_store_explicit(&jack_core1_probe_stage, 2u, memory_order_release);
    video_output_core1_run();
}

#if defined(JACK_MEGADUCK_LOW_NOISE029) || defined(JACK_MEGADUCK_STACK_ISOLATE030)
#define JACK_CORE1_STAGE(value) jack_core1_stage_set(value)
#define JACK_CORE1_INCREMENT(value) jack_core1_probe_increment(&(value))
#else
#define JACK_CORE1_STAGE(value) \
    atomic_store_explicit(&jack_core1_probe_stage, (value), memory_order_release)
#define JACK_CORE1_INCREMENT(value) \
    atomic_fetch_add_explicit(&(value), 1u, memory_order_relaxed)
#endif
#else
#define JACK_CORE1_STAGE(value) ((void)0)
#define JACK_CORE1_INCREMENT(value) ((void)0)
#endif

uint8_t *hstx_getframebuffer(void) { return (uint8_t *)framebuffer; }
uint16_t *hstx_getlineFromFramebuffer(int line) { return framebuffer + line * 320; }
uint32_t hstx_getframecounter(void) { return video_frame_count; }
void hstx_waitForVSync(void) {
    uint32_t frame = video_frame_count;
    while (frame == video_frame_count) tight_loop_contents();
}
static void __not_in_flash_func(scanline)(uint32_t v, uint32_t active, uint32_t *out)
{
    (void)active;
    int line = (int)v - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    if (line < 0 || line >= 480) return;
    const uint16_t *src = framebuffer + (line >> 1) * 320;
    for (unsigned x = 0; x < 320; x++) {
        uint32_t p = src[x] & 0x7fffu;
        out[x] = p | (p << 16);
    }
}
#if defined(JACK_MEGADUCK_DIRECT_BACKGROUND028) || defined(JACK_MEGADUCK_LOW_NOISE029) || defined(JACK_MEGADUCK_STACK_ISOLATE030)
void __attribute__((noinline)) __not_in_flash_func(audio_pump)(void)
#else
static void __not_in_flash_func(audio_pump)(void)
#endif
{
#ifndef JACK_WAVE1_CONTINUOUS_TRANSPORT
    if (!atomic_load_explicit(&producer_started, memory_order_acquire)) {
        JACK_CORE1_STAGE(11u);
        return;
    }
    if (!consumer_started) {
        JACK_CORE1_STAGE(12u);
        if (jack_pcm_level(&pcm) < 1024) return;
        consumer_started = true;
        JACK_CORE1_STAGE(10u);
    }
#endif

    for (unsigned packet_index = 0; packet_index < 8; packet_index++) {
        JACK_CORE1_STAGE(20u);
#ifdef JACK_WAVE1_CONTINUOUS_TRANSPORT
        if (hstx_di_queue_get_level() >= 96) return;
        unsigned real_frames = 0;
#else
        if (hstx_di_queue_get_level() >= 96 || jack_pcm_level(&pcm) < 4) return;
#endif
        JACK_CORE1_INCREMENT(jack_core1_probe_packets);
        audio_sample_t samples[4];
        JACK_CORE1_STAGE(21u);
        for (unsigned i = 0; i < 4; i++) {
#ifdef JACK_WAVE1_CONTINUOUS_TRANSPORT
            uint32_t packed = 0;
            if (atomic_load_explicit(&producer_started, memory_order_acquire) &&
                jack_pcm_pop(&pcm, &packed)) real_frames++;
#else
            uint32_t packed;
            if (!jack_pcm_pop(&pcm, &packed)) return;
#endif
            samples[i].left = (int16_t)packed;
            samples[i].right = (int16_t)(packed >> 16);
            if (atomic_load_explicit(&muted, memory_order_relaxed))
                samples[i].left = samples[i].right = 0;
        }
        hstx_packet_t packet;
        hstx_data_island_t island;
        JACK_CORE1_STAGE(22u);
        int next = hstx_packet_set_audio_samples_cs(&packet, samples, 4, audio_frame_counter);
        JACK_CORE1_INCREMENT(jack_core1_probe_packet_sets);
        JACK_CORE1_STAGE(23u);
        hstx_encode_data_island(&island, &packet, false, true);
        JACK_CORE1_INCREMENT(jack_core1_probe_islands);
        JACK_CORE1_STAGE(24u);
        if (hstx_di_queue_push(&island)) {
            audio_frame_counter = next;
#ifdef JACK_WAVE1_CONTINUOUS_TRANSPORT

            atomic_store_explicit(&encoded,
                atomic_load_explicit(&encoded, memory_order_relaxed) + real_frames,
                memory_order_relaxed);
#ifdef JACK_MEGADUCK_CONTINUOUS031

            jack_core1_probe_increment(&audio031_total);
#else
            atomic_fetch_add_explicit(&audio031_total, 1u, memory_order_relaxed);
#endif
            atomic_store_explicit(&audio031_padded,
                atomic_load_explicit(&audio031_padded, memory_order_relaxed) + 4u - real_frames,
                memory_order_relaxed);
#else
            atomic_fetch_add_explicit(&encoded, 4, memory_order_relaxed);
#endif
            JACK_CORE1_INCREMENT(jack_core1_probe_pushes);
#if defined(JACK_MEGADUCK_CONTINUOUS031)

            uint32_t total = atomic_load_explicit(&audio031_total, memory_order_relaxed);
            if ((total & 63u) == 0u)
                atomic_store_explicit(&jack_core1_probe_heartbeat, total,
                                      memory_order_relaxed);
#elif defined(JACK_MEGADUCK_LOW_NOISE029) || defined(JACK_MEGADUCK_STACK_ISOLATE030)
            if (((uint32_t)next & 0xffu) == 0u)
                atomic_store_explicit(&jack_core1_probe_heartbeat,
                                      (uint32_t)next, memory_order_relaxed);
#elif defined(JACK_MEGADUCK_CORE1_PROBE026)
            atomic_fetch_add_explicit(&jack_core1_probe_heartbeat, 1u,
                                      memory_order_relaxed);
#endif
#ifdef JACK_WAVE1_CONTINUOUS_TRANSPORT
        } else {
#ifdef JACK_MEGADUCK_CONTINUOUS031
            jack_core1_probe_increment(&audio031_push_failures);
#else
            atomic_fetch_add_explicit(&audio031_push_failures, 1u,
                                      memory_order_relaxed);
#endif
#endif
        }
    }
    JACK_CORE1_STAGE(25u);
}
void hstx_init(bool dvi_only)
{
    video_output_set_dvi_mode(dvi_only);
    hstx_di_queue_init();
    video_output_init(640, 480);
    pico_hdmi_set_audio_sample_rate(44100);
#ifdef JACK_WAVE1_CONTINUOUS_TRANSPORT

    // Prime HDMI packets before Core 1 starts consuming them.
    for (unsigned i = 0; i < 12; ++i) audio_pump();
#ifdef JACK_WAVE1_CONTINUOUS_SILENCE
    printf("WAVE1_AUDIO init mode=continuous-silence prefill_packets=%lu rate=44100\n",
           (unsigned long)hstx_di_queue_get_level());
#else
    printf("MEGADUCK_AUDIO031 init mode=continuous-cs prefill_packets=%lu rate=44100\n",
           (unsigned long)hstx_di_queue_get_level());
#endif
#endif
#ifdef JACK_MEGADUCK_AUDIO034
    printf("MEGADUCK_AUDIO034 init mode=frame-linear target_pcm=%u telemetry=staggered infoframe=stream-header\n",
           JACK_AUDIO034_TARGET_PCM);
#elif defined(JACK_MEGADUCK_AUDIO033)
    printf("MEGADUCK_AUDIO033 init mode=adaptive-linear target_pcm=%u infoframe=stream-header\n",
           JACK_AUDIO033_TARGET_PCM);
#endif
    video_output_set_scanline_callback(scanline);
    video_output_set_background_task(audio_pump);
#ifdef JACK_MEGADUCK_CORE1_PROBE026
#ifdef JACK_MEGADUCK_STACK_ISOLATE030
    for (unsigned i = 0; i < MEGADUCK_CORE1_STACK_WORDS; ++i)
        megaduck_core1_stack[i] = MEGADUCK_STACK_PATTERN;
    multicore_launch_core1_with_stack(jack_video_core1_entry,
        megaduck_core1_stack, sizeof(megaduck_core1_stack));
    printf("MEGADUCK_CORE1_STACK init bottom=%08lx top=%08lx bytes=%lu guard_words=%u\n",
        (unsigned long)(uintptr_t)megaduck_core1_stack,
        (unsigned long)(uintptr_t)(megaduck_core1_stack + MEGADUCK_CORE1_STACK_WORDS),
        (unsigned long)sizeof(megaduck_core1_stack),
        (unsigned)MEGADUCK_STACK_GUARD_WORDS);
#else
    multicore_launch_core1_with_stack(jack_video_core1_entry, __StackOneBottom,
        (uintptr_t)__StackOneTop - (uintptr_t)__StackOneBottom);
#endif
#else
    multicore_launch_core1_with_stack(video_output_core1_run, __StackOneBottom,
        (uintptr_t)__StackOneTop - (uintptr_t)__StackOneBottom);
#endif
    printf("WAVE1_HDMI ready rate=44100 channels=2 bits=16 pcm_frames=4096 di_packets=128\n");
}
bool jack_wave1_audio_begin(unsigned rate)
{
    if (rate != 44100) return false;
    last_under = hstx_di_queue_get_underrun_count();
    report_start = time_us_32();
#ifdef JACK_WAVE1_CONTINUOUS_TRANSPORT
    audio031_last_total = atomic_load_explicit(&audio031_total, memory_order_relaxed);
    audio031_last_padded = atomic_load_explicit(&audio031_padded, memory_order_relaxed);
#endif
#ifdef JACK_MEGADUCK_AUDIO034
    audio034_stage_frames = 0;
    audio034_last_wire_packets =
        atomic_load_explicit(&audio031_total, memory_order_relaxed);
    audio034_report_due = report_start + 2500000u;
    for (uint32_t i = 0; i < JACK_AUDIO034_TARGET_PCM; ++i) {
        if (jack_pcm_push(&pcm, 0)) audio034_output_frames++;
        else dropped++;
    }
    audio034_prime_silence += JACK_AUDIO034_TARGET_PCM;
#elif defined(JACK_MEGADUCK_AUDIO033)
    audio033_primed = false;
    audio033_last_level = jack_pcm_level(&pcm);
#endif
#ifdef JACK_WAVE1_DEFER_HDMI_AUDIO

    for (unsigned i = 0; i < 1024u; ++i) (void)jack_pcm_push(&pcm, 0u);
    atomic_store_explicit(&producer_started, true, memory_order_release);
    const uint32_t prime_start = time_us_32();
    while (hstx_di_queue_get_level() < 16u &&
           (uint32_t)(time_us_32() - prime_start) < 100000u) {
        tight_loop_contents();
    }
    const uint32_t prime_packets = hstx_di_queue_get_level();
    video_output_set_dvi_mode(false);
    printf("WAVE1_HDMI deferred_switch packets=%lu wait_us=%lu\n",
           (unsigned long)prime_packets,
           (unsigned long)(time_us_32() - prime_start));
#else
    atomic_store_explicit(&producer_started, true, memory_order_release);
#endif
    return true;
}

#if defined(JACK_MEGADUCK_AUDIO034) || defined(JACK_MEGADUCK_AUDIO033)
static inline uint32_t jack_audio033_pack(int16_t left, int16_t right)
{
    return (uint16_t)left | ((uint32_t)(uint16_t)right << 16);
}

static inline int16_t jack_audio033_lerp(int16_t a, int16_t b, uint32_t frac)
{
    return (int16_t)((int32_t)a +
        (int32_t)(((int64_t)b - (int64_t)a) * (int64_t)frac >> 16));
}

static inline void jack_audio033_push(uint32_t packed)
{
#ifdef JACK_MEGADUCK_AUDIO034
    if (jack_pcm_push(&pcm, packed)) audio034_output_frames++;
#else
    if (jack_pcm_push(&pcm, packed)) audio033_output_frames++;
#endif
    else dropped++;
}

#ifdef JACK_MEGADUCK_AUDIO034
static void jack_audio034_accumulate(const int16_t *samples, size_t frames)
{
    audio034_batch_calls++;
    audio034_source_frames += (uint32_t)frames;
    uint32_t room = JACK_AUDIO034_STAGE_CAPACITY - audio034_stage_frames;
    uint32_t keep = frames < room ? (uint32_t)frames : room;
    for (uint32_t i = 0; i < keep; ++i) {
        audio034_stage[(audio034_stage_frames + i) * 2u] = samples[i * 2u];
        audio034_stage[(audio034_stage_frames + i) * 2u + 1u] = samples[i * 2u + 1u];
    }
    audio034_stage_frames += keep;
    audio034_overflow += (uint32_t)frames - keep;
}

static void jack_audio034_flush_frame(void)
{
    uint32_t packets = atomic_load_explicit(&audio031_total, memory_order_relaxed);
    audio034_wire_frames += (packets - audio034_last_wire_packets) * 4u;
    audio034_last_wire_packets = packets;

    uint32_t frames = audio034_stage_frames;
    if (!frames) return;
    uint32_t level = jack_pcm_level(&pcm);
    int32_t desired = (int32_t)JACK_AUDIO034_TARGET_PCM - (int32_t)level;
    uint32_t minimum = (frames + 1u) / 2u;
    uint32_t maximum = frames > JACK_PCM_CAPACITY / 4u ?
                       JACK_PCM_CAPACITY : frames * 4u;
    if (desired < (int32_t)minimum) {
        desired = (int32_t)minimum;
        audio034_clamped++;
    } else if (desired > (int32_t)maximum) {
        desired = (int32_t)maximum;
        audio034_clamped++;
    }
    uint32_t available = JACK_PCM_CAPACITY - level;
    if ((uint32_t)desired > available) {
        desired = (int32_t)available;
        audio034_clamped++;
    }
    uint32_t output = (uint32_t)desired;

    if (frames == 1u || output == 1u) {
        uint32_t packed = jack_audio033_pack(audio034_stage[0], audio034_stage[1]);
        for (uint32_t i = 0; i < output; ++i) jack_audio033_push(packed);
    } else if (output) {
        uint32_t step = (uint32_t)((((uint64_t)frames - 1u) << 16) /
                                   (output - 1u));
        uint32_t position = 0;
        for (uint32_t i = 0; i < output; ++i) {
            if (i + 1u == output) {
                jack_audio033_push(jack_audio033_pack(
                    audio034_stage[(frames - 1u) * 2u],
                    audio034_stage[(frames - 1u) * 2u + 1u]));
                continue;
            }
            uint32_t index = position >> 16;
            uint32_t frac = position & 0xffffu;
            int16_t left = jack_audio033_lerp(audio034_stage[index * 2u],
                audio034_stage[(index + 1u) * 2u], frac);
            int16_t right = jack_audio033_lerp(audio034_stage[index * 2u + 1u],
                audio034_stage[(index + 1u) * 2u + 1u], frac);
            jack_audio033_push(jack_audio033_pack(left, right));
            position += step;
        }
    }
    audio034_stage_frames = 0;
    audio034_frame_flushes++;
}
#else
static void jack_audio033_submit(const int16_t *samples, size_t frames)
{
    if (!frames) return;

    uint32_t level = jack_pcm_level(&pcm);
    uint32_t output = (uint32_t)frames;
    if (!audio033_primed) {

        uint32_t lead = 0;
        if (level < JACK_AUDIO033_TARGET_PCM &&
            frames < JACK_AUDIO033_TARGET_PCM - level)
            lead = JACK_AUDIO033_TARGET_PCM - level - (uint32_t)frames;
        for (uint32_t i = 0; i < lead; ++i) jack_audio033_push(0);
        audio033_prime_silence += lead;
        audio033_primed = true;
    } else {

        uint32_t consumed = audio033_last_level > level ?
                            audio033_last_level - level : 0u;
        int32_t correction = ((int32_t)JACK_AUDIO033_TARGET_PCM -
                              (int32_t)audio033_last_level) / 4;
        int32_t desired = (int32_t)consumed + correction;
        uint32_t minimum = ((uint32_t)frames + 1u) / 2u;
        uint32_t maximum = frames >= JACK_PCM_CAPACITY / 2u ?
                           JACK_PCM_CAPACITY : (uint32_t)frames * 2u;
        if (desired < (int32_t)minimum) {
            desired = (int32_t)minimum;
            audio033_clamped++;
        } else if (desired > (int32_t)maximum) {
            desired = (int32_t)maximum;
            audio033_clamped++;
        }
        output = (uint32_t)desired;
        if (output > frames) audio033_stretch_up++;
        else if (output < frames) audio033_stretch_down++;
    }

    if (frames == 1u || output == 1u) {
        uint32_t packed = jack_audio033_pack(samples[0], samples[1]);
        for (uint32_t i = 0; i < output; ++i) jack_audio033_push(packed);
    } else {
        uint32_t step = (uint32_t)((((uint64_t)frames - 1u) << 16) /
                                   (output - 1u));
        uint32_t position = 0;
        for (uint32_t i = 0; i < output; ++i) {
            if (i + 1u == output) {
                jack_audio033_push(jack_audio033_pack(
                    samples[(frames - 1u) * 2u],
                    samples[(frames - 1u) * 2u + 1u]));
                continue;
            }
            uint32_t index = position >> 16;
            uint32_t frac = position & 0xffffu;
            int16_t left = jack_audio033_lerp(samples[index * 2u],
                                              samples[(index + 1u) * 2u], frac);
            int16_t right = jack_audio033_lerp(samples[index * 2u + 1u],
                                               samples[(index + 1u) * 2u + 1u], frac);
            jack_audio033_push(jack_audio033_pack(left, right));
            position += step;
        }
    }
    audio033_source_frames += (uint32_t)frames;
    audio033_last_level = jack_pcm_level(&pcm);
}
#endif
#endif

size_t jack_wave1_audio_submit(const int16_t *samples, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        int left = samples[i * 2], right = samples[i * 2 + 1];
        submitted++;
        if (left || right) nonzero++;
        unsigned peak = (unsigned)(left < 0 ? -left : left);
        unsigned rp = (unsigned)(right < 0 ? -right : right);
        if (rp > peak) peak = rp;
        if (peak > max_abs) max_abs = peak;
#ifndef JACK_MEGADUCK_AUDIO033
        uint32_t packed = (uint16_t)left | ((uint32_t)(uint16_t)right << 16);
        if (!jack_pcm_push(&pcm, packed)) dropped++;
#endif
    }
#ifdef JACK_MEGADUCK_AUDIO034
    jack_audio034_accumulate(samples, frames);
#elif defined(JACK_MEGADUCK_AUDIO033)
    jack_audio033_submit(samples, frames);
#endif

    return frames;
}
void jack_wave1_audio_end_frame(void)
{
#ifdef JACK_MEGADUCK_AUDIO034
    jack_audio034_flush_frame();
#endif
}
bool jack_wave1_audio_wait(void)
{
    bool waited = false;
    uint32_t start = time_us_32(), usb_at = start;
#ifdef JACK_MEGADUCK_AUDIO034
    const uint32_t high_water = JACK_AUDIO034_WAIT_HIGH;
#elif defined(JACK_MEGADUCK_AUDIO033)
    const uint32_t high_water = JACK_AUDIO033_WAIT_HIGH;
#else
    const uint32_t high_water = 1470u;
#endif
    while (jack_pcm_level(&pcm) + hstx_di_queue_get_level() * 4u > high_water) {
        waited = true;
        uint32_t now = time_us_32();
        if (now - start > 50000u) { wait_timeouts++; break; }
        if (now - usb_at >= 1000u) { jack_wave1_usb_service(); usb_at = now; }
        tight_loop_contents();
    }
    return waited;
}
void jack_wave1_audio_mute(bool value) { atomic_store(&muted, value); }
void jack_wave1_audio_report(void)
{
    uint32_t now = time_us_32();
#ifdef JACK_MEGADUCK_AUDIO034
    if ((int32_t)(now - audio034_report_due) < 0) return;
    uint32_t under = hstx_di_queue_get_underrun_count();
    uint32_t sent = atomic_load_explicit(&encoded, memory_order_relaxed);
    uint32_t total031 = atomic_load_explicit(&audio031_total, memory_order_relaxed);
    uint32_t padded031 = atomic_load_explicit(&audio031_padded, memory_order_relaxed);
    uint32_t core1_stage = 0, core1_fault = ~0u, stack_guard = 1, stack_low = 0;
#ifdef JACK_MEGADUCK_CORE1_PROBE026
    core1_stage = atomic_load_explicit(&jack_core1_probe_stage, memory_order_acquire);
    core1_fault = jack_core1_fault_core;
#ifdef JACK_MEGADUCK_STACK_ISOLATE030
    unsigned free_words = 0;
    while (free_words < MEGADUCK_CORE1_STACK_WORDS &&
           megaduck_core1_stack[free_words] == MEGADUCK_STACK_PATTERN)
        ++free_words;
    stack_low = free_words * sizeof(uint32_t);
    for (unsigned i = 0; i < MEGADUCK_STACK_GUARD_WORDS; ++i) {
        if (megaduck_core1_stack[i] != MEGADUCK_STACK_PATTERN) {
            stack_guard = 0;
            break;
        }
    }
#endif
#endif
    printf("MEGADUCK_AUDIO034 elapsed_us=%lu source=%lu output=%lu wire=%lu padded=%lu nonzero=%lu peak=%lu prime=%lu batches=%lu frames=%lu clamps=%lu overflow=%lu queued_pcm=%lu queued_di=%lu dropped=%lu under=%lu push_fail=%lu wait=%lu resync=%d miss=%lu stage=%08lx fault=%lu guard=%lu low_free=%lu\n",
        (unsigned long)(now - report_start),
        (unsigned long)(audio034_source_frames - audio034_last_source),
        (unsigned long)(audio034_output_frames - audio034_last_output),
        (unsigned long)(audio034_wire_frames - audio034_last_wire),
        (unsigned long)(padded031 - audio031_last_padded),
        (unsigned long)(nonzero - last_nonzero), (unsigned long)max_abs,
        (unsigned long)(audio034_prime_silence - audio034_last_prime),
        (unsigned long)(audio034_batch_calls - audio034_last_batches),
        (unsigned long)(audio034_frame_flushes - audio034_last_flushes),
        (unsigned long)(audio034_clamped - audio034_last_clamped),
        (unsigned long)(audio034_overflow - audio034_last_overflow),
        (unsigned long)jack_pcm_level(&pcm),
        (unsigned long)hstx_di_queue_get_level(),
        (unsigned long)(dropped - last_drop), (unsigned long)(under - last_under),
        (unsigned long)atomic_load_explicit(&audio031_push_failures, memory_order_relaxed),
        (unsigned long)wait_timeouts, get_video_output_resync_count(),
        (unsigned long)video_output_get_line_miss_count(),
        (unsigned long)core1_stage, (unsigned long)core1_fault,
        (unsigned long)stack_guard, (unsigned long)stack_low);
    audio031_last_total = total031;
    audio031_last_padded = padded031;
    audio034_last_source = audio034_source_frames;
    audio034_last_output = audio034_output_frames;
    audio034_last_wire = audio034_wire_frames;
    audio034_last_prime = audio034_prime_silence;
    audio034_last_batches = audio034_batch_calls;
    audio034_last_flushes = audio034_frame_flushes;
    audio034_last_clamped = audio034_clamped;
    audio034_last_overflow = audio034_overflow;
    last_submit = submitted; last_encoded = sent; last_nonzero = nonzero;
    last_drop = dropped; last_under = under; report_start = now; max_abs = 0;
    audio034_report_due = now + 5000000u;
#else
    if (now - report_start < 5000000u) return;
    uint32_t under = hstx_di_queue_get_underrun_count();
    uint32_t sent = atomic_load(&encoded);
    printf("MEGADUCK_AUDIO rate=44100 elapsed_us=%lu generated=%lu encoded=%lu nonzero=%lu peak=%lu dropped=%lu underrun_packets=%lu queued_pcm=%lu queued_di=%lu wait_timeouts=%lu resync=%d line_miss=%lu\n",
        (unsigned long)(now-report_start), (unsigned long)(submitted-last_submit),
        (unsigned long)(sent-last_encoded), (unsigned long)(nonzero-last_nonzero),
        (unsigned long)max_abs, (unsigned long)(dropped-last_drop),
        (unsigned long)(under-last_under), (unsigned long)jack_pcm_level(&pcm),
        (unsigned long)hstx_di_queue_get_level(), (unsigned long)wait_timeouts,
        get_video_output_resync_count(), (unsigned long)video_output_get_line_miss_count());
#ifdef JACK_WAVE1_CONTINUOUS_TRANSPORT
    uint32_t total031 = atomic_load_explicit(&audio031_total, memory_order_relaxed);
    uint32_t padded031 = atomic_load_explicit(&audio031_padded, memory_order_relaxed);
#ifdef JACK_WAVE1_CONTINUOUS_SILENCE
    printf("WAVE1_AUDIO_CONTINUOUS elapsed_us=%lu output_frames=%lu padded_frames=%lu push_failures=%lu fallback_packets=%lu\n",
#else
    printf("MEGADUCK_AUDIO031 elapsed_us=%lu output_frames=%lu padded_frames=%lu push_failures=%lu fallback_packets=%lu\n",
#endif
        (unsigned long)(now-report_start),
        (unsigned long)((total031-audio031_last_total)*4u),
        (unsigned long)(padded031-audio031_last_padded),
        (unsigned long)atomic_load_explicit(&audio031_push_failures, memory_order_relaxed),
        (unsigned long)(under-last_under));
    audio031_last_total = total031;
    audio031_last_padded = padded031;
#endif
#ifdef JACK_MEGADUCK_AUDIO033
    printf("MEGADUCK_AUDIO033 elapsed_us=%lu source_frames=%lu output_frames=%lu prime_silence=%lu stretch_up=%lu stretch_down=%lu clamps=%lu target_pcm=%u queued_pcm=%lu\n",
        (unsigned long)(now-report_start),
        (unsigned long)(audio033_source_frames-audio033_last_source),
        (unsigned long)(audio033_output_frames-audio033_last_output),
        (unsigned long)(audio033_prime_silence-audio033_last_prime),
        (unsigned long)(audio033_stretch_up-audio033_last_up),
        (unsigned long)(audio033_stretch_down-audio033_last_down),
        (unsigned long)(audio033_clamped-audio033_last_clamped),
        JACK_AUDIO033_TARGET_PCM, (unsigned long)jack_pcm_level(&pcm));
    audio033_last_source = audio033_source_frames;
    audio033_last_output = audio033_output_frames;
    audio033_last_prime = audio033_prime_silence;
    audio033_last_up = audio033_stretch_up;
    audio033_last_down = audio033_stretch_down;
    audio033_last_clamped = audio033_clamped;
#endif
#ifdef JACK_MEGADUCK_CORE1_PROBE026
    uint32_t heartbeat = atomic_load_explicit(&jack_core1_probe_heartbeat,
                                               memory_order_acquire);
    printf("MEGADUCK_CORE1 stage=%08lx heartbeat=%lu delta=%lu packets=%lu packet_sets=%lu islands=%lu pushes=%lu frame_total=%lu fault_core=%lu sp=%08lx exc_return=%08lx pc=%08lx lr=%08lx xpsr=%08lx cfsr=%08lx hfsr=%08lx bfar=%08lx mmfar=%08lx\n",
        (unsigned long)atomic_load_explicit(&jack_core1_probe_stage, memory_order_acquire),
        (unsigned long)heartbeat,
        (unsigned long)(heartbeat - jack_core1_probe_last_heartbeat),
        (unsigned long)atomic_load_explicit(&jack_core1_probe_packets, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&jack_core1_probe_packet_sets, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&jack_core1_probe_islands, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&jack_core1_probe_pushes, memory_order_relaxed),
        (unsigned long)video_frame_count,
        (unsigned long)jack_core1_fault_core,
        (unsigned long)jack_core1_fault_sp,
        (unsigned long)jack_core1_fault_exc_return,
        (unsigned long)jack_core1_fault_pc,
        (unsigned long)jack_core1_fault_lr,
        (unsigned long)jack_core1_fault_xpsr,
        (unsigned long)jack_core1_fault_cfsr,
        (unsigned long)jack_core1_fault_hfsr,
        (unsigned long)jack_core1_fault_bfar,
        (unsigned long)jack_core1_fault_mmfar);
    jack_core1_probe_last_heartbeat = heartbeat;
#ifdef JACK_MEGADUCK_STACK_ISOLATE030
    unsigned free_words = 0;
    while (free_words < MEGADUCK_CORE1_STACK_WORDS &&
           megaduck_core1_stack[free_words] == MEGADUCK_STACK_PATTERN)
        ++free_words;
    bool guard_ok = true;
    for (unsigned i = 0; i < MEGADUCK_STACK_GUARD_WORDS; ++i) {
        if (megaduck_core1_stack[i] != MEGADUCK_STACK_PATTERN) {
            guard_ok = false;
            break;
        }
    }
    printf("MEGADUCK_STACK bottom=%08lx top=%08lx bytes=%lu limit=%08lx guard=%u low_free=%lu used_max=%lu fault_stage=%08lx\n",
        (unsigned long)(uintptr_t)megaduck_core1_stack,
        (unsigned long)(uintptr_t)(megaduck_core1_stack + MEGADUCK_CORE1_STACK_WORDS),
        (unsigned long)sizeof(megaduck_core1_stack),
        (unsigned long)jack_core1_stack_limit, guard_ok ? 1u : 0u,
        (unsigned long)(free_words * sizeof(uint32_t)),
        (unsigned long)((MEGADUCK_CORE1_STACK_WORDS - free_words) * sizeof(uint32_t)),
        (unsigned long)jack_core1_fault_stage);
#endif
#endif
    last_submit=submitted; last_encoded=sent; last_nonzero=nonzero;
    last_drop=dropped; last_under=under; report_start=now; max_abs=0;
#endif
}
