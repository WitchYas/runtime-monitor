#ifndef MONITOR_EVENT_H
#define MONITOR_EVENT_H

#include <stddef.h>
#include <stdint.h>


typedef enum {

    MONITOR_EVENT_INVALID = 0,

    MONITOR_EVENT_LOCK_REQUEST,
    MONITOR_EVENT_LOCK_ACQUIRED,
    MONITOR_EVENT_LOCK_FAILED,

    MONITOR_EVENT_UNLOCK_REQUEST,
    MONITOR_EVENT_UNLOCKED,
    MONITOR_EVENT_UNLOCK_FAILED,

    MONITOR_EVENT_THREAD_START,
    MONITOR_EVENT_THREAD_EXIT

} monitor_event_type_t;


typedef struct {

    uint64_t sequence_id;

    uint64_t timestamp_ns;

    uint64_t thread_id;

    uintptr_t object_address;

    uint32_t event_type;

    int32_t result;

    uint32_t flags;

    uint32_t thread_sequence;

} monitor_event_t;


_Static_assert(
    sizeof(monitor_event_t) <= 64,
    "monitor_event_t must remain compact"
);


#endif