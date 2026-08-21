#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "monitor_event.h"
#include "ring_buffer.h"


#define CONCURRENT_EVENT_COUNT 200000u


static spsc_ring_buffer_t concurrent_buffer;


static int test_basic_fifo(void)
{
    spsc_ring_buffer_t buffer;

    spsc_ring_buffer_init(
        &buffer
    );

    monitor_event_t input = {
        .sequence_id = 42,
        .timestamp_ns = 1000,
        .thread_id = 7,
        .object_address = 0x1234,
        .event_type = MONITOR_EVENT_LOCK_REQUEST,
        .result = 0,
        .flags = 0
    };

    monitor_event_t output = {0};


    if (!spsc_ring_buffer_try_push(
            &buffer,
            &input)) {

        fprintf(
            stderr,
            "[FAIL] basic push failed\n"
        );

        return -1;
    }


    if (!spsc_ring_buffer_try_pop(
            &buffer,
            &output)) {

        fprintf(
            stderr,
            "[FAIL] basic pop failed\n"
        );

        return -1;
    }


    if (output.sequence_id !=
        input.sequence_id) {

        fprintf(
            stderr,
            "[FAIL] FIFO data mismatch\n"
        );

        return -1;
    }


    if (spsc_ring_buffer_try_pop(
            &buffer,
            &output)) {

        fprintf(
            stderr,
            "[FAIL] empty queue returned data\n"
        );

        return -1;
    }


    printf(
        "[PASS] basic FIFO\n"
    );

    return 0;
}


static int test_capacity(void)
{
    spsc_ring_buffer_t buffer;

    spsc_ring_buffer_init(
        &buffer
    );


    for (uint64_t i = 0;
         i < SPSC_RING_CAPACITY;
         ++i) {

        monitor_event_t event = {
            .sequence_id = i
        };

        if (!spsc_ring_buffer_try_push(
                &buffer,
                &event)) {

            fprintf(
                stderr,
                "[FAIL] queue became full too early\n"
            );

            return -1;
        }
    }


    monitor_event_t overflow_event = {
        .sequence_id = UINT64_MAX
    };


    if (spsc_ring_buffer_try_push(
            &buffer,
            &overflow_event)) {

        fprintf(
            stderr,
            "[FAIL] full queue accepted event\n"
        );

        return -1;
    }


    for (uint64_t i = 0;
         i < SPSC_RING_CAPACITY;
         ++i) {

        monitor_event_t event;

        if (!spsc_ring_buffer_try_pop(
                &buffer,
                &event)) {

            fprintf(
                stderr,
                "[FAIL] queue became empty too early\n"
            );

            return -1;
        }


        if (event.sequence_id != i) {

            fprintf(
                stderr,
                "[FAIL] queue order corrupted\n"
            );

            return -1;
        }
    }


    printf(
        "[PASS] bounded capacity\n"
    );

    return 0;
}


static int test_wraparound(void)
{
    spsc_ring_buffer_t buffer;

    spsc_ring_buffer_init(
        &buffer
    );


    /*
     * More operations than queue capacity to force repeated
     * array-index wraparound.
     */
    const uint64_t count =
        SPSC_RING_CAPACITY * 10u;


    for (uint64_t i = 0;
         i < count;
         ++i) {

        monitor_event_t input = {
            .sequence_id = i
        };

        monitor_event_t output;


        if (!spsc_ring_buffer_try_push(
                &buffer,
                &input)) {

            fprintf(
                stderr,
                "[FAIL] wraparound push failed\n"
            );

            return -1;
        }


        if (!spsc_ring_buffer_try_pop(
                &buffer,
                &output)) {

            fprintf(
                stderr,
                "[FAIL] wraparound pop failed\n"
            );

            return -1;
        }


        if (output.sequence_id != i) {

            fprintf(
                stderr,
                "[FAIL] wraparound sequence mismatch\n"
            );

            return -1;
        }
    }


    printf(
        "[PASS] wraparound\n"
    );

    return 0;
}


static void *producer_thread(
    void *arg
)
{
    (void)arg;


    for (uint64_t i = 0;
         i < CONCURRENT_EVENT_COUNT;
         ++i) {

        monitor_event_t event = {
            .sequence_id = i,
            .thread_id = 1,
            .event_type =
                MONITOR_EVENT_LOCK_REQUEST
        };


        /*
         * For this unit test only, retry when full.
         *
         * The production monitor will NOT retry: it will drop
         * the event and increment a loss counter instead.
         */
        while (!spsc_ring_buffer_try_push(
                    &concurrent_buffer,
                    &event)) {

            sched_yield();
        }
    }


    return NULL;
}


static int test_concurrent_spsc(void)
{
    pthread_t producer;

    spsc_ring_buffer_init(
        &concurrent_buffer
    );


    int rc = pthread_create(
        &producer,
        NULL,
        producer_thread,
        NULL
    );


    if (rc != 0) {

        fprintf(
            stderr,
            "[FAIL] pthread_create: %d\n",
            rc
        );

        return -1;
    }


    for (uint64_t expected = 0;
         expected < CONCURRENT_EVENT_COUNT;
         ++expected) {

        monitor_event_t event;


        while (!spsc_ring_buffer_try_pop(
                    &concurrent_buffer,
                    &event)) {

            sched_yield();
        }


        if (event.sequence_id != expected) {

            fprintf(
                stderr,
                "[FAIL] concurrent sequence mismatch: "
                "expected=%lu actual=%lu\n",
                (unsigned long)expected,
                (unsigned long)event.sequence_id
            );

            return -1;
        }
    }


    rc = pthread_join(
        producer,
        NULL
    );


    if (rc != 0) {

        fprintf(
            stderr,
            "[FAIL] pthread_join: %d\n",
            rc
        );

        return -1;
    }


    printf(
        "[PASS] concurrent SPSC (%u events)\n",
        CONCURRENT_EVENT_COUNT
    );

    return 0;
}


int main(void)
{
    printf(
        "=== SPSC ring buffer tests ===\n"
    );

    printf(
        "monitor_event_t size: %zu bytes\n",
        sizeof(monitor_event_t)
    );

    printf(
        "ring buffer size:    %zu bytes\n",
        sizeof(spsc_ring_buffer_t)
    );


    if (test_basic_fifo() != 0) {
        return EXIT_FAILURE;
    }

    if (test_capacity() != 0) {
        return EXIT_FAILURE;
    }

    if (test_wraparound() != 0) {
        return EXIT_FAILURE;
    }

    if (test_concurrent_spsc() != 0) {
        return EXIT_FAILURE;
    }


    printf(
        "[PASS] all SPSC tests\n"
    );

    return EXIT_SUCCESS;
}
