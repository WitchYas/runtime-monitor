#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define THREAD_COUNT 4
#define ITERATIONS_PER_THREAD 250000

static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t shared_counter = 0;

static void *worker(void *arg)
{
    (void)arg;

    for (uint64_t i = 0; i < ITERATIONS_PER_THREAD; ++i) {

        int rc = pthread_mutex_lock(&counter_mutex);

        if (rc != 0) {
            fprintf(stderr,
                    "pthread_mutex_lock failed: %d\n",
                    rc);
            return (void *)1;
        }

        ++shared_counter;

        rc = pthread_mutex_unlock(&counter_mutex);

        if (rc != 0) {
            fprintf(stderr,
                    "pthread_mutex_unlock failed: %d\n",
                    rc);
            return (void *)1;
        }
    }

    return NULL;
}

int main(void)
{
    pthread_t threads[THREAD_COUNT];

    printf("=== Correct mutex target ===\n");
    printf("Threads: %d\n", THREAD_COUNT);
    printf("Iterations/thread: %d\n", ITERATIONS_PER_THREAD);

    for (int i = 0; i < THREAD_COUNT; ++i) {

        int rc = pthread_create(
            &threads[i],
            NULL,
            worker,
            NULL
        );

        if (rc != 0) {
            fprintf(stderr,
                    "pthread_create failed: %d\n",
                    rc);
            return EXIT_FAILURE;
        }
    }

    for (int i = 0; i < THREAD_COUNT; ++i) {

        void *thread_result = NULL;

        int rc = pthread_join(
            threads[i],
            &thread_result
        );

        if (rc != 0) {
            fprintf(stderr,
                    "pthread_join failed: %d\n",
                    rc);
            return EXIT_FAILURE;
        }

        if (thread_result != NULL) {
            fprintf(stderr,
                    "Worker thread reported failure\n");
            return EXIT_FAILURE;
        }
    }

    const uint64_t expected =
        (uint64_t)THREAD_COUNT * ITERATIONS_PER_THREAD;

    printf("Expected counter: %lu\n",
           (unsigned long)expected);

    printf("Actual counter:   %lu\n",
           (unsigned long)shared_counter);

    if (shared_counter != expected) {
        fprintf(stderr,
                "[FAIL] Counter mismatch\n");
        return EXIT_FAILURE;
    }

    printf("[PASS] Synchronization is correct\n");

    return EXIT_SUCCESS;
}
