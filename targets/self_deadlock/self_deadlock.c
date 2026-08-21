#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>


static pthread_mutex_t mutex;


static void *worker(void *arg)
{
    (void)arg;


    printf(
        "[WORKER] First mutex lock\n"
    );


    const int first_rc =
        pthread_mutex_lock(
            &mutex
        );


    if (first_rc != 0) {

        printf(
            "[FAIL] First lock returned %d\n",
            first_rc
        );

        return NULL;
    }


    printf(
        "[WORKER] First lock acquired\n"
    );


    printf(
        "[WORKER] Requesting same mutex again\n"
    );


    const int second_rc =
        pthread_mutex_lock(
            &mutex
        );


    printf(
        "[WORKER] Second lock returned %d\n",
        second_rc
    );


    if (second_rc == EDEADLK) {

        printf(
            "[PASS] ERRORCHECK mutex rejected self-lock with EDEADLK\n"
        );

    } else {

        printf(
            "[FAIL] Expected EDEADLK=%d\n",
            EDEADLK
        );
    }


    (void)pthread_mutex_unlock(
        &mutex
    );


    return NULL;
}


int main(void)
{
    printf(
        "=== Self-deadlock candidate target ===\n"
    );


    pthread_mutexattr_t attr;


    if (pthread_mutexattr_init(
            &attr) != 0) {

        return EXIT_FAILURE;
    }


    if (pthread_mutexattr_settype(
            &attr,
            PTHREAD_MUTEX_ERRORCHECK) != 0) {

        return EXIT_FAILURE;
    }


    if (pthread_mutex_init(
            &mutex,
            &attr) != 0) {

        return EXIT_FAILURE;
    }


    (void)pthread_mutexattr_destroy(
        &attr
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