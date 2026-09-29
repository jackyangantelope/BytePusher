
#ifndef JACK_BYTEPUSHER_PAGER_H
#define JACK_BYTEPUSHER_PAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BP_ADDRESS_BITS 24u
#define BP_VIRTUAL_BYTES (1u << BP_ADDRESS_BITS)
#define BP_PAGE_SHIFT 12u
#define BP_PAGE_BYTES (1u << BP_PAGE_SHIFT)
#define BP_PAGE_COUNT (BP_VIRTUAL_BYTES / BP_PAGE_BYTES)
#define BP_INVALID_PAGE 0xffffu
#define BP_INVALID_SLOT (-1)

typedef bool (*bp_read_page_fn)(void *context, uint16_t page, uint8_t *destination);
typedef bool (*bp_write_page_fn)(void *context, uint16_t page, const uint8_t *source);

typedef struct bp_backing_store {
    void *context;
    bp_read_page_fn read_page;
    bp_write_page_fn write_page;
} bp_backing_store_t;

typedef struct bp_slot {
    uint32_t last_use;
    uint16_t virtual_page;
    bool valid;
    bool dirty;
    bool pinned;
} bp_slot_t;

typedef struct bp_pager_stats {
    uint32_t page_hits;
    uint32_t page_faults;
    uint32_t page_reads;
    uint32_t dirty_writes;
    uint32_t evictions;
} bp_pager_stats_t;

typedef struct bp_pager {
    uint8_t *cache;
    bp_slot_t *slots;
    int16_t *page_to_slot;
    uint16_t slot_count;
    uint32_t use_clock;
    bp_backing_store_t backing;
    bp_pager_stats_t stats;
} bp_pager_t;

bool bp_pager_init(bp_pager_t *pager,
                   uint8_t *cache,
                   bp_slot_t *slots,
                   uint16_t slot_count,
                   int16_t page_to_slot[BP_PAGE_COUNT],
                   bp_backing_store_t backing);

// A mapping can be evicted by the next page fault.
bool bp_pager_map_read(bp_pager_t *pager, uint32_t address, uint32_t bytes,
                       const uint8_t **data);
bool bp_pager_map_write(bp_pager_t *pager, uint32_t address, uint32_t bytes,
                        uint8_t **data);
bool bp_pager_read8(bp_pager_t *pager, uint32_t address, uint8_t *value);
bool bp_pager_write8(bp_pager_t *pager, uint32_t address, uint8_t value);
bool bp_pager_read24(bp_pager_t *pager, uint32_t address, uint32_t *value);
bool bp_pager_pin_page(bp_pager_t *pager, uint16_t virtual_page, bool pinned);

bool bp_pager_flush_next(bp_pager_t *pager, uint16_t *cursor, bool *wrote_page);
bool bp_pager_flush(bp_pager_t *pager);

#ifdef __cplusplus
}
#endif

#endif
