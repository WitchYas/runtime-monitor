#ifndef MONITOR_VERIFIER_H
#define MONITOR_VERIFIER_H

#include <stdbool.h>

#include "monitor_event.h"


void monitor_verifier_init(void);

void monitor_verifier_process_event(
    const monitor_event_t *event
);

void monitor_verifier_check_deadlocks(void);

void monitor_verifier_print_summary(
    bool trace_complete
);


#endif