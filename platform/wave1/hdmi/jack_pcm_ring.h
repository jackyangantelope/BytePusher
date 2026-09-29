
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#define JACK_PCM_CAPACITY 4096u
typedef struct {
    uint32_t samples[JACK_PCM_CAPACITY];
    _Atomic uint32_t head, tail;
} jack_pcm_ring_t;

static inline uint32_t jack_pcm_level(jack_pcm_ring_t *q)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t used = head - tail;
    return used > JACK_PCM_CAPACITY ? JACK_PCM_CAPACITY : used;
}
static inline bool jack_pcm_push(jack_pcm_ring_t *q, uint32_t value)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head - tail >= JACK_PCM_CAPACITY) return false;
    q->samples[head & (JACK_PCM_CAPACITY - 1)] = value;
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return true;
}
static inline bool jack_pcm_pop(jack_pcm_ring_t *q, uint32_t *value)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail == head) return false;
    *value = q->samples[tail & (JACK_PCM_CAPACITY - 1)];
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return true;
}
