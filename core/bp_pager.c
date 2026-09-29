
#include "bp_pager.h"

#include <limits.h>
#include <string.h>

static bool flush_slot(bp_pager_t *pager, uint16_t slot_index)
{
    bp_slot_t *slot = &pager->slots[slot_index];
    if (!slot->valid || !slot->dirty) {
        return true;
    }
    if (pager->backing.write_page == NULL ||
        !pager->backing.write_page(pager->backing.context,
                                   slot->virtual_page,
                                   pager->cache + (size_t)slot_index * BP_PAGE_BYTES)) {
        return false;
    }
    slot->dirty = false;
    ++pager->stats.dirty_writes;
    return true;
}

static int16_t choose_slot(const bp_pager_t *pager)
{
    int16_t oldest_slot = BP_INVALID_SLOT;
    uint32_t oldest_use = UINT32_MAX;

    for (uint16_t i = 0; i < pager->slot_count; ++i) {
        const bp_slot_t *slot = &pager->slots[i];
        if (!slot->valid) {
            return (int16_t)i;
        }
        if (!slot->pinned && slot->last_use < oldest_use) {
            oldest_use = slot->last_use;
            oldest_slot = (int16_t)i;
        }
    }
    return oldest_slot;
}

static bool resolve_page(bp_pager_t *pager, uint16_t virtual_page, int16_t *resolved_slot)
{
    int16_t slot_index = pager->page_to_slot[virtual_page];
    ++pager->use_clock;

    if (slot_index != BP_INVALID_SLOT) {
        pager->slots[slot_index].last_use = pager->use_clock;
        ++pager->stats.page_hits;
        *resolved_slot = slot_index;
        return true;
    }

    ++pager->stats.page_faults;
    slot_index = choose_slot(pager);
    if (slot_index == BP_INVALID_SLOT) {
        return false;
    }

    bp_slot_t *slot = &pager->slots[slot_index];
    if (slot->valid) {
        if (!flush_slot(pager, (uint16_t)slot_index)) {
            return false;
        }
        pager->page_to_slot[slot->virtual_page] = BP_INVALID_SLOT;
        ++pager->stats.evictions;
    }

    uint8_t *page_data = pager->cache + (size_t)slot_index * BP_PAGE_BYTES;
    if (!pager->backing.read_page(pager->backing.context, virtual_page, page_data)) {
        slot->valid = false;
        slot->virtual_page = BP_INVALID_PAGE;
        return false;
    }

    slot->valid = true;
    slot->dirty = false;
    slot->pinned = false;
    slot->virtual_page = virtual_page;
    slot->last_use = pager->use_clock;
    pager->page_to_slot[virtual_page] = slot_index;
    ++pager->stats.page_reads;
    *resolved_slot = slot_index;
    return true;
}

bool bp_pager_init(bp_pager_t *pager,
                   uint8_t *cache,
                   bp_slot_t *slots,
                   uint16_t slot_count,
                   int16_t page_to_slot[BP_PAGE_COUNT],
                   bp_backing_store_t backing)
{
    if (pager == NULL || cache == NULL || slots == NULL || page_to_slot == NULL ||
        slot_count == 0 || slot_count > INT16_MAX || backing.read_page == NULL) {
        return false;
    }

    memset(pager, 0, sizeof(*pager));
    pager->cache = cache;
    pager->slots = slots;
    pager->slot_count = slot_count;
    pager->page_to_slot = page_to_slot;
    pager->backing = backing;

    for (uint16_t i = 0; i < slot_count; ++i) {
        slots[i].last_use = 0;
        slots[i].virtual_page = BP_INVALID_PAGE;
        slots[i].valid = false;
        slots[i].dirty = false;
        slots[i].pinned = false;
    }
    for (uint16_t page = 0; page < BP_PAGE_COUNT; ++page) {
        page_to_slot[page] = BP_INVALID_SLOT;
    }
    return true;
}

