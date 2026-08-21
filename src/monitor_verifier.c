#include "monitor_verifier.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define VERIFIER_MAX_THREADS              16u
#define VERIFIER_MAX_MUTEXES              128u
#define VERIFIER_MAX_HELD_MUTEXES         32u

#define DEADLOCK_CONFIRMATIONS_REQUIRED   2u

#define DEFAULT_MAX_HOLD_NS \
    100000000ULL


/*
 * ==========================================================================
 * Held mutex
 * ==========================================================================
 */

typedef struct {

    uintptr_t address;

    uint64_t first_acquired_timestamp_ns;

    uint32_t depth;

} verifier_held_mutex_t;


/*
 * ==========================================================================
 * Thread state
 * ==========================================================================
 */

typedef struct {

    bool used;

    bool exited;

    bool trace_tainted;


    bool sequence_initialized;


    uint64_t thread_id;


    uint32_t next_expected_sequence;


    uintptr_t waiting_mutex;

    uint64_t waiting_sequence;


    verifier_held_mutex_t
        held_mutexes[VERIFIER_MAX_HELD_MUTEXES];


    uint32_t held_count;

} verifier_thread_state_t;


/*
 * ==========================================================================
 * Mutex state
 * ==========================================================================
 */

typedef struct {

    bool used;


    /*
     * False means ownership cannot currently be trusted.
     */
    bool owner_known;


    uintptr_t address;


    uint64_t owner_thread_id;


    /*
     * Greatest ownership-changing global sequence accepted.
     *
     * This protects current mutex state from cross-queue
     * out-of-order consumption.
     */
    uint64_t last_state_sequence;

} verifier_mutex_state_t;


/*
 * ==========================================================================
 * Tables
 * ==========================================================================
 */

static verifier_thread_state_t
    thread_states[VERIFIER_MAX_THREADS];


static verifier_mutex_state_t
    mutex_states[VERIFIER_MAX_MUTEXES];


/*
 * ==========================================================================
 * Statistics
 * ==========================================================================
 */

static uint64_t processed_events = 0;

static uint64_t deadlock_cycle_violations = 0;

static uint64_t non_owner_unlock_violations = 0;

static uint64_t self_deadlock_candidates = 0;

static uint64_t lock_held_at_exit_violations = 0;

static uint64_t hold_time_violations = 0;

static uint64_t state_overflow_events = 0;

static uint64_t detected_stream_gaps = 0;

static uint32_t tainted_thread_streams = 0;


/*
 * ==========================================================================
 * Configuration
 * ==========================================================================
 */

static uint64_t max_hold_ns =
    DEFAULT_MAX_HOLD_NS;


/*
 * ==========================================================================
 * Deadlock state
 * ==========================================================================
 */

static uint32_t waiting_thread_count = 0;

static uint32_t candidate_cycle_mask = 0;

static uint32_t candidate_cycle_confirmations = 0;

static uint32_t reported_cycle_mask = 0;

static uint64_t last_processed_sequence = 0;


/*
 * ==========================================================================
 * Configuration
 * ==========================================================================
 */

static void load_configuration(void)
{
    max_hold_ns =
        DEFAULT_MAX_HOLD_NS;


    const char *value =
        getenv(
            "MONITOR_MAX_HOLD_NS"
        );


    if (value == NULL ||
        value[0] == '\0') {

        return;
    }


    errno = 0;

    char *end = NULL;


    const unsigned long long parsed =
        strtoull(
            value,
            &end,
            10
        );


    if (errno != 0 ||
        end == value ||
        *end != '\0') {

        return;
    }


    max_hold_ns =
        (uint64_t)parsed;
}


/*
 * ==========================================================================
 * Thread lookup
 * ==========================================================================
 */

