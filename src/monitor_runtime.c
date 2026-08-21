#define _GNU_SOURCE

#include "monitor_runtime.h"
#include "monitor_verifier.h"
#include "ring_buffer.h"

#include <pthread.h>
#include <sched.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>


#define MONITOR_DRAIN_BATCH 64u


/*
 * ==========================================================================
 * Producer statistics/state
 * ==========================================================================
 */

typedef struct {

    _Atomic uint64_t generated;

    _Atomic uint64_t dropped;


    uint32_t next_thread_sequence;

} producer_counters_t;


/*
 * ==========================================================================
 * Consumer statistics
 * ==========================================================================
 */

typedef struct {

    _Atomic uint64_t consumed;

} consumer_counters_t;


/*
 * ==========================================================================
 * Per-thread transport context
 * ==========================================================================
 */

typedef struct {

    _Atomic bool active;

    uint64_t thread_id;

    spsc_ring_buffer_t queue;


    alignas(64)
    producer_counters_t producer;


    alignas(64)
    consumer_counters_t consumer;

} monitor_thread_context_t;


_Static_assert(
    _Alignof(monitor_thread_context_t) >= 64,
    "monitor_thread_context_t must remain cache-line aligned"
);


/*
 * ==========================================================================
 * Runtime state
 * ==========================================================================
 */

static monitor_thread_context_t
    thread_contexts[MONITOR_MAX_THREADS];


static _Atomic uint32_t
    next_thread_slot = 0;


static _Atomic uint64_t
    global_sequence = 0;


static _Atomic uint64_t
    unregistered_events = 0;


static _Atomic uint64_t
    lifecycle_registration_failures = 0;


static _Atomic bool
    accept_events = false;


static _Atomic bool
    consumer_running = false;


static pthread_t
    consumer_thread;


/*
 * ==========================================================================
 * Thread lifecycle TLS
 * ==========================================================================
 */

static pthread_key_t
    thread_exit_key;


static bool
    thread_exit_key_ready = false;


/*
 * ==========================================================================
 * Monitor TLS
 * ==========================================================================
 */

static _Thread_local
    monitor_thread_context_t *tls_context = NULL;


static _Thread_local
    bool tls_registration_failed = false;


static _Thread_local
    bool tls_internal_thread = false;


/*
 * ==========================================================================
 * Single-writer counter
 * ==========================================================================
 */

static inline void single_writer_increment(
    _Atomic uint64_t *counter
)
{
    const uint64_t value =
        atomic_load_explicit(
            counter,
            memory_order_relaxed
        );


    atomic_store_explicit(
        counter,
        value + 1u,
        memory_order_relaxed
    );
}


/*
 * ==========================================================================
 * Timestamp
 * ==========================================================================
 */

static uint64_t timestamp_now_ns(void)
{
    struct timespec ts;


    if (clock_gettime(
            CLOCK_MONOTONIC_RAW,
            &ts) != 0) {

        return 0;
    }


    return
        ((uint64_t)ts.tv_sec * 1000000000ULL) +
        (uint64_t)ts.tv_nsec;
}


/*
 * ==========================================================================
 * Event emission
 * ==========================================================================
 */

static void emit_event_to_context(
    monitor_thread_context_t *context,
    monitor_event_type_t type,
    uintptr_t object_address,
    int32_t result
)
{
    /*
     * Reserve both sequence numbers BEFORE queue insertion.
     *
     * If the queue is full, the per-thread sequence is still consumed.
     *
     * The next successfully delivered event then exposes the gap.
     */

    const uint64_t sequence_id =
        atomic_fetch_add_explicit(
            &global_sequence,
            1,
            memory_order_relaxed
        );


    const uint32_t thread_sequence =
        context->producer.next_thread_sequence;


    context->producer.next_thread_sequence =
        thread_sequence + 1u;


    monitor_event_t event = {

        .sequence_id =
            sequence_id,

        .timestamp_ns =
            timestamp_now_ns(),

        .thread_id =
            context->thread_id,

        .object_address =
            object_address,

        .event_type =
            (uint32_t)type,

        .result =
            result,

        .flags =
            0,

        .thread_sequence =
            thread_sequence
    };


    single_writer_increment(
        &context->producer.generated
    );


    /*
     * DROP_NEW:
     *
     * Never block application execution.
     */
    if (!spsc_ring_buffer_try_push(
            &context->queue,
            &event)) {

        single_writer_increment(
            &context->producer.dropped
        );
    }
}


