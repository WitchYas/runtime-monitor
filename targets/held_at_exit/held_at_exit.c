#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>


static pthread_mutex_t mutex =
    PTHREAD_MUTEX_INITIALIZER;


static void *worker(void *arg)
{
    (void)arg;


    printf(
        "[WORKER] Locking mutex\n"
    );


    const int rc =
        pthread_mutex_lock(
            &mutex
        );


    if (rc != 0) {

        printf(
            "[FAIL] pthread_mutex_lock returned %d\n",
            rc
        );

        return NULL;
    }


    printf(
        "[WORKER] Mutex acquired\n"
    );


    printf(
        "[WORKER] Exiting without unlocking\n"
    );


    /*
     * Intentionally return while owning mutex.
     */
    return NULL;
}


int main(void)
{
    printf(
        "=== Lock-held-at-thread-exit target ===\n"
    );


    pthread_t thread;


    if (pthread_create(
            &thread,
            NULL,
            worker,
            NULL) != 0) {

        return EXIT_FAILURE;
    }


    (void)pthread_join(
        thread,
        NULL
    );


    printf(
        "[TARGET] Worker has exited\n"
    );


    /*
     * Do not attempt to destroy the intentionally still-locked
     * mutex. Process termination will reclaim it.
     */

    return EXIT_SUCCESS;
}