static verifier_thread_state_t *
find_thread(
    uint64_t thread_id,
    bool create
)
{
    verifier_thread_state_t *free_slot =
        NULL;


    for (uint32_t i = 0;
         i < VERIFIER_MAX_THREADS;
         ++i) {

        verifier_thread_state_t *state =
            &thread_states[i];


        if (state->used) {

            if (state->thread_id ==
                thread_id) {

                return state;
            }

        } else if (free_slot == NULL) {

            free_slot =
                state;
        }
    }


    if (!create) {
        return NULL;
    }


    if (free_slot == NULL) {

        ++state_overflow_events;

        return NULL;
    }


    memset(
        free_slot,
        0,
        sizeof(*free_slot)
    );


    free_slot->used =
        true;


    free_slot->thread_id =
        thread_id;


    return free_slot;
}


/*
 * ==========================================================================
 * Mutex lookup
 * ==========================================================================
 */

static verifier_mutex_state_t *
find_mutex(
    uintptr_t address,
    bool create
)
{
    verifier_mutex_state_t *free_slot =
        NULL;


    for (uint32_t i = 0;
         i < VERIFIER_MAX_MUTEXES;
         ++i) {

        verifier_mutex_state_t *state =
            &mutex_states[i];


        if (state->used) {

            if (state->address ==
                address) {

                return state;
            }

        } else if (free_slot == NULL) {

            free_slot =
                state;
        }
    }


    if (!create) {
        return NULL;
    }


    if (free_slot == NULL) {

        ++state_overflow_events;

        return NULL;
    }


    memset(
        free_slot,
        0,
        sizeof(*free_slot)
    );


    free_slot->used =
        true;


    /*
     * Seeing an address alone does not establish whether
     * it is owned.
     */
    free_slot->owner_known =
        false;


    free_slot->address =
        address;


    return free_slot;
}


/*
 * ==========================================================================
 * Thread index
 * ==========================================================================
 */

static int thread_index(
    uint64_t thread_id
)
{
    for (uint32_t i = 0;
         i < VERIFIER_MAX_THREADS;
         ++i) {

        if (thread_states[i].used &&
            thread_states[i].thread_id ==
                thread_id) {

            return (int)i;
        }
    }


    return -1;
}


/*
 * ==========================================================================
 * Stream-gap handling
 * ==========================================================================
 */

static void invalidate_mutexes_owned_by_thread(
    uint64_t thread_id,
    uint64_t barrier_sequence
)
{
    for (uint32_t i = 0;
         i < VERIFIER_MAX_MUTEXES;
         ++i) {

        verifier_mutex_state_t *mutex =
            &mutex_states[i];


        if (!mutex->used) {
            continue;
        }


        if (!mutex->owner_known) {
            continue;
        }


        if (mutex->owner_thread_id !=
            thread_id) {

            continue;
        }


        /*
         * Missing producer events mean we can no longer prove
         * this ownership state.
         */
        mutex->owner_known =
            false;


        mutex->owner_thread_id =
            0;


        /*
         * Do not permit older cross-queue events to resurrect
         * state that predates the observed gap.
         *
         * The current event itself may re-establish state because
         * handlers accept equality.
         */
        if (barrier_sequence >
            mutex->last_state_sequence) {

            mutex->last_state_sequence =
                barrier_sequence;
        }
    }
}


static void mark_thread_stream_tainted(
    verifier_thread_state_t *thread,
    uint64_t barrier_sequence
)
{
    if (thread->trace_tainted) {
        return;
    }


    thread->trace_tainted =
        true;


    ++tainted_thread_streams;


    /*
     * Any state relying on the missing events is discarded.
     */
    if (thread->waiting_mutex != 0) {

        thread->waiting_mutex = 0;

        thread->waiting_sequence = 0;


        if (waiting_thread_count != 0) {

            --waiting_thread_count;
        }
    }


    memset(
        thread->held_mutexes,
        0,
        sizeof(thread->held_mutexes)
    );


    thread->held_count =
        0;


    invalidate_mutexes_owned_by_thread(
        thread->thread_id,
        barrier_sequence
    );
}


/*
 * Observe one event's local producer sequence.
 *
 * This happens BEFORE the event mutates verifier state.
 */