/*
 * ==========================================================================
 * Thread-exit TLS destructor
 * ==========================================================================
 */

static void monitor_thread_exit_destructor(
    void *value
)
{
    monitor_thread_context_t *context =
        (monitor_thread_context_t *)value;


    if (context == NULL) {
        return;
    }


    if (atomic_load_explicit(
            &accept_events,
            memory_order_acquire)) {

        emit_event_to_context(
            context,
            MONITOR_EVENT_THREAD_EXIT,
            0,
            0
        );
    }


    tls_context = NULL;
}


/*
 * ==========================================================================
 * Slot allocation
 * ==========================================================================
 */

static bool claim_thread_slot(
    uint32_t *slot
)
{
    uint32_t expected =
        atomic_load_explicit(
            &next_thread_slot,
            memory_order_relaxed
        );


    for (;;) {

        if (expected >= MONITOR_MAX_THREADS) {
            return false;
        }


        if (atomic_compare_exchange_weak_explicit(
                &next_thread_slot,
                &expected,
                expected + 1u,
                memory_order_acq_rel,
                memory_order_relaxed)) {

            *slot = expected;

            return true;
        }
    }
}


/*
 * ==========================================================================
 * Thread registration
 * ==========================================================================
 */

static monitor_thread_context_t *
register_current_thread(void)
{
    if (tls_context != NULL) {
        return tls_context;
    }


    if (tls_registration_failed) {

        atomic_fetch_add_explicit(
            &unregistered_events,
            1,
            memory_order_relaxed
        );


        return NULL;
    }


    uint32_t slot = 0;


    if (!claim_thread_slot(
            &slot)) {

        tls_registration_failed = true;


        atomic_fetch_add_explicit(
            &unregistered_events,
            1,
            memory_order_relaxed
        );


        return NULL;
    }


    monitor_thread_context_t *context =
        &thread_contexts[slot];


    spsc_ring_buffer_init(
        &context->queue
    );


    context->thread_id =
        (uint64_t)syscall(
            SYS_gettid
        );


    atomic_store_explicit(
        &context->producer.generated,
        0,
        memory_order_relaxed
    );


    atomic_store_explicit(
        &context->producer.dropped,
        0,
        memory_order_relaxed
    );


    context->producer.next_thread_sequence =
        0;


    atomic_store_explicit(
        &context->consumer.consumed,
        0,
        memory_order_relaxed
    );


    atomic_store_explicit(
        &context->active,
        true,
        memory_order_release
    );


    tls_context =
        context;


    if (thread_exit_key_ready) {

        const int rc =
            pthread_setspecific(
                thread_exit_key,
                context
            );


        if (rc != 0) {

            atomic_fetch_add_explicit(
                &lifecycle_registration_failures,
                1,
                memory_order_relaxed
            );
        }

    } else {

        atomic_fetch_add_explicit(
            &lifecycle_registration_failures,
            1,
            memory_order_relaxed
        );
    }


    /*
     * First event from every participating application thread.
     *
     * thread_sequence = 0
     */
    emit_event_to_context(
        context,
        MONITOR_EVENT_THREAD_START,
        0,
        0
    );


    return context;
}


/*
 * ==========================================================================
 * Public producer API
 * ==========================================================================
 */

void monitor_runtime_emit(
    monitor_event_type_t type,
    uintptr_t object_address,
    int32_t result
)
{
    if (!atomic_load_explicit(
            &accept_events,
            memory_order_relaxed)) {

        return;
    }


    if (tls_internal_thread) {
        return;
    }


    monitor_thread_context_t *context =
        register_current_thread();


    if (context == NULL) {
        return;
    }


    emit_event_to_context(
        context,
        type,
        object_address,
        result
    );
}


/*
 * ==========================================================================
 * L3 -> L4
 * ==========================================================================
 */

static bool drain_context(
    monitor_thread_context_t *context
)
{
    bool consumed_any = false;

    monitor_event_t event;


    for (uint32_t i = 0;
         i < MONITOR_DRAIN_BATCH;
         ++i) {

        if (!spsc_ring_buffer_try_pop(
                &context->queue,
                &event)) {

            break;
        }


        monitor_verifier_process_event(
            &event
        );


        single_writer_increment(
            &context->consumer.consumed
        );


        consumed_any = true;
    }


    return consumed_any;
}


/*
 * ==========================================================================
 * Consumer
 * ==========================================================================
 */

