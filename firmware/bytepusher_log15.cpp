
#include "bytepusher_log15.h"
#include <stdarg.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"

namespace {
constexpr uint32_t capacity = 2048u;
char queue[capacity];
uint32_t head, tail, dropped;
bool active;
}

extern "C" void bytepusher_log15_start(void)
{
    head = tail = dropped = 0;
    active = true;
}

extern "C" uint32_t bytepusher_log15_dropped(void) { return dropped; }

extern "C" int bytepusher_log15_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (!active) {
        const int n = vprintf(format, args);
        va_end(args);
        return n;
    }
    char line[768];
    const int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n < 0 || static_cast<unsigned>(n) >= sizeof(line) ||
        static_cast<unsigned>(n) > capacity - (head - tail)) {
        ++dropped; // Drop a whole record when the UART queue fills.
        return -1;
    }
    for (int i = 0; i < n; ++i) queue[head++ % capacity] = line[i];
    return n;
}

extern "C" void bytepusher_log15_pump(void)
{
    if (!active) return;

    for (unsigned i = 0; i < 32u && tail != head; ++i) {
        if (!uart_is_writable(uart_default)) break;
        uart_get_hw(uart_default)->dr = static_cast<uint8_t>(queue[tail++ % capacity]);
    }
}
