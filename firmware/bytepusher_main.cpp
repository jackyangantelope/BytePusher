
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bp_pager.h"
#include "bytepusher_log15.h"
#include "ff.h"
#include "hardware/psram.h"
#include "jack_wave1_audio.h"
#include "jack_wave1_runtime.h"
#include "pico/stdlib.h"

namespace {

constexpr uint32_t kInstructionsPerFrame = 65536u;
constexpr uint32_t kFrameMicroseconds = 16667u;
constexpr uint32_t kAudioRate = 44100u;
constexpr uint32_t kAudioFramesPerVideoFrame = kAudioRate / 60u;
constexpr uint32_t kIncrementalSavePeriodFrames = 30u;
constexpr uint32_t kSaveSyncPeriodFrames = 300u;
constexpr uint16_t kCacheSlots = 1536u;
constexpr uint32_t kStateHeaderBytes = 520u;
constexpr char kStateMagic[8] = {'J', 'B', 'P', 'S', 'T', '0', '1', '\0'};

struct BackingStore {
    FIL program;
    FIL state;
    FSIZE_t program_size;
    uint8_t present[BP_PAGE_COUNT / 8u];
};

uint8_t __uninitialized_psram("bytepusher_cache")
    g_cache[static_cast<size_t>(kCacheSlots) * BP_PAGE_BYTES];
bp_slot_t g_slots[kCacheSlots];
int16_t g_page_to_slot[BP_PAGE_COUNT];
bp_pager_t g_pager;
BackingStore g_backing;

constexpr uint32_t kDecodedInstructionSlots = 8192u;
struct DecodedInstruction {
    uint32_t pc;
    uint32_t source;
    uint32_t destination;
    uint32_t jump;
    uint32_t page_generation;
};
DecodedInstruction g_decoded_instructions[kDecodedInstructionSlots];
uint32_t g_page_generation[BP_PAGE_COUNT];
uint32_t g_decode_misses;
uint32_t g_decode_fallbacks;
uint32_t g_jump_reloads;
uint32_t g_fast_read_hits;
uint32_t g_fast_write_hits;
uint32_t g_fast_accesses_since_touch;

constexpr uint32_t kValueCacheEntries = BP_PAGE_BYTES;
constexpr uint32_t kValueCacheMask = kValueCacheEntries - 1u;
constexpr uint16_t kValueCacheValid = 0x8000u;
uint16_t g_value_cache_tags[kValueCacheEntries];
uint8_t g_value_cache_values[kValueCacheEntries];
uint32_t g_value_read_misses;
uint32_t g_value_write_commits;
int16_t g_audio[kAudioFramesPerVideoFrame * 2u];

bool page_present(const BackingStore *backing, uint16_t page)
{
    return (backing->present[page >> 3u] & (1u << (page & 7u))) != 0u;
}

void set_page_present(BackingStore *backing, uint16_t page)
{
    backing->present[page >> 3u] |= static_cast<uint8_t>(1u << (page & 7u));
}

bool read_exact(FIL *file, void *destination, UINT bytes)
{
    UINT read = 0;
    return f_read(file, destination, bytes, &read) == FR_OK && read == bytes;
}

bool write_exact(FIL *file, const void *source, UINT bytes)
{
    UINT written = 0;
    return f_write(file, source, bytes, &written) == FR_OK && written == bytes;
}

bool backing_read_page(void *context, uint16_t page, uint8_t *destination)
{
    BackingStore *backing = static_cast<BackingStore *>(context);
    memset(destination, 0, BP_PAGE_BYTES);
    if (page_present(backing, page)) {
        const FSIZE_t offset = kStateHeaderBytes +
                               static_cast<FSIZE_t>(page) * BP_PAGE_BYTES;
        return f_lseek(&backing->state, offset) == FR_OK &&
               read_exact(&backing->state, destination, BP_PAGE_BYTES);
    }
    const FSIZE_t offset = static_cast<FSIZE_t>(page) * BP_PAGE_BYTES;
    if (offset >= backing->program_size) return true;
    const UINT available = static_cast<UINT>(
        backing->program_size - offset > BP_PAGE_BYTES
            ? BP_PAGE_BYTES : backing->program_size - offset);
    if (f_lseek(&backing->program, offset) != FR_OK) return false;
    UINT read = 0;
    return f_read(&backing->program, destination, available, &read) == FR_OK &&
           read == available;
}

bool backing_write_page(void *context, uint16_t page, const uint8_t *source)
{
    BackingStore *backing = static_cast<BackingStore *>(context);
    const FSIZE_t offset = kStateHeaderBytes +
                           static_cast<FSIZE_t>(page) * BP_PAGE_BYTES;
    if (f_lseek(&backing->state, offset) != FR_OK ||
        !write_exact(&backing->state, source, BP_PAGE_BYTES)) {
        return false;
    }
    if (!page_present(backing, page)) {
        set_page_present(backing, page);
        const FSIZE_t bitmap_offset = sizeof(kStateMagic) + (page >> 3u);
        if (f_lseek(&backing->state, bitmap_offset) != FR_OK ||
            !write_exact(&backing->state, &backing->present[page >> 3u], 1u)) {
            return false;
        }
    }
    return true;
}

void make_state_path(const char *program_path, char *state_path, size_t size)
{
    const char *name = strrchr(program_path, '/');
    name = name == nullptr ? program_path : name + 1;
    char base[96];
    strncpy(base, name, sizeof(base) - 1u);
    base[sizeof(base) - 1u] = '\0';
    char *dot = strrchr(base, '.');
    if (dot != nullptr) *dot = '\0';
    snprintf(state_path, size, "/SAVES/BYTEPUSHER/%s.state", base);
}

bool open_backing(const char *program_path)
{
    memset(&g_backing, 0, sizeof(g_backing));
    if (f_open(&g_backing.program, program_path, FA_READ) != FR_OK) return false;
    g_backing.program_size = f_size(&g_backing.program);
    if (g_backing.program_size > BP_VIRTUAL_BYTES) {
        f_close(&g_backing.program);
        return false;
    }

    (void)f_mkdir("/SAVES");
    (void)f_mkdir("/SAVES/BYTEPUSHER");
    char state_path[JACK_WAVE1_PATH_MAX];
    make_state_path(program_path, state_path, sizeof(state_path));
    FRESULT result = f_open(&g_backing.state, state_path, FA_READ | FA_WRITE);
    if (result == FR_NO_FILE) {
        result = f_open(&g_backing.state, state_path,
                        FA_CREATE_ALWAYS | FA_READ | FA_WRITE);
        if (result != FR_OK ||
            !write_exact(&g_backing.state, kStateMagic, sizeof(kStateMagic)) ||
            !write_exact(&g_backing.state, g_backing.present,
                         sizeof(g_backing.present))) {
            f_close(&g_backing.program);
            return false;
        }
    } else if (result == FR_OK) {
        char magic[sizeof(kStateMagic)];
        if (!read_exact(&g_backing.state, magic, sizeof(magic)) ||
            memcmp(magic, kStateMagic, sizeof(magic)) != 0 ||
            !read_exact(&g_backing.state, g_backing.present,
                        sizeof(g_backing.present))) {
            f_close(&g_backing.state);
            f_close(&g_backing.program);
            return false;
        }
    } else {
        f_close(&g_backing.program);
        return false;
    }
    return true;
}

uint16_t encode_keyboard(uint16_t pad)
{
    uint16_t keys = 0;
    const uint16_t mapping[12] = {
        JACK_BUTTON_A, JACK_BUTTON_UP, JACK_BUTTON_X, JACK_BUTTON_R,
        JACK_BUTTON_LEFT, JACK_BUTTON_START, JACK_BUTTON_RIGHT, JACK_BUTTON_L,
        JACK_BUTTON_Y, JACK_BUTTON_DOWN, JACK_BUTTON_SELECT, JACK_BUTTON_B,
    };
    for (unsigned int key = 0; key < 12u; ++key) {
        if ((pad & mapping[key]) != 0u) keys |= static_cast<uint16_t>(1u << key);
    }
    return keys;
}

inline uint32_t __not_in_flash_func(read_be24)(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 16u) |
           (static_cast<uint32_t>(data[1]) << 8u) | data[2];
}

