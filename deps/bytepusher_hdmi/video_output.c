#include "video_output.h"

#include "hstx_data_island_queue.h"
#include "hstx_packet.h"
#include "hstx_pins.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/sync.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/pll.h"

#include <math.h>
#include <string.h>
#include "hstx.h"
#include "stdio.h"

#ifndef HSTX_DEBUG
#define HSTX_DEBUG 0
#endif

// gpSP can opt into producing scanlines from the core-1 foreground loop
// instead of inside DMA_IRQ_0. This keeps the IRQ short enough to re-arm the
// ping/pong chain before its deadline and adds an explicit ready-tag handoff.
#ifndef PICO_HSTX_ASYNC_LINE_GUARD
#define PICO_HSTX_ASYNC_LINE_GUARD 0
#endif

#if defined(JACK_MEGADUCK_DIRECT_BACKGROUND028) || defined(JACK_MEGADUCK_LOW_NOISE029) || defined(JACK_MEGADUCK_STACK_ISOLATE030)
/* MegaDuck 0.1.28 deliberately removes the only data-derived branch target
 * from the Core1 steady-state loop. The implementation lives in SRAM in the
 * HDMI audio adapter; the linker resolves this as a direct Thumb call. */
extern void audio_pump(void);
#endif

// ============================================================================
// DVI/HSTX Constants
// ============================================================================

#define TMDS_CTRL_00 0x354u // vsync=0 hsync=0
#define TMDS_CTRL_01 0x0abu // vsync=0 hsync=1
#define TMDS_CTRL_10 0x154u // vsync=1 hsync=0
#define TMDS_CTRL_11 0x2abu // vsync=1 hsync=1

// Sync symbols: Lane 0 carries sync, Lanes 1&2 are always CTRL_00
#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// Data Island preamble: Lane 0 = sync, Lanes 1&2 = CTRL_01 pattern
// Per HDMI 1.3a Table 5-2: CTL0=1, CTL1=0, CTL2=1, CTL3=0
#define PREAMBLE_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))

// Video preamble: Lane 0 = sync, Lane 1 = CTRL_01, Lane 2 = CTRL_00
// Per HDMI 1.3a Table 5-2: CTL0=1, CTL1=0, CTL2=0, CTL3=0
#define VIDEO_PREAMBLE_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))
#define VIDEO_PREAMBLE_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))

// Video guard band: Per HDMI 1.3a Table 5-5
// CH0 = 0b1011001100 (0x2CC), CH1 = 0b0100110011 (0x133), CH2 = 0b1011001100 (0x2CC)
#define VIDEO_GUARD_BAND (0x2CCu | (0x133u << 10) | (0x2CCu << 20))

#define HSTX_CMD_RAW (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP (0xfu << 12)

#define SYNC_AFTER_DI (MODE_H_SYNC_WIDTH - W_PREAMBLE - W_DATA_ISLAND)

// Video preamble and guard band widths (HDMI 1.3a Section 5.2.2)
#define W_VIDEO_PREAMBLE 8
#define W_VIDEO_GUARD_BAND 2

// ============================================================================
// Audio/Video State
// ============================================================================

uint16_t frame_width = 0;
uint16_t frame_height = 0;
volatile uint32_t video_frame_count = 0;

#if HSTX_DEBUG
static volatile uint32_t irq_count = 0;
#endif

// DVI mode: when true, disables all HDMI Data Islands (pure DVI output, no audio)
// Some monitors have trouble syncing with HDMI Data Islands
static bool dvi_mode = false; // Default to HDMI mode (full features with audio)

// What DVI mode actually puts on the wire.
//
//   0 (default) real DVI 1.0 -- sync symbols only, no data-island preamble, no
//               TERC4 island, no video preamble or guard band. What DVI mode
//               emitted up to v0.42.
//   1           "silent HDMI" -- the HDMI cmdlist shape carrying a null DI
//               packet. Introduced in v0.43 and made unconditional there.
//
// v0.43 switched everyone to 1 because the uniform 96-cycle hsync of real DVI
// outran the TMDS clock-recovery PLL on some sinks, which then needed the
// watchdog to resync. But a DVI 1.0 receiver has no rule for TERC4 in the
// blanking interval, so that change made DVI-only monitors reject the signal
// outright (issue #217: BenQ FP72E and Dell 1908FPb, both no picture at all).
// Adding the AVI InfoFrame to the island stream did not help them, so the
// objection is to the islands themselves, not to a missing InfoFrame -- there
// is no single bitstream that satisfies both kinds of sink.
//
// 1 is kept for reference only -- do not reach for it. It is not DVI, so it
// buys a sink nothing that Display Mode = HDMI does not already give: that
// sends the same island structure with real packets, and carries audio too.
// A display that cannot hold sync on real DVI should simply be run in HDMI
// mode. Hence 0 is the default and there is no reason for a shared-driver
// consumer to override it.
#ifndef PICO_HDMI_DVI_USE_DATA_ISLANDS
#define PICO_HDMI_DVI_USE_DATA_ISLANDS 0
#endif

// Double-buffered line buffer. While DMA reads line_buffer[dma_idx] for the
// current active line, the scanline callback fills line_buffer[fill_idx] with
// the next line's pixels. This removes the write/read race and lets us
// reconfigure the DMA *before* calling the (slow) scanline callback.
static uint16_t line_buffer[2][MODE_H_ACTIVE_PIXELS] __attribute__((aligned(4)));
#if PICO_HSTX_ASYNC_LINE_GUARD
static uint16_t safe_line_buffer[MODE_H_ACTIVE_PIXELS] __attribute__((aligned(4)));
static volatile uint32_t line_buffer_ready_tag[2] = {0, 0};
static volatile uint32_t line_fill_request_sequence = 0;
static volatile uint32_t line_fill_request_v_scanline = UINT32_MAX;
static volatile uint32_t line_fill_request_active_line = 0;
static volatile uint32_t line_fill_request_tag = 0;
static uintptr_t line_dma_source = 0;
static volatile uint32_t line_ready_miss_count = 0;
#else
static uint8_t line_buf_fill_idx = 0;  // buffer the callback writes into
#endif
static uint8_t line_buf_dma_idx = 0;   // buffer the next pixel-data DMA reads
static uint32_t v_scanline = 2;
static bool vactive_cmdlist_posted = false;
static bool dma_pong = false;