static verifier_thread_state_t *
observe_thread_sequence(
    const monitor_event_t *event
)
{
    verifier_thread_state_t *thread =
        find_thread(
            event->thread_id,
            true
        );


    if (thread == NULL) {
        return NULL;
    }


    if (!thread->sequence_initialized) {

        thread->sequence_initialized =
            true;


        /*
         * THREAD_START should normally have sequence zero.
         *
         * If the first delivered event is already later than zero,
         * at least one event was lost.
         */
        if (event->thread_sequence != 0u) {

            ++detected_stream_gaps;


            mark_thread_stream_tainted(
                thread,
                event->sequence_id
            );
        }


        thread->next_expected_sequence =
            event->thread_sequence + 1u;


        return thread;
    }


    if (event->thread_sequence !=
        thread->next_expected_sequence) {

        ++detected_stream_gaps;


        mark_thread_stream_tainted(
            thread,
            event->sequence_id
        );
    }


    thread->next_expected_sequence =
        event->thread_sequence + 1u;


    return thread;
}


/*
 * ==========================================================================
 * Held mutex lookup
 * ==========================================================================
 */

static verifier_held_mutex_t *
find_held_mutex(
    verifier_thread_state_t *thread,
    uintptr_t address
)
{
    for (uint32_t i = 0;
         i < thread->held_count;
         ++i) {

        if (thread->held_mutexes[i].address ==
            address) {

            return
                &thread->held_mutexes[i];
        }
    }


    return NULL;
}


/*
 * ==========================================================================
 * Held mutex acquisition
 * ==========================================================================
 */

static void add_held_mutex(
    verifier_thread_state_t *thread,
    uintptr_t address,
    uint64_t timestamp_ns
)
{
    /*
     * Once a stream has a gap, held-lock state is incomplete.
     *
     * Do not reconstruct partial state and then make assertions
     * from it.
     */
    if (thread->trace_tainted) {
        return;
    }


    verifier_held_mutex_t *held =
        find_held_mutex(
            thread,
            address
        );


    if (held != NULL) {

        if (held->depth <
            UINT32_MAX) {

            ++held->depth;
        }


        return;
    }


    if (thread->held_count >=
        VERIFIER_MAX_HELD_MUTEXES) {

        ++state_overflow_events;

        return;
    }


    held =
        &thread->held_mutexes[
            thread->held_count
        ];


    held->address =
        address;


    held->first_acquired_timestamp_ns =
        timestamp_ns;


    held->depth =
        1u;


    ++thread->held_count;
}


/*
 * ==========================================================================
 * Violation output
 * ==========================================================================
 */

