
#ifndef JACK_WAVE1_RUNTIME_H
#define JACK_WAVE1_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JACK_WAVE1_SCREEN_WIDTH 320u
#define JACK_WAVE1_SCREEN_HEIGHT 240u
#define JACK_WAVE1_PATH_MAX 256u

typedef struct jack_wave1_clock_info {
    uint32_t sys_hz;
    uint32_t peri_hz;
    uint32_t hstx_hz;
    uint32_t flash_hz;
    uint32_t psram_hz;
    uint8_t flash_divider;
    uint8_t psram_divider;
    uint8_t psram_rxdelay;
} jack_wave1_clock_info_t;

typedef struct jack_wave1_usb_status {
    uint32_t report_count;
    uint32_t service_count;
    uint32_t receive_failure_count;
    uint32_t last_report_us;
    uint16_t current_buttons;
    uint16_t latched_buttons;
    uint8_t keyboard_count;
    bool initialized;
} jack_wave1_usb_status_t;

enum {
    JACK_BUTTON_A = 1u << 0,
    JACK_BUTTON_B = 1u << 1,
    JACK_BUTTON_SELECT = 1u << 2,
    JACK_BUTTON_START = 1u << 3,
    JACK_BUTTON_UP = 1u << 4,
    JACK_BUTTON_DOWN = 1u << 5,
    JACK_BUTTON_LEFT = 1u << 6,
    JACK_BUTTON_RIGHT = 1u << 7,
    JACK_BUTTON_X = 1u << 8,
    JACK_BUTTON_Y = 1u << 9,
    JACK_BUTTON_L = 1u << 10,
    JACK_BUTTON_R = 1u << 11,
    JACK_KEYBOARD_A = 1u << 12,
    JACK_KEYBOARD_B = 1u << 13,
    JACK_KEYBOARD_X = 1u << 14,
    JACK_KEYBOARD_MUTE = 1u << 15,
};

bool jack_wave1_init(const char *application_name);

bool jack_wave1_mount_sd(void);

uint16_t *jack_wave1_framebuffer(void);
void jack_wave1_wait_vsync(void);

uint32_t jack_wave1_frame_counter(void);
bool jack_wave1_wait_frame_boundary(uint32_t starting_frame);

void jack_wave1_get_clock_info(jack_wave1_clock_info_t *info);
bool jack_wave1_memory_timing_ready(void);

void jack_wave1_clear(uint16_t color);
void jack_wave1_fill_rect(unsigned int x, unsigned int y,
                          unsigned int width, unsigned int height,
                          uint16_t color);
void jack_wave1_draw_char(unsigned int x, unsigned int y, char character,
                          uint16_t foreground, uint16_t background);
void jack_wave1_draw_text(unsigned int x, unsigned int y, const char *text,
                          uint16_t foreground, uint16_t background);
void jack_wave1_show_error(const char *title, const char *detail);

uint16_t jack_wave1_poll_buttons(void);

void jack_wave1_poll_player_buttons(uint16_t players[2]);

uint16_t jack_wave1_usb_poll_buttons(void);

void jack_wave1_usb_service(void);

void jack_wave1_usb_get_status(jack_wave1_usb_status_t *status);

uint32_t jack_wave1_usb_report_count(void);

void jack_wave1_wait_buttons_released(void);

void jack_wave1_set_picker_auto_select_single(bool enabled);

bool jack_wave1_select_file(const char *title, const char *directory,
                            const char *extensions, char *selected_path,
                            size_t selected_path_size);

uint16_t jack_wave1_rgb555(unsigned int red, unsigned int green,
                           unsigned int blue);

#ifdef __cplusplus
}
#endif

#endif