static video_output_task_fn background_task = NULL;
static video_output_scanline_cb_t scanline_callback = NULL;
static video_output_vsync_cb_t vsync_callback = NULL;
// Guard every indirect callback. At extreme clk_sys rates a corrupted target
// otherwise turns into an opaque HardFault. Keep the complements in .data,
// away from the zero-initialized callback pointers in .bss; a single stray
// store can then be detected and the background callback can be recovered.
static uintptr_t background_task_guard = ~(uintptr_t)0;
static uintptr_t scanline_callback_guard = ~(uintptr_t)0;
static uintptr_t vsync_callback_guard = ~(uintptr_t)0;
static volatile uint32_t callback_guard_fault_count = 0;
static volatile uint32_t callback_guard_fault_kind = 0;
static volatile uintptr_t callback_guard_fault_pointer = 0;
static volatile uintptr_t callback_guard_fault_complement = 0;
static volatile bool callback_guard_fault_latched = false;


// Auto-resync watchdog: set by core 0 (or by the core-1 main-loop watchdog)
// to request a full HSTX+DMA reset. Consumed in video_output_core1_run().
static volatile bool resync_requested = false;
static volatile int resync_count = 0;
#define DMACH_PING 0
#define DMACH_PONG 1

#ifdef JACK_BYTEPUSHER_HSTX_DIAG_R12
static volatile uint32_t r12_irq_count = 0;
static volatile uint32_t r12_empty_irq_count = 0;
static volatile uint32_t r12_dual_irq_count = 0;
static volatile uint32_t r12_unexpected_irq_count = 0;
static volatile uint32_t r12_expected_busy_count = 0;

static inline void __not_in_flash_func(r12_hstx_reset_preserve)(void)
{
    const uint32_t csr = hstx_ctrl_hw->csr & ~HSTX_CTRL_CSR_EN_BITS;
    uint32_t pins[8];
    for (unsigned i = 0; i < 8; ++i) pins[i] = hstx_ctrl_hw->bit[i];
    const uint32_t shift = hstx_ctrl_hw->expand_shift;
    const uint32_t tmds = hstx_ctrl_hw->expand_tmds;

    reset_block_mask(RESETS_RESET_HSTX_BITS);
    unreset_block_mask_wait_blocking(RESETS_RESET_HSTX_BITS);

    for (unsigned i = 0; i < 8; ++i) hstx_ctrl_hw->bit[i] = pins[i];
    hstx_ctrl_hw->expand_shift = shift;
    hstx_ctrl_hw->expand_tmds = tmds;
    hstx_ctrl_hw->csr = csr;
}
#endif

// ============================================================================
// Command Lists
// ============================================================================

// Pure DVI command lists (no Data Islands)
static uint32_t vblank_line_vsync_off[] = {HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
                                           SYNC_V1_H1,
                                           HSTX_CMD_NOP,
                                           HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
                                           SYNC_V1_H0,
                                           HSTX_CMD_NOP,
                                           HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
                                           SYNC_V1_H1,
                                           HSTX_CMD_NOP};

static uint32_t vblank_line_vsync_on[] = {HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
                                          SYNC_V0_H1,
                                          HSTX_CMD_NOP,
                                          HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
                                          SYNC_V0_H0,
                                          HSTX_CMD_NOP,
                                          HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
                                          SYNC_V0_H1,
                                          HSTX_CMD_NOP};

// Active video line for DVI mode (no Data Island, just sync + pixels)
static uint32_t vactive_line_dvi[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V1_H1, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V1_H0, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,  SYNC_V1_H1, HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS};

static uint32_t vactive_di_ping[128], vactive_di_pong[128], vactive_di_null[128];
static uint32_t vactive_di_len, vactive_di_null_len;

static uint32_t vblank_di_ping[128], vblank_di_pong[128], vblank_di_null[128];
static uint32_t vblank_di_len, vblank_di_null_len;

// DVI-mode null-DI cmdlist for VSYNC-active blanking lines. The other
// null-DI buffers above are pre-built with vsync=false; we need a vsync=true
// variant so DVI mode can emit the same HDMI-structured cmdlist (data-island
// period + video preamble + guard band) on every line of the frame, including
// the 2-line vertical sync pulse. See video_output_init().
static uint32_t vblank_di_null_vsync_on[128];
static uint32_t vblank_di_null_vsync_on_len;

static uint32_t vblank_acr_vsync_on[64], vblank_acr_vsync_on_len;
static uint32_t vblank_acr_vsync_off[64], vblank_acr_vsync_off_len;
static uint32_t vblank_infoframe_vsync_on[64], vblank_infoframe_vsync_on_len;
static uint32_t vblank_infoframe_vsync_off[64], vblank_infoframe_vsync_off_len;
static uint32_t vblank_avi_infoframe[64], vblank_avi_infoframe_len;

// ============================================================================
// HSTX Resync - Reset output to sync with input VSYNC
// ============================================================================

static void __not_in_flash_func(hstx_resync)(void)
{
    // RP2350-E5: clear EN on the aborted channel AND any channel it chains
    // from, otherwise a chained partner can re-trigger the aborted channel
    // while abort is in flight and leave it latched live with stale config.
    hw_clear_bits(&dma_hw->ch[DMACH_PING].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[DMACH_PONG].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);

    // 1. Abort DMA chains
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);

    // 2. Reset HSTX. The BytePusher R12 path performs a complete peripheral
    // reset because a chain-launched app can inherit command/FIFO state from
    // the loader; the legacy EN toggle does not clear that state reliably.
#ifdef JACK_BYTEPUSHER_HSTX_DIAG_R12
    r12_hstx_reset_preserve();
#else
    hstx_ctrl_hw->csr &= ~HSTX_CTRL_CSR_EN_BITS;

    // Small delay to ensure HSTX fully stops
    __asm volatile("nop\nnop\nnop\nnop");
#endif

    // 3. Reset state to start of frame
    v_scanline = 0;
    vactive_cmdlist_posted = false;
    dma_pong = false;
#if PICO_HSTX_ASYNC_LINE_GUARD
    line_buffer_ready_tag[0] = 0;
    line_buffer_ready_tag[1] = 0;
    line_dma_source = (uintptr_t)safe_line_buffer;
    line_fill_request_v_scanline = UINT32_MAX;
    line_fill_request_tag = 0;
    __dmb();
    line_fill_request_sequence++;
#else
    line_buf_fill_idx = 0;
#endif
    line_buf_dma_idx = 0;

    // 4. Clear any pending DMA interrupts (including spurious completion
    //    interrupts that abort can latch per RP2350-E5 / SDK docs).
    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);

    // 5. Restore the EN bits cleared above so the channels are live again.
    hw_set_bits(&dma_hw->ch[DMACH_PING].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_set_bits(&dma_hw->ch[DMACH_PONG].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);

    // 6. Configure DMA PING to start from beginning of frame (Line 0)
    dma_channel_hw_t *ch_ping = &dma_hw->ch[DMACH_PING];
    ch_ping->read_addr = (uintptr_t)vblank_line_vsync_off;
    ch_ping->transfer_count = count_of(vblank_line_vsync_off);

    // 7. Configure DMA PONG for the NEXT line (Line 1)
    // This ensures that when PING finishes and chains to PONG, PONG is ready.
    dma_channel_hw_t *ch_pong = &dma_hw->ch[DMACH_PONG];
    ch_pong->read_addr = (uintptr_t)vblank_line_vsync_off; // Line 1 is also blank
    ch_pong->transfer_count = count_of(vblank_line_vsync_off);

    // 8. Re-enable HSTX then start DMA
    hstx_ctrl_hw->csr |= HSTX_CTRL_CSR_EN_BITS;
    dma_channel_start(DMACH_PING);
}