static void *monitor_consumer_main(
    void *arg
)
{
    (void)arg;


    tls_internal_thread =
        true;


    for (;;) {

        bool consumed_any = false;


        const uint32_t count =
            atomic_load_explicit(
                &next_thread_slot,
                memory_order_acquire
            );


        for (uint32_t i = 0;
             i < count;
             ++i) {

            monitor_thread_context_t *context =
                &thread_contexts[i];


            if (!atomic_load_explicit(
                    &context->active,
                    memory_order_acquire)) {

                continue;
            }


            if (drain_context(
                    context)) {

                consumed_any = true;
            }
        }


        /*
         * Expensive graph checking only at quiescence.
         */
        if (!consumed_any) {

            monitor_verifier_check_deadlocks();
        }


        if (!atomic_load_explicit(
                &consumer_running,
                memory_order_acquire)) {

            if (!consumed_any) {
                break;
            }
        }


        if (!consumed_any) {

            sched_yield();
        }
    }


    return NULL;
}


/*
 * ==========================================================================
 * Transport summary
 * ==========================================================================
 */

static bool print_transport_summary(void)
{
    uint64_t total_generated = 0;

    uint64_t total_consumed = 0;

    uint64_t total_dropped = 0;

    uint32_t registered_threads = 0;


    const uint32_t count =
        atomic_load_explicit(
            &next_thread_slot,
            memory_order_relaxed
        );


    char thread_buffer[4096];

    size_t thread_position = 0;


    int written =
        snprintf(
            thread_buffer,
            sizeof(thread_buffer),

            "\n"
            "=== Per-thread transport statistics ===\n"
        );


    if (written > 0) {

        thread_position =
            (size_t)written;


        if (thread_position >=
            sizeof(thread_buffer)) {

            thread_position =
                sizeof(thread_buffer) - 1u;
        }
    }


    for (uint32_t i = 0;
         i < count;
         ++i) {

        monitor_thread_context_t *context =
            &thread_contexts[i];


        if (!atomic_load_explicit(
                &context->active,
                memory_order_relaxed)) {

            continue;
        }


        ++registered_threads;


        const uint64_t generated =
            atomic_load_explicit(
                &context->producer.generated,
                memory_order_relaxed
            );


        const uint64_t consumed =
            atomic_load_explicit(
                &context->consumer.consumed,
                memory_order_relaxed
            );


        const uint64_t dropped =
            atomic_load_explicit(
                &context->producer.dropped,
                memory_order_relaxed
            );


        total_generated +=
            generated;


        total_consumed +=
            consumed;


        total_dropped +=
            dropped;


        uint64_t loss_ppm = 0;


        if (generated != 0) {

            loss_ppm =
                (
                    dropped *
                    1000000ULL
                ) /
                generated;
        }


        if (thread_position <
            sizeof(thread_buffer) - 1u) {

            written =
                snprintf(
                    thread_buffer +
                        thread_position,

                    sizeof(thread_buffer) -
                        thread_position,

                    "slot=%u "
                    "tid=%lu "
                    "generated=%lu "
                    "consumed=%lu "
                    "dropped=%lu "
                    "loss=%lu ppm\n",

                    i,

                    (unsigned long)
                        context->thread_id,

                    (unsigned long)
                        generated,

                    (unsigned long)
                        consumed,

                    (unsigned long)
                        dropped,

                    (unsigned long)
                        loss_ppm
                );


            if (written > 0) {

                const size_t available =
                    sizeof(thread_buffer) -
                    thread_position;


                if ((size_t)written >=
                    available) {

                    thread_position =
                        sizeof(thread_buffer) - 1u;

                } else {

                    thread_position +=
                        (size_t)written;
                }
            }
        }
    }


    if (thread_position > 0) {

        (void)write(
            STDERR_FILENO,
            thread_buffer,
            thread_position
        );
    }


    const uint64_t rejected =
        atomic_load_explicit(
            &unregistered_events,
            memory_order_relaxed
        );


    const uint64_t lifecycle_failures =
        atomic_load_explicit(
            &lifecycle_registration_failures,
            memory_order_relaxed
        );


    const bool transport_consistent =
        total_generated ==
        (
            total_consumed +
            total_dropped
        );


    const bool trace_complete =
        transport_consistent &&
        total_dropped == 0 &&
        rejected == 0 &&
        lifecycle_failures == 0;


    uint64_t loss_ppm = 0;


    if (total_generated != 0) {

        loss_ppm =
            (
                total_dropped *
                1000000ULL
            ) /
            total_generated;
    }


    char buffer[1536];


    const int length =
        snprintf(
            buffer,
            sizeof(buffer),

            "\n"
            "=== Monitor transport summary ===\n"
            "Registered threads : %u / %u\n"
            "Generated events   : %lu\n"
            "Consumed events    : %lu\n"
            "Dropped events     : %lu\n"
            "Loss rate          : %lu ppm\n"
            "Unregistered events: %lu\n"
            "Lifecycle failures : %lu\n"
            "Transport invariant: %s\n"
            "Trace integrity    : %s\n"
            "Event size         : %zu bytes\n"
            "Queue size         : %zu bytes\n"
            "Context size       : %zu bytes\n"
            "Queue capacity     : %u events\n"
            "Drain batch        : %u events\n",

            registered_threads,
            MONITOR_MAX_THREADS,

            (unsigned long)
                total_generated,

            (unsigned long)
                total_consumed,

            (unsigned long)
                total_dropped,

            (unsigned long)
                loss_ppm,

            (unsigned long)
                rejected,

            (unsigned long)
                lifecycle_failures,

            transport_consistent
                ? "PASS"
                : "FAIL",

            trace_complete
                ? "COMPLETE"
                : "DEGRADED",

            sizeof(monitor_event_t),
            sizeof(spsc_ring_buffer_t),
            sizeof(monitor_thread_context_t),

            SPSC_RING_CAPACITY,
            MONITOR_DRAIN_BATCH
        );


    if (length > 0) {

        size_t output_length =
            (size_t)length;


        if (output_length >=
            sizeof(buffer)) {

            output_length =
                sizeof(buffer) - 1u;
        }


        (void)write(
            STDERR_FILENO,
            buffer,
            output_length
        );
    }


    return trace_complete;
}


