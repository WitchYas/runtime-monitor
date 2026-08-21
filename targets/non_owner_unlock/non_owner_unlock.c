#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>


static pthread_mutex_t mutex;

static pthread_barrier_t owner_ready;
static pthread_barrier_t unlock_attempt_complete;


static void *owner_thread(void *arg)
{
    (void)arg;


    printf("[OWNER] Locking mutex\n");


    if (pthread_mutex_lock(&mutex) != 0) {

        fprintf(
            stderr,
            "[FAIL] Owner could not lock mutex\n"
        );

        return NULL;
    }


    printf("[OWNER] Mutex acquired\n");


    (void)pthread_barrier_wait(
        &owner_ready
    );


    /*
     * Do not release until the invalid unlock attempt
     * has actually completed.
     */
    (void)pthread_barrier_wait(
        &unlock_attempt_complete
    );


    printf("[OWNER] Releasing mutex\n");


    (void)pthread_mutex_unlock(
        &mutex
    );


    return NULL;
}


static void *non_owner_thread(void *arg)
{
    (void)arg;


    (void)pthread_barrier_wait(
        &owner_ready
    );


    printf(
        "[NON-OWNER] Attempting invalid unlock\n"
    );


    const int rc =
        pthread_mutex_unlock(
            &mutex
        );


    printf(
        "[NON-OWNER] pthread_mutex_unlock returned %d\n",
        rc
    );


    if (rc == EPERM) {

        printf(
            "[PASS] Non-owner unlock rejected with EPERM\n"
        );

    } else {

        printf(
            "[FAIL] Expected EPERM=%d\n",
            EPERM
        );
    }


    (void)pthread_barrier_wait(
        &unlock_attempt_complete
    );


    return NULL;
}


int main(void)
{
    printf(
        "=== Non-owner unlock target ===\n"
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


    if (pthread_barrier_init(
            &owner_ready,
            NULL,
            2) != 0) {

        return EXIT_FAILURE;
    }


    if (pthread_barrier_init(
            &unlock_attempt_complete,
            NULL,
            2) != 0) {

        return EXIT_FAILURE;
    }


    pthread_t owner;
    pthread_t non_owner;


    if (pthread_create(
            &owner,
            NULL,
            owner_thread,
            NULL) != 0) {

        return EXIT_FAILURE;
    }


    if (pthread_create(
            &non_owner,
            NULL,
            non_owner_thread,
            NULL) != 0) {

        return EXIT_FAILURE;
    }


    (void)pthread_join(
        owner,
        NULL
    );


    (void)pthread_join(
        non_owner,
        NULL
    );


    (void)pthread_barrier_destroy(
        &owner_ready
    );


    (void)pthread_barrier_destroy(
        &unlock_attempt_complete
    );


    (void)pthread_mutex_destroy(
        &mutex
    );


    return EXIT_SUCCESS;
}