// ============================================================================
// Internal Helpers
// ============================================================================

// __not_in_flash_func: called from the scanline DMA IRQ; must not fetch from
// flash while the QMI may be saturated (CHD decompress, PSRAM traffic).
static uint32_t __not_in_flash_func(build_line_with_di)(uint32_t *buf, const uint32_t *di_words, bool vsync, bool active)
{
    uint32_t *p = buf;
    uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
    uint32_t preamble = vsync ? PREAMBLE_V0_H0 : PREAMBLE_V1_H0;

    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
    *p++ = preamble;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
    for (int i = 0; i < W_DATA_ISLAND; i++)
        *p++ = di_words[i];
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | SYNC_AFTER_DI;
    *p++ = sync_h0;
    *p++ = HSTX_CMD_NOP;

    if (active) {
        // HDMI 1.3a Section 5.2.2: Video Data Period requires preamble and guard band
        uint32_t video_preamble = vsync ? VIDEO_PREAMBLE_V0_H1 : VIDEO_PREAMBLE_V1_H1;

        // Control period (back porch minus preamble and guard band)
        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH - W_VIDEO_PREAMBLE - W_VIDEO_GUARD_BAND);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;

        // Video Preamble (8 pixels)
        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_PREAMBLE;
        *p++ = video_preamble;
        *p++ = HSTX_CMD_NOP;

        // Video Guard Band (2 pixels)
        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_GUARD_BAND;
        *p++ = VIDEO_GUARD_BAND;

        // Active video pixels
        *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
    } else {
        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;
    }
    return (uint32_t)(p - buf);
}

typedef struct {
    bool vsync_active;
    bool front_porch;
    bool back_porch;
    bool active_video;
    bool send_acr;
    uint32_t active_line;
} scanline_state_t;

static inline void __not_in_flash_func(get_scanline_state)(uint32_t v_scanline, scanline_state_t *state)
{
    state->vsync_active = (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH));
    state->front_porch = (v_scanline < MODE_V_FRONT_PORCH);
    state->back_porch = (v_scanline >= MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH &&
                         v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH);
    state->active_video = (!state->vsync_active && !state->front_porch && !state->back_porch);

    state->send_acr = (v_scanline >= (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) &&
                       v_scanline < (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES) && (v_scanline % 4 == 0));

    if (state->active_video) {
        state->active_line = v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    } else {
        state->active_line = 0;
    }
}

#if PICO_HSTX_ASYNC_LINE_GUARD
static inline uint32_t __not_in_flash_func(video_output_line_tag)(uint32_t scanline)
{
    // 10 bits are enough for the 525-line 640x480 timing. Including the HDMI
    // frame counter prevents a completed line from the prior frame being
    // accepted as current after a producer overrun.
    return (video_frame_count << 10) | (scanline + 1u);
}

static inline void __not_in_flash_func(video_output_queue_active_line)(uint32_t scanline)
{
    const uint32_t active_begin = MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES;
    if (scanline < active_begin || scanline >= MODE_V_TOTAL_LINES)
        return;

    const uint8_t fill_idx = (uint8_t)(scanline & 1u);
    line_buffer_ready_tag[fill_idx] = 0;
    line_fill_request_v_scanline = scanline;
    line_fill_request_active_line = scanline - active_begin;
    line_fill_request_tag = video_output_line_tag(scanline);
    __dmb();
    line_fill_request_sequence++;
}

static void __not_in_flash_func(video_output_service_line_request)(uint32_t *handled_sequence)
{
    const uint32_t sequence = line_fill_request_sequence;
    if (sequence == *handled_sequence)
        return;

    __dmb();
    const uint32_t scanline = line_fill_request_v_scanline;
    const uint32_t active_line = line_fill_request_active_line;
    const uint32_t ready_tag = line_fill_request_tag;
    *handled_sequence = sequence;

    const uint32_t active_begin = MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES;
    if (ready_tag == 0 || scanline < active_begin || scanline >= MODE_V_TOTAL_LINES)
        return;

    const uint8_t fill_idx = (uint8_t)(scanline & 1u);
    uint32_t *dst32 = (uint32_t *)line_buffer[fill_idx];
    video_output_scanline_cb_t callback = scanline_callback;
    const uintptr_t callback_pointer = (uintptr_t)callback;
    const uintptr_t callback_complement = scanline_callback_guard;
    if ((callback_pointer ^ callback_complement) != ~(uintptr_t)0) {
        if (!callback_guard_fault_latched) {
            callback_guard_fault_pointer = callback_pointer;
            callback_guard_fault_complement = callback_complement;
            callback_guard_fault_kind = 1;
            callback_guard_fault_count++;
            callback_guard_fault_latched = true;
            resync_requested = true;
        }
        callback = NULL;
    }

    if (callback) {
        callback(scanline, active_line, dst32);
    } else {
        for (uint32_t i = 0; i < MODE_H_ACTIVE_PIXELS / 2; i++)
            dst32[i] = 0;
    }

    // Publish only after all 640 pixels are complete. DMA_IRQ_0 invalidates
    // the tag before each request and will use the immutable black fallback
    // rather than scan a buffer that is still being written.
    __dmb();
    line_buffer_ready_tag[fill_idx] = ready_tag;
}
#endif

