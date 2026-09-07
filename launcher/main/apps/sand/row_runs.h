/*=============================================================================
 * row_runs - finding and reconciling separate runs of occupied cells within
 * one row of a grid, and reporting only what genuinely changed.
 *
 * Pure logic, no grid type and no gfx dependency: a row is just a byte
 * array and a width, so this is testable on a host.
 *
 * WHY THIS EXISTS
 *
 * A caller that only ever reports one min/max span per row - "material
 * occupies somewhere between x0 and x1" - loses information the moment two
 * genuinely separate blobs share a row: the gap between them gets reported
 * as occupied too, and whatever consumes the report (gfx's own dirty
 * tracking) can never recover that gap, because it was never told about it
 * in the first place. Reporting up to ROW_MAX_RUNS separate runs instead
 * lets a consumer that CAN use the extra precision - gfx.c's per-cell
 * tracking and its own contiguous-run merging - actually benefit from it.
 *
 * THE RECONCILIATION PROBLEM
 *
 * A single previous/current min/max union is enough to guarantee a cell
 * that just emptied still gets sent once, clearing its stale pixels.
 * Moving to multiple runs per row turns that into a small diff between two
 * short run lists instead of one interval union - see row_runs_reconcile()
 * for the two rules that make it still safe.
 *===========================================================================*/
#pragma once

#include <stdint.h>

/* Fixed cap on how many separate runs one row tracks, for both detection
 * and reconciliation - a row can never grow arbitrarily many small sends.
 * A tunable needing real device measurement, same status GATHER_MAX_PIXELS
 * and LEAF_REFINE_MAX_RUNS in gfx.c had before their defaults were kept. */
#define ROW_MAX_RUNS 2

/* Collects `row`'s contiguous non-`empty` bytes into up to ROW_MAX_RUNS
 * [start,end) index ranges. Returns how many runs were found, or -1 if
 * there would have been more than ROW_MAX_RUNS - the caller should fall
 * back to row_runs_span_fallback() in that case. */
int row_runs_find(const uint8_t *row, int width, uint8_t empty,
                  int *run_x0, int *run_x1);

/* The plain single min/max span over the whole row - the fallback for a
 * row too fragmented for ROW_MAX_RUNS, and the original single-span
 * behaviour this module generalises. [max_x0,x1) is empty (x0==width,
 * x1==0) if the row has no non-`empty` bytes at all. */
void row_runs_span_fallback(const uint8_t *row, int width, uint8_t empty,
                            int *x0, int *x1);

/* Reconciles this frame's runs against last send, so a shrunk or
 * vanished run still sends enough to clear its old pixels: a current
 * run absorbs every previous run it overlaps; a run no current run
 * overlaps gets its own send range so its stale pixels clear. Simple
 * over tight - a pass can occasionally leave overlapping ranges rather
 * than fully re-merging, costing at most one extra small send, never a
 * dropped pixel, the property that matters. `send_x0`/`send_x1` need
 * room for cur_n + prev_n entries. */
int row_runs_reconcile(const uint16_t *cur_x0, const uint16_t *cur_x1,
                       int cur_n, const uint16_t *prev_x0,
                       const uint16_t *prev_x1, int prev_n,
                       uint16_t *send_x0, uint16_t *send_x1);