static bool map_span(bp_pager_t *pager, uint32_t address, uint32_t bytes,
                     int16_t *resolved_slot, uint16_t *offset)
{
    if (pager == NULL || resolved_slot == NULL || offset == NULL || bytes == 0u) {
        return false;
    }
    address &= (BP_VIRTUAL_BYTES - 1u);
    *offset = (uint16_t)(address & (BP_PAGE_BYTES - 1u));
    if (bytes > BP_PAGE_BYTES - *offset) {
        return false;
    }
    return resolve_page(pager, (uint16_t)(address >> BP_PAGE_SHIFT), resolved_slot);
}

bool bp_pager_map_read(bp_pager_t *pager, uint32_t address, uint32_t bytes,
                       const uint8_t **data)
{
    if (data == NULL) {
        return false;
    }
    int16_t slot;
    uint16_t offset;
    if (!map_span(pager, address, bytes, &slot, &offset)) {
        return false;
    }
    *data = pager->cache + (size_t)slot * BP_PAGE_BYTES + offset;
    return true;
}

bool bp_pager_map_write(bp_pager_t *pager, uint32_t address, uint32_t bytes,
                        uint8_t **data)
{
    if (data == NULL) {
        return false;
    }
    int16_t slot;
    uint16_t offset;
    if (!map_span(pager, address, bytes, &slot, &offset)) {
        return false;
    }
    pager->slots[slot].dirty = true;
    *data = pager->cache + (size_t)slot * BP_PAGE_BYTES + offset;
    return true;
}

bool bp_pager_read8(bp_pager_t *pager, uint32_t address, uint8_t *value)
{
    const uint8_t *data;
    if (value == NULL || !bp_pager_map_read(pager, address, 1u, &data)) {
        return false;
    }
    *value = *data;
    return true;
}

bool bp_pager_write8(bp_pager_t *pager, uint32_t address, uint8_t value)
{
    uint8_t *data;
    if (!bp_pager_map_write(pager, address, 1u, &data)) {
        return false;
    }
    *data = value;
    return true;
}

bool bp_pager_read24(bp_pager_t *pager, uint32_t address, uint32_t *value)
{
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
    if (value == NULL || !bp_pager_read8(pager, address, &b0) ||
        !bp_pager_read8(pager, address + 1u, &b1) ||
        !bp_pager_read8(pager, address + 2u, &b2)) {
        return false;
    }
    *value = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
    return true;
}

bool bp_pager_pin_page(bp_pager_t *pager, uint16_t virtual_page, bool pinned)
{
    if (pager == NULL || virtual_page >= BP_PAGE_COUNT) {
        return false;
    }
    int16_t slot;
    if (!resolve_page(pager, virtual_page, &slot)) {
        return false;
    }
    pager->slots[slot].pinned = pinned;
    return true;
}

bool bp_pager_flush_next(bp_pager_t *pager, uint16_t *cursor, bool *wrote_page)
{
    if (pager == NULL || cursor == NULL || wrote_page == NULL ||
        pager->slot_count == 0u) {
        return false;
    }
    if (*cursor >= pager->slot_count) *cursor = 0u;
    *wrote_page = false;
    for (uint16_t scanned = 0; scanned < pager->slot_count; ++scanned) {
        const uint16_t slot_index = *cursor;
        *cursor = (uint16_t)(slot_index + 1u);
        if (*cursor == pager->slot_count) *cursor = 0u;
        if (!pager->slots[slot_index].valid || !pager->slots[slot_index].dirty) {
            continue;
        }
        if (!flush_slot(pager, slot_index)) return false;
        *wrote_page = true;
        return true;
    }
    return true;
}

bool bp_pager_flush(bp_pager_t *pager)
{
    if (pager == NULL) {
        return false;
    }
    for (uint16_t i = 0; i < pager->slot_count; ++i) {
        if (!flush_slot(pager, i)) {
            return false;
        }
    }
    return true;
}