static inline void __not_in_flash_func(video_output_handle_vsync)(dma_channel_hw_t *ch, uint32_t v_scanline)
{
    if (dvi_mode) {
        // See PICO_HDMI_DVI_USE_DATA_ISLANDS above for why there are two of
        // these and why the plain DVI line is the default.
#if PICO_HDMI_DVI_USE_DATA_ISLANDS
        ch->read_addr = (uintptr_t)vblank_di_null_vsync_on;
        ch->transfer_count = vblank_di_null_vsync_on_len;
#else
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
#endif
        if (v_scanline == MODE_V_FRONT_PORCH) {
            video_frame_count++;
            video_output_vsync_cb_t callback = vsync_callback;
            uintptr_t callback_pointer = (uintptr_t)callback;
            uintptr_t callback_complement = vsync_callback_guard;
            if ((callback_pointer ^ callback_complement) == ~(uintptr_t)0) {
                if (callback)
                    callback();
            } else if (!callback_guard_fault_latched) {
                callback_guard_fault_pointer = callback_pointer;
                callback_guard_fault_complement = callback_complement;
                callback_guard_fault_kind = 2;
                callback_guard_fault_count++;
                callback_guard_fault_latched = true;
                resync_requested = true;
            }
        }
    } else {
        if (v_scanline == MODE_V_FRONT_PORCH) {
            ch->read_addr = (uintptr_t)vblank_acr_vsync_on;
            ch->transfer_count = vblank_acr_vsync_on_len;
            video_frame_count++;
            video_output_vsync_cb_t callback = vsync_callback;
            uintptr_t callback_pointer = (uintptr_t)callback;
            uintptr_t callback_complement = vsync_callback_guard;
            if ((callback_pointer ^ callback_complement) == ~(uintptr_t)0) {
                if (callback)
                    callback();
            } else if (!callback_guard_fault_latched) {
                callback_guard_fault_pointer = callback_pointer;
                callback_guard_fault_complement = callback_complement;
                callback_guard_fault_kind = 2;
                callback_guard_fault_count++;
                callback_guard_fault_latched = true;
                resync_requested = true;
            }
        } else {
            ch->read_addr = (uintptr_t)vblank_infoframe_vsync_on;
            ch->transfer_count = vblank_infoframe_vsync_on_len;
        }
    }
}

static inline void __not_in_flash_func(video_output_handle_active_start)(dma_channel_hw_t *ch, uint32_t v_scanline, uint32_t active_line, bool dma_pong)
{
    // 1. Arm the cmdlist DMA FIRST, before any slow work. This channel will be
    //    chain-triggered when the currently-running pixel-data DMA completes,
    //    so it must be armed well before then. Doing this first means that
    //    even if the scanline callback below overruns, the cmdlist chain is
    //    safe and HSTX keeps emitting valid sync.
    if (dvi_mode) {
#if PICO_HDMI_DVI_USE_DATA_ISLANDS
        ch->read_addr = (uintptr_t)vactive_di_null;
        ch->transfer_count = vactive_di_null_len;
#else
        ch->read_addr = (uintptr_t)vactive_line_dvi;
        ch->transfer_count = count_of(vactive_line_dvi);
#endif
    } else {
        uint32_t *buf = dma_pong ? vactive_di_ping : vactive_di_pong;
        const uint32_t *di_words = hstx_di_queue_get_audio_packet();
        if (di_words) {
            vactive_di_len = build_line_with_di(buf, di_words, false, true);
            ch->read_addr = (uintptr_t)buf;
            ch->transfer_count = vactive_di_len;
        } else {
            ch->read_addr = (uintptr_t)vactive_di_null;
            ch->transfer_count = vactive_di_null_len;
        }
    }

    // 2. Select the buffer for the following pixel-data transfer.
#if PICO_HSTX_ASYNC_LINE_GUARD
    const uint8_t ready_idx = (uint8_t)(v_scanline & 1u);
    const uint32_t expected_tag = video_output_line_tag(v_scanline);
    __dmb();
    if (line_buffer_ready_tag[ready_idx] == expected_tag) {
        line_buf_dma_idx = ready_idx;
        line_dma_source = (uintptr_t)line_buffer[ready_idx];
    } else {
        // Never expose a half-written line to HSTX. A missed producer deadline
        // becomes one deterministic black line and a telemetry increment.
        line_dma_source = (uintptr_t)safe_line_buffer;
        line_ready_miss_count++;
    }
#else
    // Pick the line buffer that's NOT currently being read by the pixel
    //    data DMA, and record it for the next handle_active_data() to point
    //    DMA at. Doing the handoff here (with IRQs serialised by NVIC) is
    //    race-free: the next IRQ can only run after this one returns.
    uint8_t fill_idx = line_buf_fill_idx;
    line_buf_dma_idx = fill_idx;
    line_buf_fill_idx = fill_idx ^ 1;

    // 3. Fill the line buffer (slow: ~21 us for 640-wide 2x doubling with
    //    optional scanline darken). DMA is already armed, so if this overruns
    //    the active region we still chain to a valid cmdlist rather than
    //    zero-length garbage.
    uint32_t *dst32 = (uint32_t *)line_buffer[fill_idx];
    video_output_scanline_cb_t callback = scanline_callback;
    uintptr_t callback_pointer = (uintptr_t)callback;
    uintptr_t callback_complement = scanline_callback_guard;
    if ((callback_pointer ^ callback_complement) != ~(uintptr_t)0) {
        if (!callback_guard_fault_latched) {
            callback_guard_fault_pointer = callback_pointer;
            callback_guard_fault_complement = callback_complement;
            callback_guard_fault_kind = 1;
            callback_guard_fault_count++;
            callback_guard_fault_latched = true;
            resync_requested = true;
        }
        callback = NULL;
    }
    if (callback) {
        callback(v_scanline, active_line, dst32);
    } else {
        for (uint32_t i = 0; i < MODE_H_ACTIVE_PIXELS / 2; i++) {
            dst32[i] = 0;
        }
    }
#endif
}

static inline void __not_in_flash_func(video_output_handle_blanking)(dma_channel_hw_t *ch, uint32_t v_scanline, bool send_acr, bool dma_pong)
{
    if (dvi_mode) {
        (void)send_acr;
        (void)dma_pong;
        (void)v_scanline;
#if PICO_HDMI_DVI_USE_DATA_ISLANDS
        ch->read_addr = (uintptr_t)vblank_di_null;
        ch->transfer_count = vblank_di_null_len;
#else
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
#endif
    } else {
        if (send_acr) {
            ch->read_addr = (uintptr_t)vblank_acr_vsync_off;
            ch->transfer_count = vblank_acr_vsync_off_len;
        } else if (v_scanline == 0) {
            ch->read_addr = (uintptr_t)vblank_avi_infoframe;
            ch->transfer_count = vblank_avi_infoframe_len;
        } else {
            const uint32_t *di_words = hstx_di_queue_get_audio_packet();
            if (di_words) {
                uint32_t *buf = dma_pong ? vblank_di_ping : vblank_di_pong;
                vblank_di_len = build_line_with_di(buf, di_words, false, false);
                ch->read_addr = (uintptr_t)buf;
                ch->transfer_count = vblank_di_len;
            } else {
                ch->read_addr = (uintptr_t)vblank_di_null;
                ch->transfer_count = vblank_di_null_len;
            }
        }
    }
}

static inline void __not_in_flash_func(video_output_handle_active_data)(dma_channel_hw_t *ch)
{
#if PICO_HSTX_ASYNC_LINE_GUARD
    ch->read_addr = line_dma_source;
#else
    ch->read_addr = (uintptr_t)line_buffer[line_buf_dma_idx];
#endif
    ch->transfer_count = (MODE_H_ACTIVE_PIXELS * sizeof(uint16_t)) / sizeof(uint32_t);
}

