#include "row_runs.h"

#include <stdbool.h>

int row_runs_find(const uint8_t *row, int width, uint8_t empty,
                  int *run_x0, int *run_x1)
{
    int n = 0;
    int cx = 0;

    while (cx < width) {
        if (row[cx] == empty) {
            cx++;
            continue;
        }
        if (n == ROW_MAX_RUNS) {
            return -1;
        }
        run_x0[n] = cx;
        while (cx < width && row[cx] != empty) {
            cx++;
        }
        run_x1[n] = cx;
        n++;
    }
    return n;
}

void row_runs_span_fallback(const uint8_t *row, int width, uint8_t empty,
                            int *x0, int *x1)
{
    int min_cx = width;
    int max_cx = -1;

    for (int cx = 0; cx < width; cx++) {
        if (row[cx] != empty) {
            if (cx < min_cx) { min_cx = cx; }
            max_cx = cx;
        }
    }
    *x0 = (max_cx >= 0) ? min_cx     : width;
    *x1 = (max_cx >= 0) ? max_cx + 1 : 0;
}

/* Extends [x0,x1) to also cover every previous run it overlaps, marking
 * each one absorbed in `prev_used` as it goes - the "current run absorbs
 * the previous runs it overlaps" half of row_runs_reconcile(), split out
 * to keep that function's own complexity down. */
static void absorb_overlapping(uint16_t *x0, uint16_t *x1,
                               const uint16_t *prev_x0,
                               const uint16_t *prev_x1, int prev_n,
                               bool *prev_used)
{
    for (int j = 0; j < prev_n; j++) {
        if (prev_x0[j] >= *x1 || *x0 >= prev_x1[j]) {
            continue;
        }
        if (prev_x0[j] < *x0) { *x0 = prev_x0[j]; }
        if (prev_x1[j] > *x1) { *x1 = prev_x1[j]; }
        prev_used[j] = true;
    }
}

int row_runs_reconcile(const uint16_t *cur_x0, const uint16_t *cur_x1,
                       int cur_n, const uint16_t *prev_x0,
                       const uint16_t *prev_x1, int prev_n,
                       uint16_t *send_x0, uint16_t *send_x1)
{
    bool prev_used[ROW_MAX_RUNS] = { false };
    int n = 0;

    for (int i = 0; i < cur_n; i++) {
        uint16_t x0 = cur_x0[i];
        uint16_t x1 = cur_x1[i];
        absorb_overlapping(&x0, &x1, prev_x0, prev_x1, prev_n, prev_used);
        send_x0[n] = x0;
        send_x1[n] = x1;
        n++;
    }

    for (int j = 0; j < prev_n; j++) {
        if (!prev_used[j]) {
            send_x0[n] = prev_x0[j];
            send_x1[n] = prev_x1[j];
            n++;
        }
    }
    return n;
}
