#define _GNU_SOURCE

#include "monitor_runtime.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>


/*
 * ==========================================================================
 * Real pthread function types
 * ==========================================================================
 */

typedef int (*pthread_mutex_lock_fn)(
    pthread_mutex_t *mutex
);

typedef int (*pthread_mutex_unlock_fn)(
    pthread_mutex_t *mutex
);


/*
 * ==========================================================================
 * Real pthread functions
 * ==========================================================================
 */

static pthread_mutex_lock_fn
    real_pthread_mutex_lock = NULL;

static pthread_mutex_unlock_fn
    real_pthread_mutex_unlock = NULL;


/*
 * ==========================================================================
 * Interceptor state
 * ==========================================================================
 */

/*
 * Prevent recursive monitoring caused by operations
 * performed internally by the monitoring library.
 */
static _Thread_local int monitor_internal = 0;


/*
 * True only if monitor_runtime_start() succeeded.
 */
static bool runtime_started = false;


/*
 * ==========================================================================
 * Optional debug output
 * ==========================================================================
 */

#define MONITOR_DEBUG_LIMIT 64u

static bool monitor_debug_enabled = false;

static _Atomic uint64_t debug_event_count = 0;


/*
 * ==========================================================================
 * Debug helper
 * ==========================================================================
 */

static void debug_event(
    const char *event_name,
    pthread_mutex_t *mutex,
    int result,
    bool show_result
)
{
    if (!monitor_debug_enabled) {
        return;
    }


    const uint64_t index =
        atomic_fetch_add_explicit(
            &debug_event_count,
            1,
            memory_order_relaxed
        );


    if (index >= MONITOR_DEBUG_LIMIT) {
        return;
    }


    const long tid =
        syscall(SYS_gettid);


    char buffer[256];


    int length = 0;


    if (show_result) {

        length =
            snprintf(
                buffer,
                sizeof(buffer),

                "[monitor] "
                "tid=%ld "
                "event=%s "
                "mutex=%p "
                "rc=%d\n",

                tid,
                event_name,
                (void *)mutex,
                result
            );

    } else {

        length =
            snprintf(
                buffer,
                sizeof(buffer),

                "[monitor] "
                "tid=%ld "
                "event=%s "
                "mutex=%p\n",

                tid,
                event_name,
                (void *)mutex
            );
    }


    if (length <= 0) {
        return;
    }


    size_t output_length =
        (size_t)length;


    if (output_length >= sizeof(buffer)) {

        output_length =
            sizeof(buffer) - 1u;
    }


    (void)write(
        STDERR_FILENO,
        buffer,
        output_length
    );
}


/*
 * ==========================================================================
 * Resolve real pthread functions
 * ==========================================================================
 */

static bool resolve_real_functions(void)
{
    dlerror();


    void *lock_symbol =
        dlsym(
            RTLD_NEXT,
            "pthread_mutex_lock"
        );


    const char *lock_error =
        dlerror();


    dlerror();


    void *unlock_symbol =
        dlsym(
            RTLD_NEXT,
            "pthread_mutex_unlock"
        );


    const char *unlock_error =
        dlerror();


    if (lock_error != NULL ||
        unlock_error != NULL ||
        lock_symbol == NULL ||
        unlock_symbol == NULL) {

        static const char message[] =
            "[monitor] ERROR: unable to resolve "
            "pthread mutex functions\n";


        (void)write(
            STDERR_FILENO,
            message,
            sizeof(message) - 1u
        );


        return false;
    }


    /*
     * POSIX permits dlsym() results to represent function
     * addresses. memcpy avoids ISO-C function-pointer cast
     * diagnostics under -Wpedantic.
     */
    memcpy(
        &real_pthread_mutex_lock,
        &lock_symbol,
        sizeof(real_pthread_mutex_lock)
    );


    memcpy(
        &real_pthread_mutex_unlock,
        &unlock_symbol,
        sizeof(real_pthread_mutex_unlock)
    );


    return true;
}


/*
 * ==========================================================================
 * Library initialization
 * ==========================================================================
 */