// ============================================================================
// DMA IRQ Handler
// ============================================================================

void __not_in_flash_func(dma_irq_handler)(void)
{
    #if HSTX_DEBUG
    irq_count++;
    #endif
    uint32_t ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
#ifdef JACK_BYTEPUSHER_HSTX_DIAG_R12
    const uint32_t owned = (1u << DMACH_PING) | (1u << DMACH_PONG);
    const uint32_t expected = 1u << ch_num;
    const uint32_t pending = dma_hw->ints0 & owned;
    r12_irq_count++;
    if (pending == 0) r12_empty_irq_count++;
    if (pending == owned) r12_dual_irq_count++;
    if ((pending & expected) == 0) r12_unexpected_irq_count++;
    if ((ch->al1_ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0)
        r12_expected_busy_count++;
#endif
    dma_hw->intr = 1U << ch_num;
    dma_pong = !dma_pong;

    // Advance audio/data-island scheduler exactly once per scanline (HDMI mode only)
    if (!dvi_mode && !vactive_cmdlist_posted) {
        hstx_di_queue_tick();
    }

    scanline_state_t state;
    get_scanline_state(v_scanline, &state);

    if (state.vsync_active) {
        video_output_handle_vsync(ch, v_scanline);
    } else if (state.active_video && !vactive_cmdlist_posted) {
        video_output_handle_active_start(ch, v_scanline, state.active_line, dma_pong);
        vactive_cmdlist_posted = true;
    } else if (state.active_video && vactive_cmdlist_posted) {
        video_output_handle_active_data(ch);
        vactive_cmdlist_posted = false;
    } else {
        video_output_handle_blanking(ch, v_scanline, state.send_acr, dma_pong);
    }
    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
#if PICO_HSTX_ASYNC_LINE_GUARD
        video_output_queue_active_line(v_scanline);
#endif
    }
}

// ============================================================================
// Public Interface
// ============================================================================

// ACR N/CTS lookup for 25.2 MHz pixel clock (HDMI spec Table 7-1/7-2)
static void get_acr_params(uint32_t sample_rate, uint32_t *n, uint32_t *cts)
{
    switch (sample_rate) {
        case 32000:
            *n = 4096;
            *cts = 25200;
            break;
        case 44100:
            *n = 6272;
            *cts = 28000;
            break;
        case 48000:
            *n = 6144;
            *cts = 25200;
            break;
        case 88200:
            *n = 12544;
            *cts = 28000;
            break;
        case 96000:
            *n = 12288;
            *cts = 25200;
            break;
        case 176400:
            *n = 25088;
            *cts = 28000;
            break;
        case 192000:
            *n = 24576;
            *cts = 25200;
            break;
        default:
            *n = 6144;
            *cts = 25200;
            break; // fallback to 48kHz
    }
}

// Last rate handed to configure_audio_packets. Lets hstx_restart_core1
// restore the caller-configured rate instead of resetting to 44.1 kHz
// (pico_snesPlus runs 32 kHz).
static uint32_t configured_audio_sample_rate = 48000;

uint32_t pico_hdmi_get_audio_sample_rate(void)
{
    return configured_audio_sample_rate;
}

static void configure_audio_packets(uint32_t sample_rate)
{
    configured_audio_sample_rate = sample_rate;
    hstx_di_queue_set_sample_rate(sample_rate);
    // Keep the IEC 60958 channel-status sample-frequency code in lockstep
    // with ACR and the Audio InfoFrame — strict sinks require all three to
    // agree with the actual stream rate.
    hstx_packet_set_cs_sample_rate(sample_rate);

    hstx_packet_t packet;
    hstx_data_island_t island;

    uint32_t acr_n;
    uint32_t acr_cts;
    get_acr_params(sample_rate, &acr_n, &acr_cts);
    hstx_packet_set_acr(&packet, acr_n, acr_cts);
    hstx_encode_data_island(&island, &packet, true, true);
    vblank_acr_vsync_on_len = build_line_with_di(vblank_acr_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, true);
    vblank_acr_vsync_off_len = build_line_with_di(vblank_acr_vsync_off, island.words, false, false);

    hstx_packet_set_audio_infoframe(&packet, sample_rate, 2, 16);
    hstx_encode_data_island(&island, &packet, true, true);
    vblank_infoframe_vsync_on_len = build_line_with_di(vblank_infoframe_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, true);
    vblank_infoframe_vsync_off_len = build_line_with_di(vblank_infoframe_vsync_off, island.words, false, false);
}

void video_output_init(uint16_t width, uint16_t height)
{
    frame_width = width;
    frame_height = height;
#if PICO_HSTX_ASYNC_LINE_GUARD
    line_buffer_ready_tag[0] = 0;
    line_buffer_ready_tag[1] = 0;
    line_fill_request_sequence = 0;
    line_fill_request_v_scanline = UINT32_MAX;
    line_fill_request_active_line = 0;
    line_fill_request_tag = 0;
    line_dma_source = (uintptr_t)safe_line_buffer;
    line_ready_miss_count = 0;
#endif

    // Configure clk_hstx for the current video mode
    // After set_sys_clock_khz(), clk_hstx needs to be reconfigured
#ifndef NOHSTXCLOCK
    uint32_t sys_freq = clock_get_hz(clk_sys);

    clock_configure_int_divider(clk_hstx,
                                0, // No glitchless mux
                                CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys_freq, MODE_HSTX_CLK_DIV);
#endif
    // Claim DMA channels for HSTX (channels 0 and 1)
    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);

    // Initialize HDMI audio packets (default 48kHz)
    configure_audio_packets(48000);

    hstx_packet_t packet;
    hstx_data_island_t island;

#ifdef VIDEO_MODE_320x240
    // PR=3 (4x repetition) for 1280x240 representing 320x240
    hstx_packet_set_avi_infoframe(&packet, 1, 3);
#else
    hstx_packet_set_avi_infoframe(&packet, 1, 0);
#endif
    hstx_encode_data_island(&island, &packet, false, true);
    vblank_avi_infoframe_len = build_line_with_di(vblank_avi_infoframe, island.words, false, false);

    vblank_di_null_len = build_line_with_di(vblank_di_null, hstx_get_null_data_island(false, true), false, false);
    vactive_di_null_len = build_line_with_di(vactive_di_null, hstx_get_null_data_island(false, true), false, true);

    // Vsync-active variant for DVI mode's two vsync lines (HDMI mode uses
    // the ACR/InfoFrame cmdlists instead). Built with vsync=true so the sync
    // symbols carry VSYNC asserted, and the embedded null DI also has its
    // vsync-active TERC4 encoding.
    vblank_di_null_vsync_on_len = build_line_with_di(vblank_di_null_vsync_on, hstx_get_null_data_island(true, true), true, false);

    vblank_di_len = build_line_with_di(vblank_di_ping, hstx_get_null_data_island(false, true), false, false);
    memcpy(vblank_di_pong, vblank_di_ping, sizeof(vblank_di_ping));
}

