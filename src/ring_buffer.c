#include "ring_buffer.h"


/*
 * ==========================================================================
 * Initialization
 * ==========================================================================
 */

void spsc_ring_buffer_init(
    spsc_ring_buffer_t *buffer
)
{
    atomic_init(
        &buffer->head,
        0
    );


    atomic_init(
        &buffer->tail,
        0
    );
}


/*
 * ==========================================================================
 * Producer
 * ==========================================================================
 */

bool spsc_ring_buffer_try_push(
    spsc_ring_buffer_t *buffer,
    const monitor_event_t *event
)
{
    /*
     * Only the producer modifies head.
     */
    const uint64_t head =
        atomic_load_explicit(
            &buffer->head,
            memory_order_relaxed
        );


    /*
     * tail is owned by consumer.
     *
     * Acquire allows producer to observe released consumer
     * progress before reusing a slot.
     */
    const uint64_t tail =
        atomic_load_explicit(
            &buffer->tail,
            memory_order_acquire
        );


    /*
     * Queue full.
     */
    if ((head - tail) >= SPSC_RING_CAPACITY) {
        return false;
    }


    /*
     * Fill slot completely before publishing new head.
     */
    buffer->slots[
        head & SPSC_RING_MASK
    ] = *event;


    /*
     * Publish event availability.
     */
    atomic_store_explicit(
        &buffer->head,
        head + 1u,
        memory_order_release
    );


    return true;
}


/*
 * ==========================================================================
 * Consumer
 * ==========================================================================
 */

bool spsc_ring_buffer_try_pop(
    spsc_ring_buffer_t *buffer,
    monitor_event_t *event
)
{
    /*
     * Only consumer modifies tail.
     */
    const uint64_t tail =
        atomic_load_explicit(
            &buffer->tail,
            memory_order_relaxed
        );


    /*
     * head is owned by producer.
     *
     * Acquire pairs with producer's release publication.
     */
    const uint64_t head =
        atomic_load_explicit(
            &buffer->head,
            memory_order_acquire
        );


    /*
     * Queue empty.
     */
    if (tail == head) {
        return false;
    }


    /*
     * Read event before releasing the slot.
     */
    *event =
        buffer->slots[
            tail & SPSC_RING_MASK
        ];


    /*
     * Publish consumer progress.
     */
    atomic_store_explicit(
        &buffer->tail,
        tail + 1u,
        memory_order_release
    );


    return true;
}