__attribute__((constructor))
static void monitor_library_init(void)
{
    /*
     * Any pthread activity while resolving symbols or creating
     * the monitor thread must bypass instrumentation.
     */
    monitor_internal = 1;


    const char *debug_environment =
        getenv("MONITOR_DEBUG");


    if (debug_environment != NULL &&
        strcmp(debug_environment, "0") != 0 &&
        debug_environment[0] != '\0') {

        monitor_debug_enabled = true;
    }


    atomic_store_explicit(
        &debug_event_count,
        0,
        memory_order_relaxed
    );


    if (!resolve_real_functions()) {

        monitor_internal = 0;

        return;
    }


    const int rc =
        monitor_runtime_start();


    if (rc == 0) {

        runtime_started = true;

    } else {

        static const char message[] =
            "[monitor] ERROR: monitor runtime "
            "could not be started\n";


        (void)write(
            STDERR_FILENO,
            message,
            sizeof(message) - 1u
        );
    }


    monitor_internal = 0;
}


int pthread_mutex_lock(
    pthread_mutex_t *mutex
)
{
  
    if (real_pthread_mutex_lock == NULL) {

        monitor_internal = 1;

        const bool resolved =
            resolve_real_functions();

        monitor_internal = 0;


        if (!resolved ||
            real_pthread_mutex_lock == NULL) {

            _exit(127);
        }
    }


    /*
     * Bypass monitoring for recursive/internal monitor activity.
     */
    if (monitor_internal ||
        monitor_runtime_is_internal_thread()) {

        return real_pthread_mutex_lock(
            mutex
        );
    }


    /*
     * --------------------------------------------------------------
     * LOCK_REQUEST
     * --------------------------------------------------------------
     */

    monitor_internal = 1;


    debug_event(
        "LOCK_REQUEST",
        mutex,
        0,
        false
    );


    monitor_runtime_emit(
        MONITOR_EVENT_LOCK_REQUEST,
        (uintptr_t)mutex,
        0
    );


    monitor_internal = 0;


    /*
     * The real call may block here.
     */
    const int rc =
        real_pthread_mutex_lock(
            mutex
        );


    /*
     * --------------------------------------------------------------
     * LOCK_ACQUIRED / LOCK_FAILED
     * --------------------------------------------------------------
     */

    monitor_internal = 1;


    if (rc == 0) {

        debug_event(
            "LOCK_ACQUIRED",
            mutex,
            rc,
            true
        );


        monitor_runtime_emit(
            MONITOR_EVENT_LOCK_ACQUIRED,
            (uintptr_t)mutex,
            rc
        );

    } else {

        debug_event(
            "LOCK_FAILED",
            mutex,
            rc,
            true
        );


        monitor_runtime_emit(
            MONITOR_EVENT_LOCK_FAILED,
            (uintptr_t)mutex,
            rc
        );
    }


    monitor_internal = 0;


    return rc;
}



int pthread_mutex_unlock(
    pthread_mutex_t *mutex
)
{
    if (real_pthread_mutex_unlock == NULL) {

        monitor_internal = 1;

        const bool resolved =
            resolve_real_functions();

        monitor_internal = 0;


        if (!resolved ||
            real_pthread_mutex_unlock == NULL) {

            _exit(127);
        }
    }


    if (monitor_internal ||
        monitor_runtime_is_internal_thread()) {

        return real_pthread_mutex_unlock(
            mutex
        );
    }



    const int rc =
        real_pthread_mutex_unlock(
            mutex
        );


    monitor_internal = 1;


    if (rc == 0) {

        debug_event(
            "UNLOCKED",
            mutex,
            rc,
            true
        );


        monitor_runtime_emit(
            MONITOR_EVENT_UNLOCKED,
            (uintptr_t)mutex,
            rc
        );

    } else {

        debug_event(
            "UNLOCK_FAILED",
            mutex,
            rc,
            true
        );


        monitor_runtime_emit(
            MONITOR_EVENT_UNLOCK_FAILED,
            (uintptr_t)mutex,
            rc
        );
    }


    monitor_internal = 0;


    return rc;
}


/*
 * ==========================================================================
 * Library shutdown
 * ==========================================================================
 */

__attribute__((destructor))
static void monitor_library_destroy(void)
{
    /*
     * Nothing performed during shutdown should enter
     * the application event stream.
     */
    monitor_internal = 1;

    monitor_debug_enabled = false;


    if (runtime_started) {

        monitor_runtime_stop();

        runtime_started = false;
    }
}