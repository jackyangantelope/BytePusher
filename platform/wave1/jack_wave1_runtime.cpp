
#include "jack_wave1_runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifndef JACK_WAVE1_SYS_CLOCK_KHZ
#define JACK_WAVE1_SYS_CLOCK_KHZ 252000u
#endif
#ifndef JACK_WAVE1_PERI_CLOCK_HZ
#define JACK_WAVE1_PERI_CLOCK_HZ (JACK_WAVE1_SYS_CLOCK_KHZ * 1000u)
#endif
#ifndef JACK_WAVE1_FLASH_MAX_HZ
#define JACK_WAVE1_FLASH_MAX_HZ 45000000u
#endif
#ifndef JACK_WAVE1_PSRAM_MAX_HZ
#define JACK_WAVE1_PSRAM_MAX_HZ 133000000u
#endif
#ifndef JACK_WAVE1_USE_PLL_USB_FOR_HSTX
#define JACK_WAVE1_USE_PLL_USB_FOR_HSTX 0
#endif
#ifndef JACK_WAVE1_ALLOW_FRACTIONAL_HSTX
#define JACK_WAVE1_ALLOW_FRACTIONAL_HSTX 0
#endif

#include "ff.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#if JACK_WAVE1_SYS_CLOCK_KHZ > 252000u
#include "hardware/pll.h"
#include "hardware/psram.h"
#include "hardware/structs/pll.h"
#include "hardware/sync.h"
#endif
#include "hstx.h"
#include "nespad.h"
#include "pico/stdlib.h"
#include "tf_card.h"
#include "font_8x8.h"

extern "C" bool jack_wave1_usb_init(void);
extern "C" uint16_t jack_wave1_usb_poll_buttons(void);
#ifdef JACK_WAVE1_EXTERNAL_HSTX_CLOCK
extern "C" bool jack_wave1_configure_hstx_clock(void);
#endif

