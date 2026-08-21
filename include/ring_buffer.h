#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdbool.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>

#include "monitor_event.h"
#define SPSC_RING_CAPACITY 2048u

#define SPSC_RING_MASK \
    (SPSC_RING_CAPACITY - 1u)


_Static_assert(
    (SPSC_RING_CAPACITY &
     (SPSC_RING_CAPACITY - 1u)) == 0,
    "SPSC_RING_CAPACITY must be a power of two"
);


/*
 * ==========================================================================
 * Single-Producer / Single-Consumer ring buffer
 * ==========================================================================
 *
 * Every queue has exactly:
 *
 *     one application producer
 *     one monitor consumer
 *
 * head:
 *     written only by producer
 *
 * tail:
 *     written only by consumer
 *
 * head and tail are placed on separate cache-line boundaries
 * to reduce producer/consumer false sharing.
 */

typedef struct {

    alignas(64)
    _Atomic uint64_t head;


    alignas(64)
    _Atomic uint64_t tail;


    monitor_event_t slots[SPSC_RING_CAPACITY];

} spsc_ring_buffer_t;


/*
 * ==========================================================================
 * Queue initialization
 * ==========================================================================
 */

void spsc_ring_buffer_init(
    spsc_ring_buffer_t *buffer
);



bool spsc_ring_buffer_try_push(
    spsc_ring_buffer_t *buffer,
    const monitor_event_t *event
);




bool spsc_ring_buffer_try_pop(
    spsc_ring_buffer_t *buffer,
    monitor_event_t *event
);


#endif