bool __not_in_flash_func(read24_fast)(uint32_t address, uint32_t *value)
{
    const uint8_t *data = nullptr;
    if (bp_pager_map_read(&g_pager, address, 3u, &data)) {
        *value = read_be24(data);
        return true;
    }
    return bp_pager_read24(&g_pager, address, value);
}

uint32_t __not_in_flash_func(virtual_page)(uint32_t address)
{
    return (address & (BP_VIRTUAL_BYTES - 1u)) >> BP_PAGE_SHIFT;
}

void __not_in_flash_func(fast_touch_slot)(int16_t slot)
{
    if ((++g_fast_accesses_since_touch & 0xffu) != 0u) return;
    ++g_pager.use_clock;
    g_pager.slots[slot].last_use = g_pager.use_clock;
}

bool __not_in_flash_func(fast_slot)(uint32_t address, int16_t *slot, uint16_t *offset)
{
    address &= (BP_VIRTUAL_BYTES - 1u);
    const uint16_t page = static_cast<uint16_t>(address >> BP_PAGE_SHIFT);
    const int16_t candidate = g_pager.page_to_slot[page];
    if (candidate == BP_INVALID_SLOT) return false;
    const bp_slot_t *metadata = &g_pager.slots[candidate];
    if (!metadata->valid || metadata->virtual_page != page) return false;
    *slot = candidate;
    *offset = static_cast<uint16_t>(address & (BP_PAGE_BYTES - 1u));
    return true;
}