// Tear-down counterpart to video_output_init / video_output_core1_run.
// Disables and releases everything those two install so the same hardware
// can be brought back up by a fresh init pair. Required when the caller
// wants to reset core1 with a different stack at runtime — the SDK's
// dma_channel_claim and irq_set_exclusive_handler both panic on the second
// install if the previous claim / handler wasn't released.
//
// Call ordering: caller must already have stopped feeding the audio path
// (background_task = NULL) and be on core0 — core1 will be reset by the
// caller after this returns.
void video_output_stop(void)
{
    // Quiet the IRQ first so the DMA abort that follows can't trigger a
    // half-state in dma_irq_handler.
    irq_set_enabled(DMA_IRQ_0, false);

    // Disable the DMA channels' EN bit before aborting (matches the safe
    // pattern used by hstx_resync above).
    hw_clear_bits(&dma_hw->ch[DMACH_PING].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[DMACH_PONG].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);
    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    dma_hw->inte0 &= ~((1U << DMACH_PING) | (1U << DMACH_PONG));

    // Park HSTX. video_output_core1_run will re-program CSR on the re-launch.
    hstx_ctrl_hw->csr &= ~HSTX_CTRL_CSR_EN_BITS;

    // Hand the channels and IRQ back to the SDK so the next init can
    // re-claim them without panicking.
    irq_remove_handler(DMA_IRQ_0, dma_irq_handler);
    dma_channel_unclaim(DMACH_PING);
    dma_channel_unclaim(DMACH_PONG);

    // Reset latched state in our own driver layer that the next init may
    // consult on the way back up.
    resync_requested = false;
}

void video_output_set_background_task(video_output_task_fn task)
{
    background_task = task;
    background_task_guard = ~((uintptr_t)task);
    callback_guard_fault_latched = false;
}

void video_output_request_resync(void)
{
    resync_requested = true;
}

uint32_t video_output_get_line_miss_count(void)
{
#if PICO_HSTX_ASYNC_LINE_GUARD
    return line_ready_miss_count;
#else
    return 0;
#endif
}

bool video_output_get_dvi_mode(void)
{
    return dvi_mode;
}

void video_output_set_dvi_mode(bool enabled)
{
    dvi_mode = enabled;
}

void video_output_set_scanline_callback(video_output_scanline_cb_t cb)
{
    scanline_callback = cb;
    scanline_callback_guard = ~((uintptr_t)cb);
    callback_guard_fault_latched = false;
}

void video_output_set_vsync_callback(video_output_vsync_cb_t cb)
{
    vsync_callback = cb;
    vsync_callback_guard = ~((uintptr_t)cb);
    callback_guard_fault_latched = false;
}

// __not_in_flash_func: this function's while(1) loop runs on core1 forever.
// When core0 calls flash_range_erase / flash_range_program, XIP is briefly
// disabled and any core1 instruction fetch from flash stalls or returns
// garbage. Keeping the whole function (including the one-time hardware-init
// prologue) in SRAM costs ~700 bytes but lets the bootloader's progress bar
// keep updating on screen while flashing a new emulator. All callees from
// the steady-state loop (time_us_32 inline, hstx_resync, dma_irq_handler,
// scanline_callback, etc.) are themselves __not_in_flash_func; the only
// flash-resident calls left here (printf, irq_set_enabled) sit inside the
// rare resync path, which is not expected to fire during a normal flash
// write -- and would be the same risk as before this attribute was added.
void __not_in_flash_func(video_output_core1_run)(void)
{
#ifdef JACK_BYTEPUSHER_HSTX_DIAG_R12
    // Stop any DMA/HSTX activity inherited from the chain-loading bootloader.
    // Channels are already claimed by video_output_init(), but are not yet
    // configured by this application.
    irq_set_enabled(DMA_IRQ_0, false);
    hw_clear_bits(&dma_hw->ch[DMACH_PING].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[DMACH_PONG].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);
    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    reset_block_mask(RESETS_RESET_HSTX_BITS);
    unreset_block_mask_wait_blocking(RESETS_RESET_HSTX_BITS);
#endif
    uint32_t reported_callback_guard_faults = 0;
#if PICO_HSTX_ASYNC_LINE_GUARD
    uint32_t handled_line_request = line_fill_request_sequence;
#endif
#if 0
    // HSTX Hardware Setup
    hstx_ctrl_hw->expand_tmds = 4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                                5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 3 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                                4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 13 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
#else
     // Configure HSTX's TMDS encoder for RGB555
    hstx_ctrl_hw->expand_tmds =
        29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB |
        4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        2 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        4 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        7 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB;
    // Pixels (TMDS) come in 4 8-bit chunks. Control symbols (RAW) are an
    // entire 32-bit word.
    hstx_ctrl_hw->expand_shift =
        2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

#endif
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS | (uint32_t)MODE_HSTX_CSR_CLKDIV << HSTX_CTRL_CSR_CLKDIV_LSB |
                        5U << HSTX_CTRL_CSR_N_SHIFTS_LSB | 2U << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

    int clk_bit_p = HSTX_BIT_FROM_GPIO(GPIOHSTXCK);
    int clk_bit_n = GPIOHSTXINVERTED ? (clk_bit_p - 1) : (clk_bit_p + 1);
    hstx_ctrl_hw->bit[clk_bit_p] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[clk_bit_n] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    // hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    // hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
          // For each TMDS lane, assign it to the correct GPIO pair based on the desired pinout:

        // lane_to_output_bit Array:
        // The lane_to_output_bit array specifies which HSTX output bits are used for each TMDS lane, based on the GPIOHSTXDx defines:

        // Index 0: TMDS lane D0 (data lane 0) → GPIOHSTXD0 (D0+), (GPIOHSTXD0 + GPIOHSTXINVERTED ? -1 : +1) (D0-)
        // Index 1: TMDS lane D1 (data lane 1) → GPIOHSTXD1 (D1+), (GPIOHSTXD1 + GPIOHSTXINVERTED ? -1 : +1) (D1-)
        // Index 2: TMDS lane D2 (data lane 2) → GPIOHSTXD2 (D2+), (GPIOHSTXD2 + GPIOHSTXINVERTED ? -1 : +1) (D2-)

        // Example default mapping for Adafruit Metro RP2350:
        // D0+ = GPIOHSTXD0 (default: GPIO18), D0- = GPIOHSTXD0 + 1 (default: GPIO19)
        // D1+ = GPIOHSTXD1 (default: GPIO16), D1- = GPIOHSTXD1 + 1 (default: GPIO17)
        // D2+ = GPIOHSTXD2 (default: GPIO12), D2- = GPIOHSTXD2 + 1 (default: GPIO13)
        // If GPIOHSTXINVERTED is set, D- is D+ - 1 instead of D+ + 1
        // See https://learn.adafruit.com/adafruit-metro-rp2350/pinouts#hstx-connector-3193107
        static const int lane_to_output_bit[3] = {
            HSTX_BIT_FROM_GPIO(GPIOHSTXD0),
            HSTX_BIT_FROM_GPIO(GPIOHSTXD1),
            HSTX_BIT_FROM_GPIO(GPIOHSTXD2)};
        int bit = lane_to_output_bit[lane];
        int bit_dn = GPIOHSTXINVERTED ? (bit - 1) : (bit + 1);
        // Output even bits during first half of each HSTX cycle, and odd bits
        // during second half. The shifter advances by two bits each cycle.
        uint32_t lane_data_sel_bits =
            (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        // The two halves of each pair get identical data, but one pin is inverted.
        hstx_ctrl_hw->bit[bit] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit_dn] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    // Set GPIO 12-19 to HSTX function (function 0 on RP2350)
    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, 0);
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
    }

    // DMA Setup
    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off, count_of(vblank_line_vsync_off),
                          false);

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off, count_of(vblank_line_vsync_off),
                          false);

    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    dma_hw->inte0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);

    // Watchdog: if video_frame_count stops advancing (e.g. DMA chain wedged
    // after an IRQ overrun, HSTX FIFO underflow), hstx_resync() brings the
    // pipeline back up without a reboot. 500 ms is ~30 frames at 60 Hz, long
    // enough not to false-trigger at boot but short enough to recover quickly.
    // time_us_32 wraps every ~71 min, irrelevant for the 500 ms window and
    // ~10x cheaper than time_us_64 in a hot loop.
    #define VIDEO_OUTPUT_WATCHDOG_US 500000u

    // Rate-limit watchdog: no real display exceeds ~65 Hz vertical. If
    // video_frame_count advances above 75 fps, DMA chain state has drifted
    // (observed ~158 Hz in DVI mode); trigger a full hstx_resync() to
    // recover. A soft HSTX CSR toggle was tested and does not clear it.
    #define VIDEO_OUTPUT_OVERRATE_FPS 75u
    uint32_t last_frame_us = time_us_32();
    uint32_t last_frame_count_seen = video_frame_count;
    uint32_t overrate_last_us = last_frame_us;
    uint32_t overrate_last_frames = last_frame_count_seen;