static void report_self_deadlock_candidate(
    const monitor_event_t *event
)
{
    ++self_deadlock_candidates;


    char buffer[768];


    const int length =
        snprintf(
            buffer,
            sizeof(buffer),

            "\n"
            "========================================\n"
            "[monitor][VIOLATION] SELF_DEADLOCK_CANDIDATE\n"
            "sequence=%lu\n"
            "thread=T%lu\n"
            "mutex=%p\n"
            "reason=thread requested a mutex it already holds\n"
            "========================================\n",

            (unsigned long)
                event->sequence_id,

            (unsigned long)
                event->thread_id,

            (void *)
                event->object_address
        );


    if (length <= 0) {
        return;
    }


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


static void report_non_owner_unlock(
    const monitor_event_t *event
)
{
    ++non_owner_unlock_violations;


    char buffer[768];


    const int length =
        snprintf(
            buffer,
            sizeof(buffer),

            "\n"
            "========================================\n"
            "[monitor][VIOLATION] NON_OWNER_UNLOCK\n"
            "sequence=%lu\n"
            "thread=T%lu\n"
            "mutex=%p\n"
            "pthread_result=%d (EPERM)\n"
            "reason=pthread rejected unlock because caller did not own mutex\n"
            "========================================\n",

            (unsigned long)
                event->sequence_id,

            (unsigned long)
                event->thread_id,

            (void *)
                event->object_address,

            event->result
        );


    if (length <= 0) {
        return;
    }


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


static void report_hold_time_violation(
    const monitor_event_t *event,
    uint64_t held_ns
)
{
    ++hold_time_violations;


    char buffer[768];


    const int length =
        snprintf(
            buffer,
            sizeof(buffer),

            "\n"
            "========================================\n"
            "[monitor][VIOLATION] HOLD_TIME_VIOLATION\n"
            "sequence=%lu\n"
            "thread=T%lu\n"
            "mutex=%p\n"
            "held_ns=%lu\n"
            "threshold_ns=%lu\n"
            "========================================\n",

            (unsigned long)
                event->sequence_id,

            (unsigned long)
                event->thread_id,

            (void *)
                event->object_address,

            (unsigned long)
                held_ns,

            (unsigned long)
                max_hold_ns
        );


    if (length <= 0) {
        return;
    }


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


/*
 * ==========================================================================
 * Mutex release
 * ==========================================================================
 */

static bool release_held_mutex(
    verifier_thread_state_t *thread,
    const monitor_event_t *event
)
{
    /*
     * State-based duration checking is unsafe after a stream gap.
     */
    if (thread->trace_tainted) {

        return true;
    }


    verifier_held_mutex_t *held =
        find_held_mutex(
            thread,
            event->object_address
        );


    if (held == NULL) {
        return true;
    }


    if (held->depth > 1u) {

        --held->depth;

        return false;
    }


    uint64_t held_ns = 0;


    if (event->timestamp_ns >=
        held->first_acquired_timestamp_ns) {

        held_ns =
            event->timestamp_ns -
            held->first_acquired_timestamp_ns;
    }


    if (max_hold_ns != 0 &&
        held_ns > max_hold_ns) {

        report_hold_time_violation(
            event,
            held_ns
        );
    }


    const uint32_t index =
        (uint32_t)(
            held -
            thread->held_mutexes
        );


    const uint32_t last =
        thread->held_count - 1u;


    thread->held_mutexes[index] =
        thread->held_mutexes[last];


    memset(
        &thread->held_mutexes[last],
        0,
        sizeof(
            thread->held_mutexes[last]
        )
    );


    --thread->held_count;


    return true;
}


/*
 * ==========================================================================
 * Lock held at thread exit
 * ==========================================================================
 */

static void report_lock_held_at_thread_exit(
    const monitor_event_t *event,
    const verifier_thread_state_t *thread
)
{
    ++lock_held_at_exit_violations;


    char buffer[2048];

    size_t position = 0;


    int written =
        snprintf(
            buffer,
            sizeof(buffer),

            "\n"
            "========================================\n"
            "[monitor][VIOLATION] LOCK_HELD_AT_THREAD_EXIT\n"
            "sequence=%lu\n"
            "thread=T%lu\n"
            "held_mutex_count=%u\n",

            (unsigned long)
                event->sequence_id,

            (unsigned long)
                event->thread_id,

            thread->held_count
        );


    if (written <= 0) {
        return;
    }


    position =
        (size_t)written;


    if (position >=
        sizeof(buffer)) {

        position =
            sizeof(buffer) - 1u;
    }


    for (uint32_t i = 0;
         i < thread->held_count;
         ++i) {

        if (position >=
            sizeof(buffer) - 1u) {

            break;
        }


        written =
            snprintf(
                buffer + position,
                sizeof(buffer) - position,

                "  mutex=%p depth=%u\n",

                (void *)
                    thread->held_mutexes[i].address,

                thread->held_mutexes[i].depth
            );


        if (written <= 0) {
            break;
        }


        const size_t available =
            sizeof(buffer) -
            position;


        if ((size_t)written >=
            available) {

            position =
                sizeof(buffer) - 1u;

            break;
        }


        position +=
            (size_t)written;
    }


    if (position <
        sizeof(buffer) - 1u) {

        written =
            snprintf(
                buffer + position,
                sizeof(buffer) - position,

                "========================================\n"
            );


        if (written > 0) {

            const size_t available =
                sizeof(buffer) -
                position;


            if ((size_t)written >=
                available) {

                position =
                    sizeof(buffer) - 1u;

            } else {

                position +=
                    (size_t)written;
            }
        }
    }


    (void)write(
        STDERR_FILENO,
        buffer,
        position
    );
}


/*
 * ==========================================================================
 * Waiting state
 * ==========================================================================
 */

static void set_thread_waiting(
    verifier_thread_state_t *thread,
    uintptr_t address,
    uint64_t sequence
)
{
    if (thread->waiting_mutex == 0) {

        ++waiting_thread_count;
    }


    thread->waiting_mutex =
        address;


    thread->waiting_sequence =
        sequence;
}


static void clear_thread_waiting(
    verifier_thread_state_t *thread,
    uintptr_t address
)
{
    if (thread->waiting_mutex !=
        address) {

        return;
    }


    thread->waiting_mutex =
        0;


    thread->waiting_sequence =
        0;


    if (waiting_thread_count != 0) {

        --waiting_thread_count;
    }
}


/*
 * ==========================================================================
 * Wait-for graph
 * ==========================================================================
 */

static bool find_cycle_from(
    uint64_t start_thread_id,
    uint64_t *cycle_threads,
    uintptr_t *cycle_mutexes,
    uint32_t *cycle_length,
    uint32_t *cycle_mask
)
{
    uint64_t current_thread_id =
        start_thread_id;


    uint64_t visited[
        VERIFIER_MAX_THREADS
    ] = {0};


    uint32_t visited_count =
        0;


    for (uint32_t step = 0;
         step < VERIFIER_MAX_THREADS;
         ++step) {

        verifier_thread_state_t *thread =
            find_thread(
                current_thread_id,
                false
            );


        if (thread == NULL ||
            thread->trace_tainted ||
            thread->exited) {

            return false;
        }


        if (thread->waiting_mutex == 0) {
            return false;
        }


        verifier_mutex_state_t *mutex =
            find_mutex(
                thread->waiting_mutex,
                false
            );


        if (mutex == NULL ||
            !mutex->owner_known ||
            mutex->owner_thread_id == 0) {

            return false;
        }


        const uint64_t owner =
            mutex->owner_thread_id;


        verifier_thread_state_t *owner_thread =
            find_thread(
                owner,
                false
            );


        /*
         * A cycle involving an incomplete stream cannot be
         * asserted safely.
         */
        if (owner_thread == NULL ||
            owner_thread->trace_tainted ||
            owner_thread->exited) {

            return false;
        }


        cycle_threads[step] =
            current_thread_id;


        cycle_mutexes[step] =
            thread->waiting_mutex;


        visited[visited_count] =
            current_thread_id;


        ++visited_count;


        if (owner ==
            start_thread_id) {

            const uint32_t length =
                step + 1u;


            if (length < 2u) {
                return false;
            }


            uint32_t mask = 0;


            for (uint32_t i = 0;
                 i < length;
                 ++i) {

                const int index =
                    thread_index(
                        cycle_threads[i]
                    );


                if (index >= 0 &&
                    index < 32) {

                    mask |=
                        (
                            1u <<
                            (uint32_t)index
                        );
                }
            }


            *cycle_length =
                length;


            *cycle_mask =
                mask;


            return true;
        }


        for (uint32_t i = 0;
             i < visited_count;
             ++i) {

            if (visited[i] ==
                owner) {

                return false;
            }
        }


        current_thread_id =
            owner;
    }


    return false;
}


static bool find_any_cycle(
    uint64_t *cycle_threads,
    uintptr_t *cycle_mutexes,
    uint32_t *cycle_length,
    uint32_t *cycle_mask
)
{
    for (uint32_t i = 0;
         i < VERIFIER_MAX_THREADS;
         ++i) {

        if (!thread_states[i].used ||
            thread_states[i].trace_tainted ||
            thread_states[i].waiting_mutex == 0) {

            continue;
        }


        if (find_cycle_from(
                thread_states[i].thread_id,
                cycle_threads,
                cycle_mutexes,
                cycle_length,
                cycle_mask)) {

            return true;
        }
    }


    return false;
}


/*
 * ==========================================================================
 * Deadlock report
 * ==========================================================================
 */

static void report_deadlock_cycle(
    const uint64_t *cycle_threads,
    const uintptr_t *cycle_mutexes,
    uint32_t cycle_length
)
{
    ++deadlock_cycle_violations;


    char buffer[2048];

    size_t position = 0;


    int written =
        snprintf(
            buffer,
            sizeof(buffer),

            "\n"
            "========================================\n"
            "[monitor][VIOLATION] DEADLOCK_CYCLE\n"
            "observed_after_seq=%lu\n"
            "cycle_length=%u\n",

            (unsigned long)
                last_processed_sequence,

            cycle_length
        );


    if (written <= 0) {
        return;
    }


    position =
        (size_t)written;


    if (position >=
        sizeof(buffer)) {

        position =
            sizeof(buffer) - 1u;
    }


    for (uint32_t i = 0;
         i < cycle_length;
         ++i) {

        verifier_mutex_state_t *mutex =
            find_mutex(
                cycle_mutexes[i],
                false
            );


        const uint64_t owner =
            (
                mutex != NULL &&
                mutex->owner_known
            )
            ?
                mutex->owner_thread_id
            :
                0;


        if (position >=
            sizeof(buffer) - 1u) {

            break;
        }


        written =
            snprintf(
                buffer + position,
                sizeof(buffer) - position,

                "  T%lu waits for mutex=%p "
                "owned by T%lu\n",

                (unsigned long)
                    cycle_threads[i],

                (void *)
                    cycle_mutexes[i],

                (unsigned long)
                    owner
            );


        if (written <= 0) {
            break;
        }


        const size_t available =
            sizeof(buffer) -
            position;


        if ((size_t)written >=
            available) {

            position =
                sizeof(buffer) - 1u;

            break;
        }


        position +=
            (size_t)written;
    }


    if (position <
        sizeof(buffer) - 1u) {

        written =
            snprintf(
                buffer + position,
                sizeof(buffer) - position,

                "========================================\n"
            );


        if (written > 0) {

            const size_t available =
                sizeof(buffer) -
                position;


            if ((size_t)written >=
                available) {

                position =
                    sizeof(buffer) - 1u;

            } else {

                position +=
                    (size_t)written;
            }
        }
    }


    (void)write(
        STDERR_FILENO,
        buffer,
        position
    );
}


/*
 * ==========================================================================
 * Event handlers
 * ==========================================================================
 */

static void handle_thread_start(
    const monitor_event_t *event
)
{
    verifier_thread_state_t *thread =
        find_thread(
            event->thread_id,
            true
        );


    if (thread != NULL) {

        thread->exited =
            false;
    }
}


static void handle_thread_exit(
    const monitor_event_t *event
)
{
    verifier_thread_state_t *thread =
        find_thread(
            event->thread_id,
            true
        );


    if (thread == NULL) {
        return;
    }


    /*
     * Held-lock exit is reported only from a complete producer stream.
     */
    if (!thread->trace_tainted &&
        thread->held_count != 0) {

        report_lock_held_at_thread_exit(
            event,
            thread
        );
    }


    if (thread->waiting_mutex != 0) {

        clear_thread_waiting(
            thread,
            thread->waiting_mutex
        );
    }


    thread->exited =
        true;
}


static void handle_lock_request(
    const monitor_event_t *event
)
{
    verifier_thread_state_t *thread =
        find_thread(
            event->thread_id,
            true
        );


    if (thread == NULL) {
        return;
    }


    (void)find_mutex(
        event->object_address,
        true
    );


    /*
     * Critically, never infer self-deadlock from an incomplete
     * producer stream.
     */
    if (!thread->trace_tainted) {

        if (find_held_mutex(
                thread,
                event->object_address) != NULL) {

            report_self_deadlock_candidate(
                event
            );
        }
    }


    set_thread_waiting(
        thread,
        event->object_address,
        event->sequence_id
    );
}


static void handle_lock_acquired(
    const monitor_event_t *event
)
{
    verifier_thread_state_t *thread =
        find_thread(
            event->thread_id,
            true
        );


    verifier_mutex_state_t *mutex =
        find_mutex(
            event->object_address,
            true
        );


    if (thread == NULL ||
        mutex == NULL) {

        return;
    }


    clear_thread_waiting(
        thread,
        event->object_address
    );


    add_held_mutex(
        thread,
        event->object_address,
        event->timestamp_ns
    );


    /*
     * Keep the newest ownership-changing global sequence only.
     *
     * This makes current mutex state independent of round-robin
     * cross-queue consumption order.
     */
    if (event->sequence_id >=
        mutex->last_state_sequence) {

        mutex->owner_known =
            true;


        mutex->owner_thread_id =
            event->thread_id;


        mutex->last_state_sequence =
            event->sequence_id;
    }
}


static void handle_lock_failed(
    const monitor_event_t *event
)
{
    verifier_thread_state_t *thread =
        find_thread(
            event->thread_id,
            false
        );


    if (thread == NULL) {
        return;
    }


    clear_thread_waiting(
        thread,
        event->object_address
    );
}


static void handle_unlocked(
    const monitor_event_t *event
)
{
    verifier_thread_state_t *thread =
        find_thread(
            event->thread_id,
            false
        );


    verifier_mutex_state_t *mutex =
        find_mutex(
            event->object_address,
            true
        );


    bool fully_released =
        true;


    if (thread != NULL) {

        fully_released =
            release_held_mutex(
                thread,
                event
            );
    }


    if (mutex == NULL) {
        return;
    }


    if (event->sequence_id >=
        mutex->last_state_sequence) {

        mutex->owner_known =
            true;


        mutex->owner_thread_id =
            fully_released
            ?
                0
            :
                event->thread_id;


        mutex->last_state_sequence =
            event->sequence_id;
    }
}


static void handle_unlock_failed(
    const monitor_event_t *event
)
{
    /*
     * For the ERRORCHECK/robust mutex semantics used by our
     * deterministic target, EPERM directly establishes that the
     * calling thread did not own the mutex.
     *
     * No historical ownership scan is required.
     */
    if (event->result ==
        EPERM) {

        report_non_owner_unlock(
            event
        );
    }
}


/*
 * ==========================================================================
 * Initialization
 * ==========================================================================
 */

void monitor_verifier_init(void)
{
    memset(
        thread_states,
        0,
        sizeof(thread_states)
    );


    memset(
        mutex_states,
        0,
        sizeof(mutex_states)
    );


    processed_events =
        0;


    deadlock_cycle_violations =
        0;


    non_owner_unlock_violations =
        0;


    self_deadlock_candidates =
        0;


    lock_held_at_exit_violations =
        0;


    hold_time_violations =
        0;


    state_overflow_events =
        0;


    detected_stream_gaps =
        0;


    tainted_thread_streams =
        0;


    waiting_thread_count =
        0;


    candidate_cycle_mask =
        0;


    candidate_cycle_confirmations =
        0;


    reported_cycle_mask =
        0;


    last_processed_sequence =
        0;


    load_configuration();
}


/*
 * ==========================================================================
 * Event processing
 * ==========================================================================
 */

void monitor_verifier_process_event(
    const monitor_event_t *event
)
{
    if (event == NULL) {
        return;
    }


    ++processed_events;


    if (event->sequence_id >
        last_processed_sequence) {

        last_processed_sequence =
            event->sequence_id;
    }


    /*
     * Detect producer-stream loss BEFORE mutating L4 state.
     */
    if (observe_thread_sequence(
            event) == NULL) {

        return;
    }


    candidate_cycle_mask =
        0;


    candidate_cycle_confirmations =
        0;


    switch (
        (monitor_event_type_t)
            event->event_type
    ) {

        case MONITOR_EVENT_THREAD_START:

            handle_thread_start(
                event
            );

            break;


        case MONITOR_EVENT_THREAD_EXIT:

            handle_thread_exit(
                event
            );

            break;


        case MONITOR_EVENT_LOCK_REQUEST:

            handle_lock_request(
                event
            );

            break;


        case MONITOR_EVENT_LOCK_ACQUIRED:

            handle_lock_acquired(
                event
            );

            break;


        case MONITOR_EVENT_LOCK_FAILED:

            handle_lock_failed(
                event
            );

            break;


        case MONITOR_EVENT_UNLOCKED:

            handle_unlocked(
                event
            );

            break;


        case MONITOR_EVENT_UNLOCK_FAILED:

            handle_unlock_failed(
                event
            );

            break;


        case MONITOR_EVENT_UNLOCK_REQUEST:
        case MONITOR_EVENT_INVALID:
        default:

            break;
    }
}


/*
 * ==========================================================================
 * Deadlock check
 * ==========================================================================
 */

void monitor_verifier_check_deadlocks(void)
{
    if (waiting_thread_count <
        2u) {

        candidate_cycle_mask =
            0;


        candidate_cycle_confirmations =
            0;


        reported_cycle_mask =
            0;


        return;
    }


    uint64_t cycle_threads[
        VERIFIER_MAX_THREADS
    ] = {0};


    uintptr_t cycle_mutexes[
        VERIFIER_MAX_THREADS
    ] = {0};


    uint32_t cycle_length =
        0;


    uint32_t cycle_mask =
        0;


    if (!find_any_cycle(
            cycle_threads,
            cycle_mutexes,
            &cycle_length,
            &cycle_mask)) {

        candidate_cycle_mask =
            0;


        candidate_cycle_confirmations =
            0;


        reported_cycle_mask =
            0;


        return;
    }


    if (candidate_cycle_mask !=
        cycle_mask) {

        candidate_cycle_mask =
            cycle_mask;


        candidate_cycle_confirmations =
            1u;


        return;
    }


    if (candidate_cycle_confirmations <
        UINT32_MAX) {

        ++candidate_cycle_confirmations;
    }


    if (candidate_cycle_confirmations <
        DEADLOCK_CONFIRMATIONS_REQUIRED) {

        return;
    }


    if (reported_cycle_mask ==
        cycle_mask) {

        return;
    }


    reported_cycle_mask =
        cycle_mask;


    report_deadlock_cycle(
        cycle_threads,
        cycle_mutexes,
        cycle_length
    );
}


/*
 * ==========================================================================
 * Summary
 * ==========================================================================
 */

void monitor_verifier_print_summary(
    bool trace_complete
)
{
    const uint64_t total_violations =
        deadlock_cycle_violations +
        non_owner_unlock_violations +
        self_deadlock_candidates +
        lock_held_at_exit_violations +
        hold_time_violations;


    const bool verifier_complete =
        state_overflow_events == 0 &&
        tainted_thread_streams == 0;


    const bool authoritative =
        trace_complete &&
        verifier_complete;


    char buffer[1792];


    const int length =
        snprintf(
            buffer,
            sizeof(buffer),

            "\n"
            "=== Runtime verifier summary ===\n"
            "Processed events              : %lu\n"
            "\n"
            "Violations:\n"
            "  DEADLOCK_CYCLE              : %lu\n"
            "  NON_OWNER_UNLOCK            : %lu\n"
            "  SELF_DEADLOCK_CANDIDATE     : %lu\n"
            "  LOCK_HELD_AT_THREAD_EXIT    : %lu\n"
            "  HOLD_TIME_VIOLATION         : %lu\n"
            "\n"
            "Total violations              : %lu\n"
            "Waiting threads               : %u\n"
            "Detected stream gaps          : %lu\n"
            "Tainted thread streams        : %u\n"
            "State overflow events         : %lu\n"
            "Hold-time threshold           : %lu ns\n"
            "Verifier state                : %s\n"
            "Interpretation                : %s\n",

            (unsigned long)
                processed_events,

            (unsigned long)
                deadlock_cycle_violations,

            (unsigned long)
                non_owner_unlock_violations,

            (unsigned long)
                self_deadlock_candidates,

            (unsigned long)
                lock_held_at_exit_violations,

            (unsigned long)
                hold_time_violations,

            (unsigned long)
                total_violations,

            waiting_thread_count,

            (unsigned long)
                detected_stream_gaps,

            tainted_thread_streams,

            (unsigned long)
                state_overflow_events,

            (unsigned long)
                max_hold_ns,

            verifier_complete
                ? "COMPLETE"
                : "DEGRADED",

            authoritative
                ? "AUTHORITATIVE"
                : "INCOMPLETE TRACE - conclusions are provisional"
        );


    if (length <= 0) {
        return;
    }


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