bool __not_in_flash_func(fast_read8)(uint32_t address, uint8_t *value)
{
    int16_t slot;
    uint16_t offset;
    if (!fast_slot(address, &slot, &offset))
        return bp_pager_read8(&g_pager, address, value);
    fast_touch_slot(slot);
    *value = g_pager.cache[static_cast<size_t>(slot) * BP_PAGE_BYTES + offset];
    ++g_fast_read_hits;
    return true;
}

bool __not_in_flash_func(fast_write8)(uint32_t address, uint8_t value)
{
    int16_t slot;
    uint16_t offset;
    if (!fast_slot(address, &slot, &offset))
        return bp_pager_write8(&g_pager, address, value);
    fast_touch_slot(slot);
    g_pager.cache[static_cast<size_t>(slot) * BP_PAGE_BYTES + offset] = value;
    g_pager.slots[slot].dirty = true;
    ++g_fast_write_hits;
    return true;
}

inline uint32_t __not_in_flash_func(value_cache_index)(uint32_t address)
{
    return address & kValueCacheMask;
}

inline uint16_t __not_in_flash_func(value_cache_tag)(uint32_t address)
{
    return static_cast<uint16_t>(kValueCacheValid | virtual_page(address));
}

static __force_inline bool cached_read8(uint32_t address, uint8_t *value)
{
    address &= (BP_VIRTUAL_BYTES - 1u);
    const uint32_t index = value_cache_index(address);
    const uint16_t tag = value_cache_tag(address);
    if (g_value_cache_tags[index] == tag) {
        *value = g_value_cache_values[index];
        return true;
    }
    if (!fast_read8(address, value)) return false;
    g_value_cache_tags[index] = tag;
    g_value_cache_values[index] = *value;
    ++g_value_read_misses;
    return true;
}

static __force_inline bool write8_vm(uint32_t address, uint8_t value)
{
    address &= (BP_VIRTUAL_BYTES - 1u);
    const uint32_t index = value_cache_index(address);
    const uint16_t tag = value_cache_tag(address);
    if (g_value_cache_tags[index] == tag &&
        g_value_cache_values[index] == value) {
        return true;
    }
    if (!fast_write8(address, value)) return false;
    g_value_cache_tags[index] = tag;
    g_value_cache_values[index] = value;
    ++g_value_write_commits;
    ++g_page_generation[virtual_page(address)];
    return true;
}

bool __not_in_flash_func(overwrites_current_jump)(uint32_t destination, uint32_t pc)
{
    const uint32_t jump_address = (pc + 6u) & (BP_VIRTUAL_BYTES - 1u);
    return ((destination - jump_address) & (BP_VIRTUAL_BYTES - 1u)) < 3u;
}

