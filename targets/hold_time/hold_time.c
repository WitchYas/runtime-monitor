#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


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
        "[WORKER] Holding mutex for approximately 200 ms\n"
    );


    const struct timespec delay = {

        .tv_sec = 0,

        .tv_nsec = 200000000L
    };


    (void)nanosleep(
        &delay,
        NULL
    );


    printf(
        "[WORKER] Unlocking mutex\n"
    );


    (void)pthread_mutex_unlock(
        &mutex
    );


    return NULL;
}


int main(void)
{
    printf(
        "=== Hold-time violation target ===\n"
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


    (void)pthread_mutex_destroy(
        &mutex
    );


    return EXIT_SUCCESS;
}