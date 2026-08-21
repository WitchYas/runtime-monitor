#ifndef MONITOR_RUNTIME_H
#define MONITOR_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "monitor_event.h"

#define MONITOR_MAX_THREADS 16u


int monitor_runtime_start(void);


/*
 * Stop accepting events.
 *
 * The consumer drains all remaining queues before terminating.
 * Transport statistics are printed after shutdown.
 */
void monitor_runtime_stop(void);


/*
 * Emit one event from the current application thread.
 *
 * Each registered application thread writes exclusively
 * to its own SPSC queue.
 *
 * This function never waits for queue capacity.
 *
 * If the queue is full:
 *     - the event is dropped;
 *     - the dropped-event counter is incremented;
 *     - application execution continues.
 */
void monitor_runtime_emit(
    monitor_event_type_t type,
    uintptr_t object_address,
    int32_t result
);


/*
 * Returns true only for threads that belong to the monitor itself.
 *
 * This prevents monitor-internal synchronization activity from
 * being interpreted as target-application activity.
 */
bool monitor_runtime_is_internal_thread(void);


#endif