static __force_inline DecodedInstruction *decode_instruction(uint32_t pc)
{
    const uint32_t page = virtual_page(pc);
    DecodedInstruction *slot =
        &g_decoded_instructions[(pc >> 3u) & (kDecodedInstructionSlots - 1u)];
    if (slot->pc == pc && slot->page_generation == g_page_generation[page]) {
        return slot;
    }

    const uint8_t *encoded = nullptr;
    if (!bp_pager_map_read(&g_pager, pc, 9u, &encoded)) {
        ++g_decode_fallbacks;
        return nullptr;
    }
    slot->pc = pc;
    slot->source = read_be24(encoded);
    slot->destination = read_be24(encoded + 3u);
    if (overwrites_current_jump(slot->destination, pc))
        slot->destination |= 0x01000000u;
    slot->jump = read_be24(encoded + 6u);
    slot->page_generation = g_page_generation[page];
    ++g_decode_misses;
    return slot;
}

void initialize_decode_cache()
{
    memset(g_page_generation, 0, sizeof(g_page_generation));
    for (DecodedInstruction &decoded : g_decoded_instructions)
        decoded.pc = BP_VIRTUAL_BYTES;
    g_decode_misses = 0;
    g_decode_fallbacks = 0;
    g_jump_reloads = 0;
    g_fast_read_hits = 0;
    g_fast_write_hits = 0;
    g_fast_accesses_since_touch = 0;
    memset(g_value_cache_tags, 0, sizeof(g_value_cache_tags));
    g_value_read_misses = 0;
    g_value_write_commits = 0;
}

__attribute__((noinline)) bool __not_in_flash_func(run_frame)(uint16_t keys)
{
    if (!write8_vm(0u, static_cast<uint8_t>(keys >> 8u)) ||
        !write8_vm(1u, static_cast<uint8_t>(keys))) {
        return false;
    }
    uint32_t pc = 0;
    if (!read24_fast(2u, &pc)) return false;
    for (uint32_t instruction = 0; instruction < kInstructionsPerFrame;
         ++instruction) {
        uint32_t jump = 0;
        uint8_t value = 0;
        DecodedInstruction *decoded = decode_instruction(pc);
        if (decoded != nullptr) {
            if (!cached_read8(decoded->source, &value) ||
                !write8_vm(decoded->destination, value)) {
                return false;
            }
            if ((decoded->destination & 0x01000000u) != 0u) {
                if (!read24_fast(pc + 6u, &jump)) return false;
                ++g_jump_reloads;
            } else {
                jump = decoded->jump;
            }
        } else {
            uint32_t source = 0;
            uint32_t destination = 0;
            if (!read24_fast(pc, &source) ||
                !read24_fast(pc + 3u, &destination) ||
                !fast_read8(source, &value) ||
                !write8_vm(destination, value) ||
                !read24_fast(pc + 6u, &jump)) {
                return false;
            }
        }
        pc = jump;
    }
    return true;
}

uint16_t palette_color(uint8_t value)
{
    if (value >= 216u) {
        const unsigned int gray = (value - 216u) * 31u / 39u;
        return jack_wave1_rgb555(gray, gray, gray);
    }
    const unsigned int red = value / 36u;
    const unsigned int green = (value / 6u) % 6u;
    const unsigned int blue = value % 6u;
    return jack_wave1_rgb555(red * 31u / 5u, green * 31u / 5u,
                             blue * 31u / 5u);
}

uint16_t g_palette[256];

void initialize_palette()
{
    for (unsigned int value = 0; value < 256u; ++value)
        g_palette[value] = palette_color(static_cast<uint8_t>(value));
}