#if defined(JACK_BYTEPUSHER_HSTX_DIAG_R12) && !defined(JACK_BYTEPUSHER_QUIET_CORE1_R15)
    uint32_t r12_last_irqs = r12_irq_count;
    uint32_t r12_last_empty = r12_empty_irq_count;
    uint32_t r12_last_dual = r12_dual_irq_count;
    uint32_t r12_last_unexpected = r12_unexpected_irq_count;
    uint32_t r12_last_busy = r12_expected_busy_count;
    printf("BYTEPUSHER_HSTX init reset=peripheral diag=r12\n");
#endif
#if HSTX_DEBUG
    // 1 Hz rate-diagnostic baseline
    uint32_t rate_last_us = last_frame_us;
    uint32_t rate_last_frames = last_frame_count_seen;
    uint32_t rate_last_irqs = irq_count;
#endif
    while (1) {
#if PICO_HSTX_ASYNC_LINE_GUARD
        // DMA_IRQ_0 may pre-empt pixel conversion without waiting for it.
        video_output_service_line_request(&handled_line_request);
#endif

        // ----------------------------------------------------------------
        // HSTX recovery loop
        //
        // The HSTX/DMA pipeline can wedge in two distinct failure modes that
        // both manifest as the monitor losing signal. Two cooperating
        // watchdogs detect them; both feed into a single recovery path that
        // calls hstx_resync() to rebuild the DMA chain from scratch.
        //
        //   1. Stuck-frame watchdog (per-iteration check)
        //      video_frame_count stops advancing — typical after a DMA IRQ
        //      overrun or HSTX FIFO underflow leaves the ping/pong chain in
        //      an inconsistent state. Detected when no new frame has been
        //      observed for VIDEO_OUTPUT_WATCHDOG_US (~500 ms).
        //
        //   2. Over-rate watchdog (1 Hz sampling window)
        //      video_frame_count advances faster than any real display can
        //      sync to (>75 fps; ~158 Hz observed in DVI mode after the chain
        //      drifts). Latches resync_requested for the next iteration.
        //
        // Originally these checks were gated on dvi_mode (signal loss was
        // only ever observed in DVI), but they are now run unconditionally
        // for HDMI as a safety net.
        //
        // hstx_resync() is the only recovery that has been shown to work;
        // a soft HSTX CSR toggle was tested and fails reliably.
        //
        // All elapsed-time comparisons use uint32_t subtraction, which is
        // safe across the ~71 min wrap of time_us_32().
        // ----------------------------------------------------------------

        uint32_t now = time_us_32();
        uint32_t current_count = video_frame_count;

        // Stuck-frame watchdog: refresh the "last progress" timestamp
        // whenever the IRQ-driven frame counter has moved forward.
        if (current_count != last_frame_count_seen)
        {
            last_frame_count_seen = current_count;
            last_frame_us = now;
        }

#if HSTX_DEBUG
        // 1 Hz diagnostic dump: frame/IRQ rates plus a snapshot of every
        // clock and HSTX/DMA register relevant to debugging a wedge.
        if ((now - rate_last_us) >= 1000000u)
        {
            uint32_t d_us = now - rate_last_us;
            uint32_t d_frames = current_count - rate_last_frames;
            uint32_t d_irqs = irq_count - rate_last_irqs;
            printf("video rate: %lu frames/s, %lu irqs/s (dvi=%d) clk_sys=%lu clk_hstx=%lu csr=%08lx resync=%d di_lvl=%lu underrun=%lu (+%lu/s)\n",
                   (unsigned long)((uint64_t)d_frames * 1000000u / d_us),
                   (unsigned long)((uint64_t)d_irqs * 1000000u / d_us),
                   dvi_mode ? 1 : 0,
                   (unsigned long)clock_get_hz(clk_sys),
                   (unsigned long)clock_get_hz(clk_hstx),
                   (unsigned long)hstx_ctrl_hw->csr,
                   resync_count,
                   (unsigned long)hstx_di_queue_get_level(),
                   (unsigned long)hstx_di_queue_get_underrun_count(),
                   (unsigned long)hstx_di_queue_get_underrun_delta());
            printf("  hstx_div=%08lx exp_sh=%08lx exp_tmds=%08lx ping_ctrl=%08lx pong_ctrl=%08lx\n",
                   (unsigned long)clocks_hw->clk[clk_hstx].div,
                   (unsigned long)hstx_ctrl_hw->expand_shift,
                   (unsigned long)hstx_ctrl_hw->expand_tmds,
                   (unsigned long)dma_hw->ch[DMACH_PING].al1_ctrl,
                   (unsigned long)dma_hw->ch[DMACH_PONG].al1_ctrl);
            uint32_t fc_sys_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
            uint32_t fc_hstx_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX);
            printf("  sys_ctrl=%08lx sys_div=%08lx pll_fbdiv=%lu pll_prim=%08lx fc_sys=%lu kHz fc_hstx=%lu kHz\n",
                   (unsigned long)clocks_hw->clk[clk_sys].ctrl,
                   (unsigned long)clocks_hw->clk[clk_sys].div,
                   (unsigned long)pll_sys_hw->fbdiv_int,
                   (unsigned long)pll_sys_hw->prim,
                   (unsigned long)fc_sys_khz,
                   (unsigned long)fc_hstx_khz);
            rate_last_us = now;
            rate_last_frames = current_count;
            rate_last_irqs = irq_count;
        }