namespace {
#ifndef JACK_WAVE1_MAX_FILES
#define JACK_WAVE1_MAX_FILES 64
#endif
constexpr unsigned kMaximumFiles = JACK_WAVE1_MAX_FILES;
constexpr unsigned kFileNameBytes = 128;
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0x7fff;
constexpr uint16_t kDark = 0x0842;
constexpr uint16_t kMint = 0x3f70;
constexpr uint32_t kSystemHz = JACK_WAVE1_SYS_CLOCK_KHZ * 1000u;
constexpr uint32_t kPeriHz = JACK_WAVE1_PERI_CLOCK_HZ;
constexpr uint32_t kHstxHz = 126000000u;
constexpr bool kExperimentalOverclock = kSystemHz > 252000000u;

#if !JACK_WAVE1_USE_PLL_USB_FOR_HSTX && !JACK_WAVE1_ALLOW_FRACTIONAL_HSTX && !defined(JACK_WAVE1_EXTERNAL_HSTX_CLOCK)
static_assert(kSystemHz % kHstxHz == 0u,
              "clk_sys must integer-divide to the fixed 126 MHz HSTX clock");
#endif

constexpr uint32_t ceiling_div_u64(uint64_t numerator, uint64_t denominator)
{
    return static_cast<uint32_t>((numerator + denominator - 1u) / denominator);
}

constexpr uint32_t kFlashDivider =
    ceiling_div_u64(kSystemHz, JACK_WAVE1_FLASH_MAX_HZ);
constexpr uint32_t kFlashRxDelayUnclamped =
    (3u * JACK_WAVE1_SYS_CLOCK_KHZ + 189000u) / 378000u;
constexpr uint32_t kFlashRxDelay =
    kFlashRxDelayUnclamped > 7u ? 7u : kFlashRxDelayUnclamped;
constexpr uint32_t kPsramDividerInitial =
    ceiling_div_u64(kSystemHz, JACK_WAVE1_PSRAM_MAX_HZ);
constexpr uint32_t kPsramDivider =
    kPsramDividerInitial == 1u && kSystemHz > 100000000u
        ? 2u : kPsramDividerInitial;
constexpr uint32_t kPsramRxDelay =
    kPsramDivider + (kSystemHz / kPsramDivider > 100000000u ? 1u : 0u);
constexpr uint32_t kPsramMaxSelect = static_cast<uint32_t>(
    (8000ull * kSystemHz) / (64ull * 1000000000ull));
constexpr uint32_t kPsramDeselectCycles =
    ceiling_div_u64(18ull * kSystemHz, 1000000000ull);
constexpr uint32_t kPsramMinDeselect =
    kPsramDeselectCycles > (kPsramDivider + 1u) / 2u
        ? kPsramDeselectCycles - (kPsramDivider + 1u) / 2u : 0u;

static_assert(kFlashDivider <= 255u, "QMI M0 divider is eight bits");
static_assert(kPsramDivider <= 255u, "QMI M1 divider is eight bits");
static_assert(kPsramRxDelay <= 7u, "QMI M1 RX delay is three bits");

FATFS g_fatfs;
bool g_sd_mounted = false;
bool g_picker_auto_select_single = false;
bool g_memory_timing_ready = !kExperimentalOverclock;
char g_file_names[kMaximumFiles][kFileNameBytes];
unsigned g_file_count = 0;

#if JACK_WAVE1_SYS_CLOCK_KHZ > 252000u
void __no_inline_not_in_flash_func(apply_qmi_timings)(uint32_t flash_timing,
                                                       uint32_t psram_timing)
{

    qmi_hw->m[1].timing = psram_timing;
    qmi_hw->m[0].timing = flash_timing;
    __compiler_memory_barrier();
}

bool prepare_external_memory_for_target_clock()
{
    const bool psram_available = psram_is_available();
    const bool psram_params_ok = psram_available &&
        psram_set_params(kPsramDivider, kPsramRxDelay,
                         kPsramMaxSelect, kPsramMinDeselect) == 0;

    uint32_t flash_timing = qmi_hw->m[0].timing;
    flash_timing &= ~(QMI_M0_TIMING_MIN_DESELECT_BITS |
                      QMI_M0_TIMING_RXDELAY_BITS |
                      QMI_M0_TIMING_CLKDIV_BITS);
    flash_timing |= (14u << QMI_M0_TIMING_MIN_DESELECT_LSB) |
                    (kFlashRxDelay << QMI_M0_TIMING_RXDELAY_LSB) |
                    (kFlashDivider << QMI_M0_TIMING_CLKDIV_LSB);

    uint32_t psram_timing = qmi_hw->m[1].timing;
    if (psram_params_ok) {
        psram_timing &= ~(QMI_M1_TIMING_MAX_SELECT_BITS |
                          QMI_M1_TIMING_MIN_DESELECT_BITS |
                          QMI_M1_TIMING_RXDELAY_BITS |
                          QMI_M1_TIMING_CLKDIV_BITS);
        psram_timing |= (kPsramMaxSelect << QMI_M1_TIMING_MAX_SELECT_LSB) |
                        (kPsramMinDeselect << QMI_M1_TIMING_MIN_DESELECT_LSB) |
                        (kPsramRxDelay << QMI_M1_TIMING_RXDELAY_LSB) |
                        (kPsramDivider << QMI_M1_TIMING_CLKDIV_LSB);
    }

    const uint32_t interrupt_state = save_and_disable_interrupts();
    apply_qmi_timings(flash_timing, psram_timing);
    restore_interrupts(interrupt_state);

    const uint32_t live_flash_divider =
        (qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >>
        QMI_M0_TIMING_CLKDIV_LSB;
    const uint32_t live_psram_divider =
        (qmi_hw->m[1].timing & QMI_M1_TIMING_CLKDIV_BITS) >>
        QMI_M1_TIMING_CLKDIV_LSB;
    const uint32_t live_psram_rxdelay =
        (qmi_hw->m[1].timing & QMI_M1_TIMING_RXDELAY_BITS) >>
        QMI_M1_TIMING_RXDELAY_LSB;
    return psram_params_ok && live_flash_divider == kFlashDivider &&
           live_psram_divider == kPsramDivider &&
           live_psram_rxdelay == kPsramRxDelay;
}
#else
bool prepare_external_memory_for_target_clock()
{
    return true;
}
#endif

bool configure_clocks()
{
    vreg_disable_voltage_limit();

    vreg_set_voltage(kExperimentalOverclock ? VREG_VOLTAGE_1_50
                                            : VREG_VOLTAGE_1_20);
    sleep_ms(10);

    g_memory_timing_ready = prepare_external_memory_for_target_clock();
    if (!set_sys_clock_khz(JACK_WAVE1_SYS_CLOCK_KHZ, true)) return false;

#ifdef JACK_WAVE1_EXTERNAL_HSTX_CLOCK
    if (!jack_wave1_configure_hstx_clock()) return false;
#elif JACK_WAVE1_USE_PLL_USB_FOR_HSTX

    pll_deinit(pll_usb);
    pll_init(pll_usb, 1u, 756000000u, 6u, 1u);
    if (!clock_configure(clk_hstx, 0u,
                         CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                         kHstxHz, kHstxHz)) return false;
#else

    if (!clock_configure(clk_hstx, 0u,
                         CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                         kSystemHz, kHstxHz)) return false;
#endif
    if (!clock_configure(clk_peri, 0u,
                         CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                         kSystemHz, kPeriHz)) return false;
    return true;
}

bool ends_with_case_insensitive(const char *name, const char *suffix,
                                size_t suffix_length)
{
    const size_t name_length = strlen(name);
    if (name_length < suffix_length) return false;
    const char *tail = name + name_length - suffix_length;
    for (size_t i = 0; i < suffix_length; ++i) {
        if (tolower(static_cast<unsigned char>(tail[i])) !=
            tolower(static_cast<unsigned char>(suffix[i]))) return false;
    }
    return true;
}

bool matches_extensions(const char *name, const char *extensions)
{
    if (extensions == nullptr || *extensions == '\0') return true;
    const char *cursor = extensions;
    while (*cursor != '\0') {
        while (*cursor == ' ') ++cursor;
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ') ++cursor;
        const size_t length = static_cast<size_t>(cursor - start);
        if (length != 0u && ends_with_case_insensitive(name, start, length)) return true;
    }
    return false;
}

uint16_t normalize_pad(unsigned index)
{
    const uint16_t raw = nespad_states_ext[index];
    if (nespad_padtype[index] != NESPAD_TYPE_SNES) {
        return static_cast<uint16_t>(raw & 0x00ffu);
    }
    uint16_t result = raw & (JACK_BUTTON_SELECT | JACK_BUTTON_START |
                             JACK_BUTTON_UP | JACK_BUTTON_DOWN |
                             JACK_BUTTON_LEFT | JACK_BUTTON_RIGHT);
    if ((raw & (1u << 8)) != 0u) result |= JACK_BUTTON_A;
    if ((raw & (1u << 0)) != 0u) result |= JACK_BUTTON_B;
    if ((raw & (1u << 9)) != 0u) result |= JACK_BUTTON_X;
    if ((raw & (1u << 1)) != 0u) result |= JACK_BUTTON_Y;
    if ((raw & (1u << 10)) != 0u) result |= JACK_BUTTON_L;
    if ((raw & (1u << 11)) != 0u) result |= JACK_BUTTON_R;
    return result;
}

void draw_picker(const char *title, const char *directory, unsigned selection)
{
    jack_wave1_clear(kBlack);
    jack_wave1_draw_text(8, 8, title, kMint, kBlack);
    jack_wave1_draw_text(8, 24, directory, kWhite, kBlack);
    jack_wave1_draw_text(8, 224, "ARROWS:SELECT X/S/ENTER:OPEN", kWhite, kBlack);
    const unsigned first = selection >= 20u ? selection - 19u : 0u;
    for (unsigned row = 0; row < 20u && first + row < g_file_count; ++row) {
        const unsigned index = first + row;
        const unsigned y = 48u + row * 8u;
        const bool selected = index == selection;
        if (selected) jack_wave1_fill_rect(4, y, 312, 8, kWhite);
        jack_wave1_draw_text(8, y, g_file_names[index],
                             selected ? kBlack : kWhite,
                             selected ? kWhite : kBlack);
    }
}
}

extern "C" uint16_t jack_wave1_rgb555(unsigned red, unsigned green, unsigned blue)
{
    return static_cast<uint16_t>(((red & 31u) << 10) |
                                 ((green & 31u) << 5) | (blue & 31u));
}

extern "C" bool jack_wave1_init(const char *application_name)
{
    if (!configure_clocks()) return false;
    stdio_init_all();
    sleep_ms(50);
    jack_wave1_clock_info_t clock_info{};
    jack_wave1_get_clock_info(&clock_info);
    printf("WAVE1_RUNTIME begin app=%s sys=%lu peri=%lu hstx=%lu "
           "flash=%lu/div%u psram=%lu/div%u/rx%u qmi_ok=%d\n",
           application_name == nullptr ? "unknown" : application_name,
           static_cast<unsigned long>(clock_info.sys_hz),
           static_cast<unsigned long>(clock_info.peri_hz),
           static_cast<unsigned long>(clock_info.hstx_hz),
           static_cast<unsigned long>(clock_info.flash_hz),
           clock_info.flash_divider,
           static_cast<unsigned long>(clock_info.psram_hz),
           clock_info.psram_divider, clock_info.psram_rxdelay,
           g_memory_timing_ready ? 1 : 0);
#ifdef JACK_WAVE1_HDMI_AUDIO
    hstx_init(false);
#else
    hstx_init();
#endif
    if (!jack_wave1_usb_init())
        printf("WAVE1_RUNTIME usb_host_unavailable\n");
    const uint32_t system_clock_khz = clock_get_hz(clk_sys) / 1000u;
    (void)nespad_begin(0, system_clock_khz, 6u, 8u, 7u, pio1);
    (void)nespad_begin(1, system_clock_khz, 9u, 11u, 10u, pio1);
    jack_wave1_clear(kBlack);
    jack_wave1_draw_text(8, 8,
                         application_name == nullptr ? "Wave 1" : application_name,
                         kMint, kBlack);
    printf("WAVE1_RUNTIME init_complete\n");
    return true;
}

extern "C" bool jack_wave1_mount_sd(void)
{
    static pico_fatfs_spi_config_t config = {
        SDCARD_SPI,
        CLK_SLOW_DEFAULT,
        CLK_FAST_DEFAULT_PIO,
        SDCARD_PIN_MISO,
        SDCARD_PIN_CS,
        SDCARD_PIN_SCK,
        SDCARD_PIN_MOSI,
        true,
    };

    const bool hardware_spi = pico_fatfs_set_config(&config);
    if (!hardware_spi) {
        const uint state_machine = pio_claim_unused_sm(SDCARD_PIO, true);
        pico_fatfs_config_spi_pio(SDCARD_PIO, state_machine);
        printf("WAVE1_RUNTIME sd_transport=pio sm=%u\n", state_machine);
    } else {
        printf("WAVE1_RUNTIME sd_transport=spi0 pins=%u/%u/%u/%u\n",
               SDCARD_PIN_MISO, SDCARD_PIN_CS,
               SDCARD_PIN_SCK, SDCARD_PIN_MOSI);
    }

    FRESULT result = FR_NOT_READY;
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        result = f_mount(&g_fatfs, "", 1);
        printf("WAVE1_RUNTIME sd_mount attempt=%u result=%d\n",
               attempt, static_cast<int>(result));
        if (result == FR_OK) {
            g_sd_mounted = true;
            return true;
        }
        (void)pico_fatfs_reboot_spi();
        sleep_ms(50);
    }
    g_sd_mounted = false;
    return false;
}