bool render_frame()
{
    uint8_t video_page = 0;
    if (!bp_pager_read8(&g_pager, 5u, &video_page)) return false;
    const uint32_t base = static_cast<uint32_t>(video_page) << 16u;
    uint16_t *framebuffer = jack_wave1_framebuffer();
    for (unsigned int y = 0; y < 240u; ++y) {
        const unsigned int source_y = y * 256u / 240u;
        const uint8_t *source_row = nullptr;
        if (!bp_pager_map_read(&g_pager, base + source_y * 256u, 256u,
                               &source_row)) {
            return false;
        }
        uint16_t *row = framebuffer + y * JACK_WAVE1_SCREEN_WIDTH;
        for (unsigned int x = 0; x < 40u; ++x) row[x] = 0u;
        for (unsigned int x = 0; x < 240u; ++x) {
            const unsigned int source_x = x * 256u / 240u;
            row[40u + x] = g_palette[source_row[source_x]];
        }
        for (unsigned int x = 280u; x < 320u; ++x) row[x] = 0u;
    }
    return true;
}

__attribute__((noinline)) bool render_audio_frame()
{
    uint8_t audio_bank_hi = 0, audio_bank_lo = 0;
    if (!bp_pager_read8(&g_pager, 6u, &audio_bank_hi) ||
        !bp_pager_read8(&g_pager, 7u, &audio_bank_lo)) return false;

    // Addresses 6-7 are a big-endian 256-byte audio bank selector.
    const uint32_t audio_address = (static_cast<uint32_t>(audio_bank_hi) << 16u) |
                                   (static_cast<uint32_t>(audio_bank_lo) << 8u);
    const uint8_t *source = nullptr;
    if (!bp_pager_map_read(&g_pager,
                           audio_address,
                           256u, &source)) {
        return false;
    }

    constexpr uint32_t source_step = (255u << 16u) /
                                     (kAudioFramesPerVideoFrame - 1u);
    uint32_t position = 0u;
    for (size_t frame = 0; frame < kAudioFramesPerVideoFrame; ++frame) {
        const uint32_t index = position >> 16u;
        const uint32_t fraction = position & 0xffffu;
        const int32_t first = static_cast<int8_t>(source[index]);
        const int32_t second = static_cast<int8_t>(source[index < 255u ? index + 1u : index]);
        const int32_t interpolated =
            first + (((second - first) * static_cast<int32_t>(fraction)) >> 16u);
        const int16_t sample = static_cast<int16_t>(interpolated * 256);
        g_audio[frame * 2u] = sample;
        g_audio[frame * 2u + 1u] = sample;
        position += source_step;
    }
    jack_wave1_audio_submit(g_audio, kAudioFramesPerVideoFrame);
    return true;
}

[[noreturn]] void stop_with_error(const char *message)
{
    printf("BYTEPUSHER FAIL %s\n", message);
    jack_wave1_show_error("BytePusher error", message);
    for (;;) tight_loop_contents();
}

}

#if BYTEPUSHER_PROFILE_OC480
extern "C" void bytepusher_clock13_report(void);
#endif

