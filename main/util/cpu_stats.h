#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tier 0 resource diagnostics: periodic per-core IDLE% logging (v2).
//
// Every CPU_STATS_PERIOD_MS a background task reads the two idle tasks'
// run-time counters (ulTaskGetIdleRunTimeCounterForCore — a single TCB field
// read, O(1)) and logs the per-core idle percentage of the last window into
// the always-on diag ring (one line: "cpu: idle0 X% idle1 Y%").
//
// v1 logged a full per-task table via uxTaskGetSystemState() — REMOVED
// (2026-07-13): that call byte-walks every task's stack for the watermark
// inside a kernel-lock critical section (several stacks are 64 KB, in
// PSRAM), a multi-ms interrupts-off window every period. It delayed the
// core-0 MIPI-DSI frame-restart ISR past the blanking window and blanked the
// panel for one frame (the FT4 "full-screen cyan flash") — and core-pinning
// does NOT contain it (hardware-verified: lock contention propagates the
// ints-off window to the other core). Idle% is the headline number anyway;
// the per-task breakdown remains available on-demand via the dev-only resmon
// (POST /api/cmd {resmon}), where a rare one-frame blink is acceptable.
//
// Motivation: two core-0 CPU hogs (the priority-6 CW-audio ghost task, the
// FT8-mode panadapter render waste) each cost multiple releases to find
// because there was no CPU accounting on the device. Requires
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS. Counters are 32-bit µs and wrap
// every ~71 min; unsigned delta arithmetic makes that harmless.
esp_err_t cpu_stats_init(void);

/* The latest per-core IDLE sample, for the on-screen CPU readout.
 *
 * Values are in TENTHS of a percent (855 == 85.5% idle), clamped to 0..1000,
 * and describe the last COMPLETED CPU_STATS_PERIOD_MS window - not "now". A
 * caller that repaints faster than that must use the sequence number, or it
 * will redraw the same digits as though they were fresh.
 *
 * @param tenths0  out: core 0 idle, tenths of a percent (may be NULL)
 * @param tenths1  out: core 1 idle, tenths of a percent (may be NULL)
 * @param seq      out: increments once per completed sample; 0 means none has
 *                 been taken yet, which is the only time the numbers mean
 *                 nothing. May be NULL.
 *
 * Cheap by construction: the sampler is O(1) counter reads (the whole reason
 * v2 dropped uxTaskGetSystemState()), this is three plain loads, and there is
 * deliberately no lock - the reader is displaying a 10 s average, for which a
 * torn read is not a hazard. */
void cpu_stats_get_idle(uint16_t *tenths0, uint16_t *tenths1, uint32_t *seq);

#ifdef __cplusplus
}
#endif
