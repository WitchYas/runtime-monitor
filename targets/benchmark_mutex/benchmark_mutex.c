#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define BENCH_MAX_THREADS 64u


typedef struct {

    uint64_t iterations;

    uint32_t work;

} worker_config_t;


/*
 * ==========================================================================
 * Shared benchmark state
 * ==========================================================================
 */

static pthread_mutex_t shared_mutex =
    PTHREAD_MUTEX_INITIALIZER;


static pthread_barrier_t
    start_barrier;


static uint64_t
    shared_counter = 0;


/*
 * Prevent the optional arithmetic work from being optimized away.
 */
static volatile uint64_t
    work_sink = 0;


static worker_config_t
    worker_config;


/*
 * ==========================================================================
 * Timestamp
 * ==========================================================================
 */

static uint64_t now_ns(void)
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
 * Command-line help
 * ==========================================================================
 */

static void usage(
    const char *program
)
{
    fprintf(
        stderr,

        "Usage: %s "
        "[--threads N] "
        "[--iterations N] "
        "[--work N]\n"
        "\n"
        "  --threads     worker threads (1..%u), default 4\n"
        "  --iterations  lock/unlock iterations per thread, default 250000\n"
        "  --work        arithmetic operations inside critical section, default 0\n",

        program,
        BENCH_MAX_THREADS
    );
}


/*
 * ==========================================================================
 * Integer parsing
 * ==========================================================================
 */

static int parse_u64(
    const char *text,
    uint64_t *value
)
{
    errno = 0;

    char *end = NULL;


    const unsigned long long parsed =
        strtoull(
            text,
            &end,
            10
        );


    if (errno != 0 ||
        end == text ||
        *end != '\0') {

        return -1;
    }


    *value =
        (uint64_t)parsed;


    return 0;
}


/*
 * ==========================================================================
 * Worker
 * ==========================================================================
 */

static void *worker_main(
    void *arg
)
{
    (void)arg;


    const int barrier_rc =
        pthread_barrier_wait(
            &start_barrier
        );


    if (barrier_rc != 0 &&
        barrier_rc !=
            PTHREAD_BARRIER_SERIAL_THREAD) {

        return
            (void *)(uintptr_t)1u;
    }


    uint64_t local_sink =
        0x9e3779b97f4a7c15ULL;


    for (uint64_t i = 0;
         i < worker_config.iterations;
         ++i) {

        if (pthread_mutex_lock(
                &shared_mutex) != 0) {

            return
                (void *)(uintptr_t)2u;
        }


        ++shared_counter;


        /*
         * Optional deterministic work while the mutex is held.
         *
         * work=0:
         *     synchronization-heavy workload
         *
         * work>0:
         *     less artificial application workload
         */

        uint64_t x =
            shared_counter ^
            local_sink;


        for (uint32_t j = 0;
             j < worker_config.work;
             ++j) {

            x =
                (
                    x *
                    6364136223846793005ULL
                ) +
                1442695040888963407ULL;


            x ^=
                x >> 17;
        }


        local_sink ^=
            x;


        work_sink =
            local_sink;


        if (pthread_mutex_unlock(
                &shared_mutex) != 0) {

            return
                (void *)(uintptr_t)3u;
        }
    }


    return NULL;
}


/*
 * ==========================================================================
 * Main
 * ==========================================================================
 */

