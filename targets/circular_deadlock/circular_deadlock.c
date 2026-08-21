#define _GNU_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t mutex_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_b = PTHREAD_MUTEX_INITIALIZER;

static pthread_barrier_t barrier;

static int wait_at_barrier(void)
{
    int rc = pthread_barrier_wait(&barrier);

    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr, "pthread_barrier_wait failed: %d\n", rc);
        return -1;
    }

    return 0;
}

static void *thread_one(void *arg)
{
    (void)arg;

    printf("[T1] Locking mutex A\n");

    if (pthread_mutex_lock(&mutex_a) != 0) {
        fprintf(stderr, "[T1] Failed to lock mutex A\n");
        return (void *)1;
    }

    printf("[T1] Acquired mutex A\n");

    if (wait_at_barrier() != 0) {
        return (void *)1;
    }

    printf("[T1] Requesting mutex B\n");

    /*
     * Expected to block because T2 owns mutex B.
     */
    if (pthread_mutex_lock(&mutex_b) != 0) {
        fprintf(stderr, "[T1] Failed to lock mutex B\n");
        return (void *)1;
    }

    pthread_mutex_unlock(&mutex_b);
    pthread_mutex_unlock(&mutex_a);

    return NULL;
}

static void *thread_two(void *arg)
{
    (void)arg;

    printf("[T2] Locking mutex B\n");

    if (pthread_mutex_lock(&mutex_b) != 0) {
        fprintf(stderr, "[T2] Failed to lock mutex B\n");
        return (void *)1;
    }

    printf("[T2] Acquired mutex B\n");

    if (wait_at_barrier() != 0) {
        return (void *)1;
    }

    printf("[T2] Requesting mutex A\n");

    /*
     * Expected to block because T1 owns mutex A.
     */
    if (pthread_mutex_lock(&mutex_a) != 0) {
        fprintf(stderr, "[T2] Failed to lock mutex A\n");
        return (void *)1;
    }

    pthread_mutex_unlock(&mutex_a);
    pthread_mutex_unlock(&mutex_b);

    return NULL;
}

int main(void)
{
    pthread_t t1;
    pthread_t t2;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== Circular deadlock target ===\n");

    int rc = pthread_barrier_init(&barrier, NULL, 2);

    if (rc != 0) {
        fprintf(stderr, "pthread_barrier_init failed: %d\n", rc);
        return EXIT_FAILURE;
    }

    rc = pthread_create(&t1, NULL, thread_one, NULL);

    if (rc != 0) {
        fprintf(stderr, "pthread_create(T1) failed: %d\n", rc);
        return EXIT_FAILURE;
    }

    rc = pthread_create(&t2, NULL, thread_two, NULL);

    if (rc != 0) {
        fprintf(stderr, "pthread_create(T2) failed: %d\n", rc);
        return EXIT_FAILURE;
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    pthread_barrier_destroy(&barrier);

    return EXIT_SUCCESS;
}
