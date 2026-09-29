
#ifndef BYTEPUSHER_LOG15_H
#define BYTEPUSHER_LOG15_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void bytepusher_log15_start(void);
void bytepusher_log15_pump(void);
uint32_t bytepusher_log15_dropped(void);
int bytepusher_log15_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
#ifdef __cplusplus
}
#endif
#endif