int main(
    int argc,
    char **argv
)
{
    uint32_t threads =
        4u;


    uint64_t iterations =
        250000ULL;


    uint32_t work =
        0u;


    /*
     * ----------------------------------------------------------------------
     * Arguments
     * ----------------------------------------------------------------------
     */

    for (int i = 1;
         i < argc;
         ++i) {

        if (strcmp(
                argv[i],
                "--threads") == 0 &&
            i + 1 < argc) {

            uint64_t value = 0;


            if (parse_u64(
                    argv[++i],
                    &value) != 0 ||
                value == 0 ||
                value > BENCH_MAX_THREADS) {

                usage(
                    argv[0]
                );

                return
                    EXIT_FAILURE;
            }


            threads =
                (uint32_t)value;


        } else if (
            strcmp(
                argv[i],
                "--iterations") == 0 &&
            i + 1 < argc) {

            if (parse_u64(
                    argv[++i],
                    &iterations) != 0 ||
                iterations == 0) {

                usage(
                    argv[0]
                );

                return
                    EXIT_FAILURE;
            }


        } else if (
            strcmp(
                argv[i],
                "--work") == 0 &&
            i + 1 < argc) {

            uint64_t value = 0;


            if (parse_u64(
                    argv[++i],
                    &value) != 0 ||
                value > UINT32_MAX) {

                usage(
                    argv[0]
                );

                return
                    EXIT_FAILURE;
            }


            work =
                (uint32_t)value;


        } else if (
            strcmp(
                argv[i],
                "--help") == 0 ||
            strcmp(
                argv[i],
                "-h") == 0) {

            usage(
                argv[0]
            );

            return
                EXIT_SUCCESS;


        } else {

            usage(
                argv[0]
            );

            return
                EXIT_FAILURE;
        }
    }


    if (iterations >
        UINT64_MAX / threads) {

        fprintf(
            stderr,
            "Requested operation count overflows uint64_t\n"
        );


        return
            EXIT_FAILURE;
    }


    const uint64_t expected =
        iterations *
        (uint64_t)threads;


    worker_config.iterations =
        iterations;


    worker_config.work =
        work;


    shared_counter =
        0;


    work_sink =
        0;


    /*
     * ----------------------------------------------------------------------
     * Start barrier
     * ----------------------------------------------------------------------
     */

    if (pthread_barrier_init(
            &start_barrier,
            NULL,
            threads + 1u) != 0) {

        perror(
            "pthread_barrier_init"
        );


        return
            EXIT_FAILURE;
    }


    /*
     * ----------------------------------------------------------------------
     * Thread allocation
     * ----------------------------------------------------------------------
     */

    pthread_t *workers =
        calloc(
            threads,
            sizeof(*workers)
        );


    if (workers == NULL) {

        perror(
            "calloc"
        );


        (void)pthread_barrier_destroy(
            &start_barrier
        );


        return
            EXIT_FAILURE;
    }


    /*
     * ----------------------------------------------------------------------
     * Create workers
     * ----------------------------------------------------------------------
     */

    uint32_t created =
        0;


    for (;
         created < threads;
         ++created) {

        const int rc =
            pthread_create(
                &workers[created],
                NULL,
                worker_main,
                NULL
            );


        if (rc != 0) {

            fprintf(
                stderr,
                "pthread_create failed: %d\n",
                rc
            );


            break;
        }
    }


    if (created !=
        threads) {

        free(
            workers
        );


        return
            EXIT_FAILURE;
    }


    /*
     * ----------------------------------------------------------------------
     * Timed region
     * ----------------------------------------------------------------------
     */

    const uint64_t start_ns =
        now_ns();


    const int barrier_rc =
        pthread_barrier_wait(
            &start_barrier
        );


    if (barrier_rc != 0 &&
        barrier_rc !=
            PTHREAD_BARRIER_SERIAL_THREAD) {

        fprintf(
            stderr,
            "pthread_barrier_wait failed: %d\n",
            barrier_rc
        );


        free(
            workers
        );


        return
            EXIT_FAILURE;
    }


    int worker_failed =
        0;


    for (uint32_t i = 0;
         i < threads;
         ++i) {

        void *result =
            NULL;


        const int rc =
            pthread_join(
                workers[i],
                &result
            );


        if (rc != 0 ||
            result != NULL) {

            worker_failed =
                1;
        }
    }


    const uint64_t end_ns =
        now_ns();


    const uint64_t elapsed_ns =
        end_ns >= start_ns
        ?
            end_ns - start_ns
        :
            0;


    /*
     * ----------------------------------------------------------------------
     * Correctness
     * ----------------------------------------------------------------------
     */

    const int counter_ok =
        shared_counter ==
        expected;


    const int status_ok =
        counter_ok &&
        !worker_failed;


    /*
     * ----------------------------------------------------------------------
     * Human-readable output
     * ----------------------------------------------------------------------
     */

    printf(
        "=== Mutex benchmark ===\n"
    );


    printf(
        "Threads             : %u\n",
        threads
    );


    printf(
        "Iterations/thread   : %" PRIu64 "\n",
        iterations
    );


    printf(
        "Total critical ops  : %" PRIu64 "\n",
        expected
    );


    printf(
        "Work/critical op    : %u\n",
        work
    );


    printf(
        "Elapsed             : %" PRIu64 " ns\n",
        elapsed_ns
    );


    printf(
        "Expected counter    : %" PRIu64 "\n",
        expected
    );


    printf(
        "Actual counter      : %" PRIu64 "\n",
        shared_counter
    );


    printf(
        "[%s] Benchmark correctness\n",
        status_ok
        ?
            "PASS"
        :
            "FAIL"
    );


    /*
     * ----------------------------------------------------------------------
     * Machine-readable result
     * ----------------------------------------------------------------------
     */

    printf(
        "BENCH_RESULT "
        "threads=%u "
        "iterations=%" PRIu64 " "
        "total_ops=%" PRIu64 " "
        "work=%u "
        "elapsed_ns=%" PRIu64 " "
        "counter=%" PRIu64 " "
        "expected=%" PRIu64 " "
        "status=%s\n",

        threads,
        iterations,
        expected,
        work,
        elapsed_ns,
        shared_counter,
        expected,

        status_ok
        ?
            "PASS"
        :
            "FAIL"
    );


    /*
     * ----------------------------------------------------------------------
     * Cleanup
     * ----------------------------------------------------------------------
     */

    free(
        workers
    );


    (void)pthread_barrier_destroy(
        &start_barrier
    );


    (void)pthread_mutex_destroy(
        &shared_mutex
    );


    return
        status_ok
        ?
            EXIT_SUCCESS
        :
            EXIT_FAILURE;
}