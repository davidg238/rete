/* Copyright (c) 2026 Ekorau LLC */
#ifndef OEVM_OBSERVER_H
#define OEVM_OBSERVER_H
/* The wedge observer's two entry points (rete#215), defined in zephyr/src/observer.c;
 * the decision it acts on is observer_core.h. Both symbols link on EVERY Zephyr board:
 * a board without CONFIG_TASK_WDT gets bodies that say so, never a missing definition. */

/* Start the observer thread and the hardware watchdog, once at boot. */
void observer_start(void);

/* Disable the hardware watchdog and park: 3 consecutive faults with no check-in between.
 * Safe from the fatal handler in ISR context. Does not return. */
void observer_halt(void);

#endif
