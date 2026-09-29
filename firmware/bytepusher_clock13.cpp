
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/resets.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/pll.h"
#include "tusb_config.h"
#include <cstdio>

static_assert(PICO_RP2350 && XOSC_HZ == 12000000, "RP2350 + 12MHz XOSC only");
// PLL_USB is safe to retune only with PIO-only USB.
static_assert(CFG_TUH_RPI_PIO_USB && BOARD_TUH_RHPORT == 1 && !CFG_TUD_ENABLED,
              "PLL_USB reuse requires PIO-only USB");
#if LIB_PICO_STDIO_USB
#error "hardware USB stdio must stay disabled"
#endif
static_assert(CLOCKS_CLK_HSTX_DIV_BITS == (3u << 16), "integer HSTX divider");

namespace {
constexpr uint32_t kSystemHz = 480000000u, kHstxHz = 126000000u;
uint32_t measured_sys_khz, measured_hstx_khz;

uint32_t clock_status = 1;

bool count_khz(uint32_t source, uint32_t &out) {
    fc_hw_t *fc = &clocks_hw->fc0;
    uint32_t start = time_us_32();
    while (fc->status & CLOCKS_FC0_STATUS_RUNNING_BITS)
        if (uint32_t(time_us_32() - start) >= 10000u) return false;
    fc->ref_khz = clock_get_hz(clk_ref) / 1000u;
    fc->interval = 10;
    fc->min_khz = 0;
    fc->max_khz = 0xffffffffu;
    fc->src = source;
    start = time_us_32();
    while (!(fc->status & CLOCKS_FC0_STATUS_DONE_BITS))
        if (uint32_t(time_us_32() - start) >= 10000u) return false;
    if (fc->status & CLOCKS_FC0_STATUS_DIED_BITS) return false;
    out = fc->result >> CLOCKS_FC0_RESULT_KHZ_LSB;
    return true;
}
}

extern "C" bool jack_wave1_configure_hstx_clock(void) {

    const uint32_t sys = clocks_hw->clk[clk_sys].ctrl;
    if (clock_get_hz(clk_sys) != kSystemHz || clock_get_hz(clk_ref) != XOSC_HZ ||
        (sys & CLOCKS_CLK_SYS_CTRL_SRC_BITS) != CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX ||
        ((sys & CLOCKS_CLK_SYS_CTRL_AUXSRC_BITS) >> CLOCKS_CLK_SYS_CTRL_AUXSRC_LSB) !=
            CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS ||
        clocks_hw->clk[clk_sys].selected != 2u ||
        (clocks_hw->clk[clk_ref].ctrl & CLOCKS_CLK_REF_CTRL_SRC_BITS) !=
            CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC)
        return false;

    clock_configure_undivided(clk_peri, 0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, kSystemHz);
    clock_status = 2;
    for (unsigned i = clk_gpout0; i <= clk_gpout3; ++i) {
        uint32_t ctrl = clocks_hw->clk[i].ctrl;
        uint32_t source = (ctrl & CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_BITS) >> CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_LSB;
        if ((ctrl & CLOCKS_CLK_GPOUT0_CTRL_ENABLE_BITS) &&
            (source == CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB ||
             source == CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB_PRIMARY_REF_OPCG))
            return false;
    }
    clock_stop(clk_hstx);

    reset_block(RESETS_RESET_USBCTRL_BITS | RESETS_RESET_ADC_BITS);
    clock_stop(clk_usb);
    clock_stop(clk_adc);

    pll_init(pll_usb, 1u, 756000000u, 6u, 1u);
    clock_configure_undivided(clk_hstx, 0,
        CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, kHstxHz);
    clock_status = 5;
    if (clocks_hw->clk[clk_hstx].div != (1u << CLOCKS_CLK_HSTX_DIV_INT_LSB) ||
        ((clocks_hw->clk[clk_hstx].ctrl & CLOCKS_CLK_HSTX_CTRL_AUXSRC_BITS) >>
            CLOCKS_CLK_HSTX_CTRL_AUXSRC_LSB) != CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB)
        return false;
    clock_status = 3;
    if (!count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS, measured_sys_khz) ||
        !count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX, measured_hstx_khz)) return false;
    clock_status = 4;

    if (measured_sys_khz < 475200u || measured_sys_khz > 484800u ||
        measured_hstx_khz < 124740u || measured_hstx_khz > 127260u) return false;
    clock_status = 0;
    return true;
}

extern "C" void bytepusher_clock13_report(void) {

    printf("BYTEPUSHER_CLOCK13 status=%lu fc_sys_khz=%lu fc_hstx_khz=%lu measured=startup "
           "hstx_ctrl=%08lx hstx_div=%08lx pll_usb_fb=%lu pll_usb_prim=%08lx "
           "usb_hz=%lu adc_hz=%lu peri_hz=%lu\n",
           (unsigned long)clock_status, (unsigned long)measured_sys_khz,
           (unsigned long)measured_hstx_khz,
           (unsigned long)clocks_hw->clk[clk_hstx].ctrl,
           (unsigned long)clocks_hw->clk[clk_hstx].div,
           (unsigned long)pll_usb->fbdiv_int, (unsigned long)pll_usb->prim,
           (unsigned long)clock_get_hz(clk_usb), (unsigned long)clock_get_hz(clk_adc),
           (unsigned long)clock_get_hz(clk_peri));
}