extern "C" uint16_t *jack_wave1_framebuffer(void)
{
    return reinterpret_cast<uint16_t *>(hstx_getframebuffer());
}

extern "C" void jack_wave1_wait_vsync(void) { hstx_waitForVSync(); }

extern "C" uint32_t jack_wave1_frame_counter(void)
{
    return hstx_getframecounter();
}

extern "C" bool jack_wave1_wait_frame_boundary(uint32_t starting_frame)
{
    if (hstx_getframecounter() != starting_frame) return false;
    while (hstx_getframecounter() == starting_frame) tight_loop_contents();
    return true;
}

extern "C" void jack_wave1_get_clock_info(jack_wave1_clock_info_t *info)
{
    if (info == nullptr) return;
    const uint32_t flash_divider =
        (qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >>
        QMI_M0_TIMING_CLKDIV_LSB;
    const uint32_t psram_divider =
        (qmi_hw->m[1].timing & QMI_M1_TIMING_CLKDIV_BITS) >>
        QMI_M1_TIMING_CLKDIV_LSB;
    info->sys_hz = clock_get_hz(clk_sys);
    info->peri_hz = clock_get_hz(clk_peri);
    info->hstx_hz = clock_get_hz(clk_hstx);
    info->flash_divider = static_cast<uint8_t>(flash_divider);
    info->psram_divider = static_cast<uint8_t>(psram_divider);
    info->psram_rxdelay = static_cast<uint8_t>(
        (qmi_hw->m[1].timing & QMI_M1_TIMING_RXDELAY_BITS) >>
        QMI_M1_TIMING_RXDELAY_LSB);
    info->flash_hz = flash_divider == 0u ? 0u : info->sys_hz / flash_divider;
    info->psram_hz = psram_divider == 0u ? 0u : info->sys_hz / psram_divider;
}

extern "C" bool jack_wave1_memory_timing_ready(void)
{
    return g_memory_timing_ready;
}

extern "C" void jack_wave1_clear(uint16_t color)
{
    jack_wave1_fill_rect(0, 0, JACK_WAVE1_SCREEN_WIDTH,
                         JACK_WAVE1_SCREEN_HEIGHT, color);
}

extern "C" void jack_wave1_fill_rect(unsigned x, unsigned y,
                                      unsigned width, unsigned height,
                                      uint16_t color)
{
    uint16_t *framebuffer = jack_wave1_framebuffer();
    if (framebuffer == nullptr) return;
    const unsigned right = x + width > JACK_WAVE1_SCREEN_WIDTH
                               ? JACK_WAVE1_SCREEN_WIDTH : x + width;
    const unsigned bottom = y + height > JACK_WAVE1_SCREEN_HEIGHT
                                ? JACK_WAVE1_SCREEN_HEIGHT : y + height;
    for (unsigned py = y; py < bottom; ++py) {
        uint16_t *row = framebuffer + py * JACK_WAVE1_SCREEN_WIDTH;
        for (unsigned px = x; px < right; ++px) row[px] = color;
    }
}

extern "C" void jack_wave1_draw_char(unsigned x, unsigned y, char character,
                                      uint16_t foreground, uint16_t background)
{
    if (character < 32 || character > 126) character = '?';
    uint16_t *framebuffer = jack_wave1_framebuffer();
    if (framebuffer == nullptr) return;
    const unsigned glyph = static_cast<unsigned>(character - 32);
    for (unsigned row = 0; row < 8u; ++row) {
        const uint8_t bits = static_cast<uint8_t>(font_8x8[row * 95u + glyph]);
        for (unsigned column = 0; column < 8u; ++column) {
            if (x + column < JACK_WAVE1_SCREEN_WIDTH &&
                y + row < JACK_WAVE1_SCREEN_HEIGHT) {
                framebuffer[(y + row) * JACK_WAVE1_SCREEN_WIDTH + x + column] =
                    (bits & (1u << column)) != 0u ? foreground : background;
            }
        }
    }
}

extern "C" void jack_wave1_draw_text(unsigned x, unsigned y, const char *text,
                                      uint16_t foreground, uint16_t background)
{
    if (text == nullptr) return;
    unsigned cursor_x = x;
    while (*text != '\0' && cursor_x + 8u <= JACK_WAVE1_SCREEN_WIDTH) {
        jack_wave1_draw_char(cursor_x, y, *text++, foreground, background);
        cursor_x += 8u;
    }
}

extern "C" void jack_wave1_show_error(const char *title, const char *detail)
{
    jack_wave1_clear(kDark);
    jack_wave1_draw_text(8, 16, title == nullptr ? "ERROR" : title, kMint, kDark);
    jack_wave1_draw_text(8, 40, detail == nullptr ? "Unknown error" : detail,
                         kWhite, kDark);
    jack_wave1_draw_text(8, 216, "Check SD paths and UART log", kWhite, kDark);
}

extern "C" void jack_wave1_poll_player_buttons(uint16_t players[2])
{
    if (players == nullptr) return;
    const uint16_t usb_buttons = jack_wave1_usb_poll_buttons();
    nespad_read_start();
    sleep_us(220);
    nespad_read_finish();
    players[0] = static_cast<uint16_t>(usb_buttons | normalize_pad(0));
    players[1] = normalize_pad(1);
}

extern "C" uint16_t jack_wave1_poll_buttons(void)
{
    uint16_t players[2];
    jack_wave1_poll_player_buttons(players);
    return static_cast<uint16_t>(players[0] | players[1]);
}
extern "C" void jack_wave1_wait_buttons_released(void)
{
    while (jack_wave1_poll_buttons() != 0u) sleep_ms(8);
}

extern "C" void jack_wave1_set_picker_auto_select_single(bool enabled)
{
    g_picker_auto_select_single = enabled;
}

extern "C" bool jack_wave1_select_file(const char *title, const char *directory,
                                        const char *extensions,
                                        char *selected_path,
                                        size_t selected_path_size)
{
    if (!g_sd_mounted || title == nullptr || directory == nullptr ||
        selected_path == nullptr || selected_path_size == 0u) return false;

    DIR directory_handle;
    FILINFO info;
    FRESULT result = f_opendir(&directory_handle, directory);
    if (result != FR_OK) {
        printf("WAVE1_RUNTIME opendir_failed path=%s result=%d\n",
               directory, static_cast<int>(result));
        return false;
    }
    g_file_count = 0;
    while (g_file_count < kMaximumFiles) {
        result = f_readdir(&directory_handle, &info);
        if (result != FR_OK || info.fname[0] == '\0') break;
        if ((info.fattrib & AM_DIR) == 0u && matches_extensions(info.fname, extensions)) {
            strncpy(g_file_names[g_file_count], info.fname, kFileNameBytes - 1u);
            g_file_names[g_file_count][kFileNameBytes - 1u] = '\0';
            ++g_file_count;
        }
    }
    f_closedir(&directory_handle);
    if (result != FR_OK || g_file_count == 0u) {
        printf("WAVE1_RUNTIME no_content path=%s result=%d\n",
               directory, static_cast<int>(result));
        return false;
    }
    if (g_picker_auto_select_single && g_file_count == 1u) {
        const int written = snprintf(selected_path, selected_path_size,
                                     "%s/%s", directory, g_file_names[0]);
        if (written < 0 || static_cast<size_t>(written) >= selected_path_size)
            return false;
        printf("WAVE1_RUNTIME auto_selected_single=%s\n", selected_path);
        return true;
    }

    unsigned selection = 0;
    uint16_t previous = 0;
    uint32_t last_input_log = time_us_32();
    for (;;) {
        jack_wave1_usb_service();
        draw_picker(title, directory, selection);
        jack_wave1_wait_vsync();
        jack_wave1_usb_service();
        jack_wave1_usb_status_t usb_status{};
        jack_wave1_usb_get_status(&usb_status);
        const uint16_t buttons = jack_wave1_poll_buttons();
        const uint16_t pressed = static_cast<uint16_t>(buttons & ~previous);
        previous = buttons;
        const uint32_t now = time_us_32();
        if (pressed != 0u || static_cast<uint32_t>(now - last_input_log) >= 1000000u) {
            const uint32_t report_age = usb_status.last_report_us == 0u ? UINT32_MAX :
                static_cast<uint32_t>(now - usb_status.last_report_us);
            printf("WAVE1_INPUT context=picker initialized=%d keyboards=%u "
                   "reports=%lu services=%lu failures=%lu report_age_us=%lu "
                   "current=%04x latched=%04x buttons=%04x pressed=%04x selection=%u\n",
                   usb_status.initialized ? 1 : 0, usb_status.keyboard_count,
                   static_cast<unsigned long>(usb_status.report_count),
                   static_cast<unsigned long>(usb_status.service_count),
                   static_cast<unsigned long>(usb_status.receive_failure_count),
                   static_cast<unsigned long>(report_age), usb_status.current_buttons,
                   usb_status.latched_buttons, buttons, pressed, selection);
            last_input_log = now;
        }
        if ((pressed & JACK_BUTTON_UP) != 0u)
            selection = selection == 0u ? g_file_count - 1u : selection - 1u;
        if ((pressed & JACK_BUTTON_DOWN) != 0u)
            selection = (selection + 1u) % g_file_count;
        if ((pressed & (JACK_BUTTON_A | JACK_BUTTON_START)) != 0u) {
            const int written = snprintf(selected_path, selected_path_size,
                                         "%s/%s", directory, g_file_names[selection]);
            if (written < 0 || static_cast<size_t>(written) >= selected_path_size)
                return false;
            printf("WAVE1_RUNTIME selected=%s\n", selected_path);
            jack_wave1_wait_buttons_released();
            return true;
        }
        sleep_ms(12);
    }
}
