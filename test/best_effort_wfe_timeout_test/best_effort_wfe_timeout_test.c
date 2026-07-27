/*
 * Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Reproduces a lost wake-up in best_effort_wfe_or_timeout().
//
// The function caches the timestamp of the last alarm it added and, when called again
// with the same timestamp, does a bare __wfe() on the assumption that the alarm added
// earlier still guarantees a wake-up no later than that time. The alarm is one-shot:
// once it has fired, and the default pool is otherwise empty, nothing is armed. If the
// deadline is already in the past when the function is re-entered, the __wfe() has no
// wake-up source and the core sleeps until some unrelated event arrives - potentially
// forever on a quiet system.
//
// This is how it bites in the field: sem_acquire_block_until() calls
// best_effort_wfe_or_timeout() in a loop with the same deadline. Any caller that can
// reach that loop with an already-expired deadline (e.g. async_context_poll's
// wait_for_work_until clamped to a stale worker time from within the cyw43 driver's
// CYW43_DO_IOCTL_WAIT) parks the core in an unwakeable WFE.
//
// A correct implementation returns true immediately for an expired deadline. The
// output ends with "TEST PASSED"; on a broken SDK the output stops after a "call N"
// line instead - the first repeat call or two may return via an event left latched in
// the event register by earlier IRQs, after which the core hangs in __wfe().
//
// Requires the default alarm pool (the PICO_TIME_DEFAULT_ALARM_POOL_DISABLED fallback
// busy-waits and does not have this bug). Run without a debugger attached: halting the
// core is itself a WFE wake-up event.

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/sync.h"

int main(void) {
    stdio_init_all();
    printf("best_effort_wfe_or_timeout lost wake-up test\n");

    // Run one wait to completion so the deadline is cached inside
    // best_effort_wfe_or_timeout() and its one-shot alarm has fired, leaving the
    // default alarm pool empty and the hardware alarm disarmed.
    absolute_time_t deadline = make_timeout_time_ms(100);
    while (!best_effort_wfe_or_timeout(deadline)) tight_loop_contents();
    hard_assert(time_reached(deadline));
    printf("deadline %lld us cached and expired\n", (long long)to_us_since_boot(deadline));

    // Wait on the same, now expired, deadline again. Several iterations are needed to
    // drain any latched events before the hang shows itself.
    for (int i = 1; i <= 5; i++) {
        printf("call %d\n", i);
        bool reached = best_effort_wfe_or_timeout(deadline);
        hard_assert(reached);
    }

    // The same hole via the path seen in the field.
    printf("sem_acquire_block_until on the expired deadline\n");
    semaphore_t sem;
    sem_init(&sem, 0, 1);
    bool acquired = sem_acquire_block_until(&sem, deadline);
    hard_assert(!acquired);

    printf("TEST PASSED\n");
    return 0;
}