int main()
{
    const bool initialized = jack_wave1_init("BytePusher");

    if (!initialized) stdio_init_all();
#if BYTEPUSHER_PROFILE_OC480
    bytepusher_clock13_report();
#endif
    if (!initialized) {
        for (;;) tight_loop_contents();
    }
    if (!jack_wave1_mount_sd()) stop_with_error("Cannot mount SD card");

    char program_path[JACK_WAVE1_PATH_MAX];
    if (!jack_wave1_select_file("BytePusher programs", "/roms/BYTEPUSHER",
                                ".BytePusher .bytepusher .bp",
                                program_path, sizeof(program_path))) {
        stop_with_error("No program in /roms/BYTEPUSHER");
    }
    if (!open_backing(program_path)) stop_with_error("Cannot open program/state");

    bp_backing_store_t backing = {
        .context = &g_backing,
        .read_page = backing_read_page,
        .write_page = backing_write_page,
    };
    if (!bp_pager_init(&g_pager, g_cache, g_slots, kCacheSlots,
                       g_page_to_slot, backing)) {
        stop_with_error("Pager initialization failed");
    }
    initialize_decode_cache();
    initialize_palette();
    if (!jack_wave1_audio_begin(kAudioRate))
        stop_with_error("HDMI audio initialization failed");

#if BYTEPUSHER_PROFILE_OC480
    constexpr const char *performance_profile = "fruit-jam-oc480-pll126";
#else
    constexpr const char *performance_profile = "fruit-jam-252";
#endif
    printf("BYTEPUSHER PLAY begin file=%s bytes=%lu cache=%lu hot=%s\n",
           program_path, static_cast<unsigned long>(g_backing.program_size),
           static_cast<unsigned long>(sizeof(g_cache)), performance_profile);
    bytepusher_log15_start();
    uint32_t next_frame = time_us_32();
    uint32_t fps_window_start = next_frame;
    uint32_t fps_window_work_us = 0;
    uint32_t fps_window_max_work_us = 0;
    uint32_t fps_window_vm_us = 0;
    uint32_t fps_window_max_vm_us = 0;
    uint32_t fps_window_render_us = 0;
    uint32_t fps_window_max_render_us = 0;
    uint32_t fps_window_vsync_us = 0;
    uint32_t fps_window_max_vsync_us = 0;
    uint32_t fps_window_overruns = 0;
    uint32_t fps_window_boundary_waited = 0;
    uint32_t fps_window_boundary_missed = 0;
    uint32_t fps_window_save_us = 0;
    uint32_t fps_window_max_save_us = 0;
    uint32_t fps_window_save_pages = 0;
    uint32_t fps_window_save_syncs = 0;
    uint16_t save_cursor = 0;
    uint32_t frame = 0;
    for (;;) {
        bytepusher_log15_pump();
        const uint32_t starting_video_frame = jack_wave1_frame_counter();
        const uint32_t frame_work_start = time_us_32();
        if (!run_frame(encode_keyboard(jack_wave1_poll_buttons()))) {
            stop_with_error("VM memory access failed");
        }
        if (!render_audio_frame()) stop_with_error("Audio memory access failed");
        const uint32_t frame_vm_us = time_us_32() - frame_work_start;
        const uint32_t frame_render_start = time_us_32();
        if (!render_frame()) stop_with_error("Video memory access failed");
        const uint32_t frame_render_us = time_us_32() - frame_render_start;
        const uint32_t frame_vsync_start = time_us_32();
        const bool frame_boundary_waited =
            jack_wave1_wait_frame_boundary(starting_video_frame);
        const uint32_t frame_vsync_us = time_us_32() - frame_vsync_start;
        jack_wave1_audio_report();
        const uint32_t frame_work_us = time_us_32() - frame_work_start;
        fps_window_work_us += frame_work_us;
        fps_window_vm_us += frame_vm_us;
        fps_window_render_us += frame_render_us;
        fps_window_vsync_us += frame_vsync_us;
        if (frame_boundary_waited)
            ++fps_window_boundary_waited;
        else
            ++fps_window_boundary_missed;
        if (frame_work_us > fps_window_max_work_us)
            fps_window_max_work_us = frame_work_us;
        if (frame_vm_us > fps_window_max_vm_us)
            fps_window_max_vm_us = frame_vm_us;
        if (frame_render_us > fps_window_max_render_us)
            fps_window_max_render_us = frame_render_us;
        if (frame_vsync_us > fps_window_max_vsync_us)
            fps_window_max_vsync_us = frame_vsync_us;
        if (frame_work_us > kFrameMicroseconds) ++fps_window_overruns;
        ++frame;
        if (frame % kIncrementalSavePeriodFrames == 0u) {
            const uint32_t save_start = time_us_32();
            bool wrote_page = false;
            if (!bp_pager_flush_next(&g_pager, &save_cursor, &wrote_page)) {
                stop_with_error("State page flush failed");
            }
            if (frame % kSaveSyncPeriodFrames == 0u) {
                if (f_sync(&g_backing.state) != FR_OK) {
                    stop_with_error("State sync failed");
                }
                ++fps_window_save_syncs;
            }
            const uint32_t save_us = time_us_32() - save_start;
            fps_window_save_us += save_us;
            if (save_us > fps_window_max_save_us)
                fps_window_max_save_us = save_us;
            if (wrote_page) ++fps_window_save_pages;
        }
        next_frame += kFrameMicroseconds;
        bytepusher_log15_pump();
        while (static_cast<int32_t>(next_frame - time_us_32()) > 0) {
            bytepusher_log15_pump();
            tight_loop_contents();
        }
        if (frame % 60u == 0u) {
            const uint32_t now = time_us_32();
            const uint32_t window_us = now - fps_window_start;
            const uint32_t fps_x100 = window_us == 0u ? 0u :
                static_cast<uint32_t>((6000000000ull + window_us / 2u) / window_us);
            bytepusher_log15_printf("BYTEPUSHER PLAY frame=%lu fps=%lu.%02lu window_us=%lu "
                   "work_us(avg=%lu max=%lu) vm_us(avg=%lu max=%lu) "
                   "render_us(avg=%lu max=%lu) vsync_us(avg=%lu max=%lu) "
                   "decode(h=%lu m=%lu fb=%lu jr=%lu) fast(r=%lu w=%lu) "
                   "value(h=%lu m=%lu skip=%lu commit=%lu) "
                   "boundary(wait=%lu miss=%lu) "
                   "save_us(total=%lu max=%lu pages=%lu syncs=%lu) "
                   "overruns=%lu faults=%lu hits=%lu writes=%lu log_drop=%lu\n",
                   static_cast<unsigned long>(frame),
                   static_cast<unsigned long>(fps_x100 / 100u),
                   static_cast<unsigned long>(fps_x100 % 100u),
                   static_cast<unsigned long>(window_us),
                   static_cast<unsigned long>(fps_window_work_us / 60u),
                   static_cast<unsigned long>(fps_window_max_work_us),
                   static_cast<unsigned long>(fps_window_vm_us / 60u),
                   static_cast<unsigned long>(fps_window_max_vm_us),
                   static_cast<unsigned long>(fps_window_render_us / 60u),
                   static_cast<unsigned long>(fps_window_max_render_us),
                   static_cast<unsigned long>(fps_window_vsync_us / 60u),
                   static_cast<unsigned long>(fps_window_max_vsync_us),
                   static_cast<unsigned long>(60u * kInstructionsPerFrame -
                                              g_decode_misses - g_decode_fallbacks),
                   static_cast<unsigned long>(g_decode_misses),
                   static_cast<unsigned long>(g_decode_fallbacks),
                   static_cast<unsigned long>(g_jump_reloads),
                   static_cast<unsigned long>(g_fast_read_hits),
                   static_cast<unsigned long>(g_fast_write_hits),
                   static_cast<unsigned long>(60u * kInstructionsPerFrame -
                                              g_value_read_misses),
                   static_cast<unsigned long>(g_value_read_misses),
                   static_cast<unsigned long>(60u * (kInstructionsPerFrame + 2u) -
                                              g_value_write_commits),
                   static_cast<unsigned long>(g_value_write_commits),
                   static_cast<unsigned long>(fps_window_boundary_waited),
                   static_cast<unsigned long>(fps_window_boundary_missed),
                   static_cast<unsigned long>(fps_window_save_us),
                   static_cast<unsigned long>(fps_window_max_save_us),
                   static_cast<unsigned long>(fps_window_save_pages),
                   static_cast<unsigned long>(fps_window_save_syncs),
                   static_cast<unsigned long>(fps_window_overruns),
                   static_cast<unsigned long>(g_pager.stats.page_faults),
                   static_cast<unsigned long>(g_pager.stats.page_hits),
                   static_cast<unsigned long>(g_pager.stats.dirty_writes),
                   static_cast<unsigned long>(bytepusher_log15_dropped()));
            fps_window_start = now;
            fps_window_work_us = 0;
            fps_window_max_work_us = 0;
            fps_window_vm_us = 0;
            fps_window_max_vm_us = 0;
            fps_window_render_us = 0;
            fps_window_max_render_us = 0;
            fps_window_vsync_us = 0;
            fps_window_max_vsync_us = 0;
            g_decode_misses = 0;
            g_decode_fallbacks = 0;
            g_jump_reloads = 0;
            g_fast_read_hits = 0;
            g_fast_write_hits = 0;
            g_value_read_misses = 0;
            g_value_write_commits = 0;
            fps_window_overruns = 0;
            fps_window_boundary_waited = 0;
            fps_window_boundary_missed = 0;
            fps_window_save_us = 0;
            fps_window_max_save_us = 0;
            fps_window_save_pages = 0;
            fps_window_save_syncs = 0;
        }
    }
}