#endif

        // Over-rate watchdog: once per second, compute fps over the elapsed
        // window. Above VIDEO_OUTPUT_OVERRATE_FPS the DMA chain has drifted
        // and the recovery is queued for this iteration's resync block.
        if ((now - overrate_last_us) >= 1000000u)
        {
            uint32_t d_us = now - overrate_last_us;
            uint32_t d_frames = current_count - overrate_last_frames;
            uint32_t fps = (uint32_t)((uint64_t)d_frames * 1000000u / d_us);
#if defined(JACK_BYTEPUSHER_HSTX_DIAG_R12) && !defined(JACK_BYTEPUSHER_QUIET_CORE1_R15)
            const uint32_t irqs = r12_irq_count;
            const uint32_t empty = r12_empty_irq_count;
            const uint32_t dual = r12_dual_irq_count;
            const uint32_t unexpected = r12_unexpected_irq_count;
            const uint32_t busy = r12_expected_busy_count;
            printf("BYTEPUSHER_HSTX elapsed_us=%lu frames=%lu fps=%lu irqs=%lu empty=%lu dual=%lu unexpected=%lu busy=%lu line=%lu phase=%u di=%lu\n",
                   (unsigned long)d_us,
                   (unsigned long)d_frames,
                   (unsigned long)fps,
                   (unsigned long)(irqs - r12_last_irqs),
                   (unsigned long)(empty - r12_last_empty),
                   (unsigned long)(dual - r12_last_dual),
                   (unsigned long)(unexpected - r12_last_unexpected),
                   (unsigned long)(busy - r12_last_busy),
                   (unsigned long)v_scanline,
                   vactive_cmdlist_posted ? 1u : 0u,
                   (unsigned long)hstx_di_queue_get_level());
            r12_last_irqs = irqs;
            r12_last_empty = empty;
            r12_last_dual = dual;
            r12_last_unexpected = unexpected;
            r12_last_busy = busy;
#endif
            if (fps > VIDEO_OUTPUT_OVERRATE_FPS)
            {
                resync_requested = true;
            }
            overrate_last_us = now;
            overrate_last_frames = current_count;
        }

        // Trigger recovery if either watchdog has fired. The DMA IRQ is
        // masked across hstx_resync() so it cannot observe the chain in a
        // half-rebuilt state, then all watchdog baselines are reset so the
        // next iteration measures fresh post-recovery progress.
        bool stall_wd = (now - last_frame_us) > VIDEO_OUTPUT_WATCHDOG_US;
        bool do_resync = resync_requested || stall_wd;

        uint32_t guard_faults = callback_guard_fault_count;
        if (guard_faults != reported_callback_guard_faults) {
            printf("HSTX callback guard fault! kind=%s ptr=%08lx guard=%08lx count=%lu\n",
                   callback_guard_fault_kind == 1 ? "scanline" :
                   callback_guard_fault_kind == 2 ? "vsync" : "background",
                   (unsigned long)callback_guard_fault_pointer,
                   (unsigned long)callback_guard_fault_complement,
                   (unsigned long)guard_faults);
            reported_callback_guard_faults = guard_faults;
        }

        if (do_resync)
        {
            // Which watchdog fired matters for diagnosis: "stall" means the
            // DMA IRQ stopped feeding the chain (missed deadline / wedge),
            // "overrate" means the ping/pong chain drifted and is consuming
            // buffers faster than the pixel clock allows.
            const char *cause = (resync_requested && stall_wd) ? "overrate+stall"
                                : (resync_requested ? "overrate" : "stall");
            resync_count++;
            irq_set_enabled(DMA_IRQ_0, false);
            hstx_resync();
            resync_requested = false;
            irq_set_enabled(DMA_IRQ_0, true);
            last_frame_us = time_us_32();
            last_frame_count_seen = video_frame_count;
            overrate_last_us = last_frame_us;
            overrate_last_frames = video_frame_count;
            printf("HSTX resync performed! cause=%s di_level=%lu Total resyncs since boot: %d\n",
                   cause, (unsigned long)hstx_di_queue_get_level(), resync_count);
        }

#if defined(JACK_MEGADUCK_DIRECT_BACKGROUND028) || defined(JACK_MEGADUCK_LOW_NOISE029) || defined(JACK_MEGADUCK_STACK_ISOLATE030)
        audio_pump();
#else
        video_output_task_fn task = background_task;
        const uintptr_t task_pointer = (uintptr_t)task;
        const uintptr_t task_complement = background_task_guard;
        if ((task_pointer ^ task_complement) != ~(uintptr_t)0) {
            if (!callback_guard_fault_latched) {
                callback_guard_fault_pointer = task_pointer;
                callback_guard_fault_complement = task_complement;
                callback_guard_fault_kind = 3;
                callback_guard_fault_count++;
                callback_guard_fault_latched = true;
            }
            /* The complement lives in .data and is the authoritative copy.
             * Repair the vulnerable .bss pointer before making the call. */
            task = (video_output_task_fn)(~task_complement);
            background_task = task;
        }
        if (task) {
            task();
        }
#endif
        tight_loop_contents();
    }
}

void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate)
{
    configure_audio_packets(sample_rate);
}

/// @brief Returns the number of times the video output has auto-resynced
/// (via hstx_resync()) since boot. This can be used to detect if the output is having trouble
/// keeping up and is frequently resyncing.
/// @param  
/// @return /
int get_video_output_resync_count(void)
{
    return resync_count;
}