/*
 * ==========================================================================
 * Startup
 * ==========================================================================
 */

int monitor_runtime_start(void)
{
    atomic_store_explicit(
        &next_thread_slot,
        0,
        memory_order_relaxed
    );


    atomic_store_explicit(
        &global_sequence,
        0,
        memory_order_relaxed
    );


    atomic_store_explicit(
        &unregistered_events,
        0,
        memory_order_relaxed
    );


    atomic_store_explicit(
        &lifecycle_registration_failures,
        0,
        memory_order_relaxed
    );


    atomic_store_explicit(
        &accept_events,
        false,
        memory_order_relaxed
    );


    for (uint32_t i = 0;
         i < MONITOR_MAX_THREADS;
         ++i) {

        atomic_store_explicit(
            &thread_contexts[i].active,
            false,
            memory_order_relaxed
        );
    }


    monitor_verifier_init();


    const int key_rc =
        pthread_key_create(
            &thread_exit_key,
            monitor_thread_exit_destructor
        );


    if (key_rc != 0) {
        return key_rc;
    }


    thread_exit_key_ready =
        true;


    atomic_store_explicit(
        &consumer_running,
        true,
        memory_order_release
    );


    const int consumer_rc =
        pthread_create(
            &consumer_thread,
            NULL,
            monitor_consumer_main,
            NULL
        );


    if (consumer_rc != 0) {

        thread_exit_key_ready =
            false;


        (void)pthread_key_delete(
            thread_exit_key
        );


        atomic_store_explicit(
            &consumer_running,
            false,
            memory_order_release
        );


        return consumer_rc;
    }


    atomic_store_explicit(
        &accept_events,
        true,
        memory_order_release
    );


    return 0;
}


/*
 * ==========================================================================
 * Shutdown
 * ==========================================================================
 */

void monitor_runtime_stop(void)
{
    atomic_store_explicit(
        &accept_events,
        false,
        memory_order_release
    );


    atomic_store_explicit(
        &consumer_running,
        false,
        memory_order_release
    );


    (void)pthread_join(
        consumer_thread,
        NULL
    );


    const bool trace_complete =
        print_transport_summary();


    monitor_verifier_print_summary(
        trace_complete
    );


    if (thread_exit_key_ready) {

        thread_exit_key_ready =
            false;


        (void)pthread_key_delete(
            thread_exit_key
        );
    }
}


/*
 * ==========================================================================
 * Internal monitor-thread query
 * ==========================================================================
 */

bool monitor_runtime_is_internal_thread(void)
{
    return tls_internal_thread;
}