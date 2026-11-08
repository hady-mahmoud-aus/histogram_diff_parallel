/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * This module follows Git/LibXDiff's classic Myers fallback algorithm,
 * originally developed by Davide Libenzi, with separate C data structures.
 * See xdiff/xdiffi.c and xdiff/xprepare.c in the Git project for provenance.
 */
#include "histogram_myers.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Standalone adaptation of the classic Myers divide-and-conquer algorithm
 * used by Git's xdiff. The preparation pass is local to each fallback range:
 * trim equal ends, drop nonmatching records, and selectively drop records
 * occurring excessively often. No upstream source is incorporated verbatim.
 *
 * Git-style nonminimal split heuristics and tie direction are retained;
 * the version-specific source audit for Git 2.47.3 remains outstanding.
 */

enum { MAYBE_DISCARD = 2, KEEP = 1, DISCARD = 0 };
enum { REPEAT_WINDOW = 100, REPEAT_RATIO = 4, MAX_EQUAL_LIMIT = 1024 };
enum { MIN_COST = 256, LONG_SNAKE = 20, HEURISTIC_FACTOR = 4 };

typedef struct {
    long old_first, old_last; /* local compacted-reference offsets */
    long new_first, new_last;
    int minimal;
} MyersBox;

typedef struct {
    long old_at, new_at;
    int minimal_left, minimal_right;
} MyersSplit;

typedef struct {
    const LineClasses *classes;
    size_t old_start, new_start;
    size_t *old_ref, *new_ref; /* local zero-based original positions */
    size_t old_refs, new_refs;
    unsigned char *old_change, *new_change;
    long *front, *back;       /* signed diagonal arrays */
    long max_cost;
} MyersWork;

static int checked_mul(size_t a, size_t b, size_t *out)
{
    if (b != 0 && a > SIZE_MAX / b)
        return -1;
    *out = a * b;
    return 0;
}

/* Same integer-square-root upper approximation as the xdiff cost threshold. */
static size_t square_estimate(size_t n)
{
    size_t v = 1;
    for (; n != 0; n >>= 2)
        v <<= 1;
    return v;
}

static int valid_arguments(const FileData *old_file,
                           const FileData *new_file,
                           const LineClasses *classes,
                           HistogramRange range,
                           const HistogramChangeFlags *flags)
{
    int line1, n1, line2, n2;
    if (old_file == NULL || new_file == NULL || classes == NULL ||
        flags == NULL ||
        range.old_start > range.old_end ||
        range.new_start > range.new_end ||
        range.old_end > old_file->line_count ||
        range.new_end > new_file->line_count ||
        flags->old_count != old_file->line_count ||
        flags->new_count != new_file->line_count ||
        (flags->old_count && flags->old_changed == NULL) ||
        (flags->new_count && flags->new_changed == NULL) ||
        (old_file->line_count && classes->old_ids == NULL) ||
        (new_file->line_count && classes->new_ids == NULL) ||
        ((old_file->line_count || new_file->line_count) &&
         classes->class_count == 0) ||
        histogram_range_to_git(range, &line1, &n1, &line2, &n2) != 0)
        return 0;
    /* Validate only IDs used by this range, before committing any output. */
    for (size_t i = range.old_start; i < range.old_end; ++i)
        if (classes->old_ids[i] >= classes->class_count)
            return 0;
    for (size_t j = range.new_start; j < range.new_end; ++j)
        if (classes->new_ids[j] >= classes->class_count)
            return 0;
    return 1;
}

/* Initial scan of a run of ambiguous matches: avoid discarding isolated runs. */
static int discard_multi_match(const unsigned char *a, size_t i,
                               size_t first, size_t last)
{
    size_t lo = i - first > REPEAT_WINDOW ? i - REPEAT_WINDOW : first;
    size_t hi = last - i > REPEAT_WINDOW ? i + REPEAT_WINDOW : last;
    size_t no_match_before = 0, multi_before = 1;
    size_t no_match_after = 0, multi_after = 1;

    for (size_t j = i; j > lo;) {
        --j;
        if (a[j] == DISCARD)
            ++no_match_before;
        else if (a[j] == MAYBE_DISCARD)
            ++multi_before;
        else
            break;
    }
    if (no_match_before == 0)
        return 0;
    for (size_t j = i + 1; j <= hi; ++j) {
        if (a[j] == DISCARD)
            ++no_match_after;
        else if (a[j] == MAYBE_DISCARD)
            ++multi_after;
        else
            break;
    }
    if (no_match_after == 0)
        return 0;
    return (multi_before + multi_after) * REPEAT_RATIO <
           (multi_before + multi_after +
            no_match_before + no_match_after);
}

static int prepare_references(MyersWork *w, size_t old_len, size_t new_len)
{
    size_t old_first = 0, new_first = 0;
    size_t old_last = old_len, new_last = new_len;
    size_t *old_frequency = NULL, *new_frequency = NULL;
    unsigned char *old_action = NULL, *new_action = NULL;
    size_t size = 0;
    int result = -1;

    /* Git reparses each subfile and removes a shared prefix and suffix. */
    while (old_first < old_last && new_first < new_last &&
           w->classes->old_ids[w->old_start + old_first] ==
           w->classes->new_ids[w->new_start + new_first]) {
        ++old_first;
        ++new_first;
    }
    while (old_first < old_last && new_first < new_last &&
           w->classes->old_ids[w->old_start + old_last - 1] ==
           w->classes->new_ids[w->new_start + new_last - 1]) {
        --old_last;
        --new_last;
    }

    /* The source classifier supplies exact class IDs, so no re-hashing. */
    if (checked_mul(w->classes->class_count, sizeof *old_frequency,
                    &size) != 0)
        return -1;
    old_frequency = calloc(w->classes->class_count, sizeof *old_frequency);
    new_frequency = calloc(w->classes->class_count, sizeof *new_frequency);
    old_action = calloc(old_len ? old_len : 1, 1);
    new_action = calloc(new_len ? new_len : 1, 1);
    if (old_frequency == NULL || new_frequency == NULL ||
        old_action == NULL || new_action == NULL)
        goto done;
    for (size_t i = 0; i < old_len; ++i)
        ++old_frequency[w->classes->old_ids[w->old_start + i]];
    for (size_t j = 0; j < new_len; ++j)
        ++new_frequency[w->classes->new_ids[w->new_start + j]];

    size_t old_limit = square_estimate(old_len);
    size_t new_limit = square_estimate(new_len);
    if (old_limit > MAX_EQUAL_LIMIT) old_limit = MAX_EQUAL_LIMIT;
    if (new_limit > MAX_EQUAL_LIMIT) new_limit = MAX_EQUAL_LIMIT;

    for (size_t i = old_first; i < old_last; ++i) {
        size_t count = new_frequency[w->classes->old_ids[w->old_start + i]];
        old_action[i] = count == 0 ? DISCARD :
                        count >= old_limit ? MAYBE_DISCARD : KEEP;
    }
    for (size_t j = new_first; j < new_last; ++j) {
        size_t count = old_frequency[w->classes->new_ids[w->new_start + j]];
        new_action[j] = count == 0 ? DISCARD :
                        count >= new_limit ? MAYBE_DISCARD : KEEP;
    }

    for (size_t i = old_first; i < old_last; ++i) {
        unsigned char action = old_action[i];
        if (action == MAYBE_DISCARD)
            action = discard_multi_match(old_action, i, old_first,
                                         old_last - 1) ? DISCARD : KEEP;
        if (action == DISCARD)
            w->old_change[i] = 1;
        else
            w->old_ref[w->old_refs++] = i;
    }
    for (size_t j = new_first; j < new_last; ++j) {
        unsigned char action = new_action[j];
        if (action == MAYBE_DISCARD)
            action = discard_multi_match(new_action, j, new_first,
                                         new_last - 1) ? DISCARD : KEEP;
        if (action == DISCARD)
            w->new_change[j] = 1;
        else
            w->new_ref[w->new_refs++] = j;
    }
    result = 0;
done:
    free(old_frequency);
    free(new_frequency);
    free(old_action);
    free(new_action);
    return result;
}

static int ref_equal(const MyersWork *w, long a, long b)
{
    return w->classes->old_ids[w->old_start + w->old_ref[a]] ==
           w->classes->new_ids[w->new_start + w->new_ref[b]];
}

/* Find Git's forward/backward middle path with its stable diagonal ties. */
static int split_box(MyersWork *w, MyersBox box, MyersSplit *split)
{
    const long old_lo = box.old_first, old_hi = box.old_last;
    const long new_lo = box.new_first, new_hi = box.new_last;
    const long diag_lo = old_lo - new_hi, diag_hi = old_hi - new_lo;
    const long forward_mid = old_lo - new_lo;
    const long backward_mid = old_hi - new_hi;
    const int odd = ((forward_mid - backward_mid) & 1L) != 0;
    long fmin = forward_mid, fmax = forward_mid;
    long bmin = backward_mid, bmax = backward_mid;

    w->front[forward_mid] = old_lo;
    w->back[backward_mid] = old_hi;
    for (long cost = 1;; ++cost) {
        int long_snake_found = 0;
        if (fmin > diag_lo) w->front[--fmin - 1] = -1;
        else ++fmin;
        if (fmax < diag_hi) w->front[++fmax + 1] = -1;
        else --fmax;
        for (long d = fmax; d >= fmin; d -= 2) {
            long x = w->front[d - 1] >= w->front[d + 1] ?
                     w->front[d - 1] + 1 : w->front[d + 1];
            long previous_x = x, y = x - d;
            while (x < old_hi && y < new_hi && ref_equal(w, x, y)) {
                ++x;
                ++y;
            }
            if (x - previous_x > LONG_SNAKE)
                long_snake_found = 1;
            w->front[d] = x;
            if (odd && bmin <= d && d <= bmax && w->back[d] <= x) {
                *split = (MyersSplit){x, y, 1, 1};
                return 0;
            }
        }
        if (bmin > diag_lo) w->back[--bmin - 1] = LONG_MAX;
        else ++bmin;
        if (bmax < diag_hi) w->back[++bmax + 1] = LONG_MAX;
        else --bmax;
        for (long d = bmax; d >= bmin; d -= 2) {
            long x = w->back[d - 1] < w->back[d + 1] ?
                     w->back[d - 1] : w->back[d + 1] - 1;
            long previous_x = x, y = x - d;
            while (x > old_lo && y > new_lo && ref_equal(w, x - 1, y - 1)) {
                --x;
                --y;
            }
            if (previous_x - x > LONG_SNAKE)
                long_snake_found = 1;
            w->back[d] = x;
            if (!odd && fmin <= d && d <= fmax && x <= w->front[d]) {
                *split = (MyersSplit){x, y, 1, 1};
                return 0;
            }
        }
        if (box.minimal)
            continue;

        if (long_snake_found && cost > MIN_COST) {
            long best = 0;
            for (long d = fmax; d >= fmin; d -= 2) {
                long distance = labs(d - forward_mid);
                long x = w->front[d], y = x - d;
                long value = (x - old_lo) + (y - new_lo) - distance;
                if (value > HEURISTIC_FACTOR * cost && value > best &&
                    x >= old_lo + LONG_SNAKE && x < old_hi &&
                    y >= new_lo + LONG_SNAKE && y < new_hi) {
                    long k;
                    for (k = 1; k <= LONG_SNAKE &&
                         ref_equal(w, x - k, y - k); ++k) {}
                    if (k > LONG_SNAKE) {
                        best = value;
                        *split = (MyersSplit){x, y, 1, 0};
                    }
                }
            }
            if (best > 0)
                return 0;
            for (long d = bmax; d >= bmin; d -= 2) {
                long distance = labs(d - backward_mid);
                long x = w->back[d], y = x - d;
                long value = (old_hi - x) + (new_hi - y) - distance;
                if (value > HEURISTIC_FACTOR * cost && value > best &&
                    x > old_lo && x <= old_hi - LONG_SNAKE &&
                    y > new_lo && y <= new_hi - LONG_SNAKE) {
                    long k;
                    for (k = 0; k < LONG_SNAKE &&
                         ref_equal(w, x + k, y + k); ++k) {}
                    if (k == LONG_SNAKE) {
                        best = value;
                        *split = (MyersSplit){x, y, 0, 1};
                    }
                }
            }
            if (best > 0)
                return 0;
        }
        if (cost >= w->max_cost) {
            long best_forward = -1, best_backward = LONG_MAX;
            long best_fx = -1, best_bx = LONG_MAX;
            for (long d = fmax; d >= fmin; d -= 2) {
                long x = w->front[d] < old_hi ? w->front[d] : old_hi;
                long y = x - d;
                if (y > new_hi) { x = new_hi + d; y = new_hi; }
                if (x + y > best_forward) {
                    best_forward = x + y;
                    best_fx = x;
                }
            }
            for (long d = bmax; d >= bmin; d -= 2) {
                long x = w->back[d] > old_lo ? w->back[d] : old_lo;
                long y = x - d;
                if (y < new_lo) { x = new_lo + d; y = new_lo; }
                if (x + y < best_backward) {
                    best_backward = x + y;
                    best_bx = x;
                }
            }
            if ((old_hi + new_hi) - best_backward <
                best_forward - (old_lo + new_lo)) {
                *split = (MyersSplit){best_fx, best_forward - best_fx, 1, 0};
            } else {
                *split = (MyersSplit){best_bx, best_backward - best_bx, 0, 1};
            }
            return 0;
        }
    }
}

static int run_diff(MyersWork *w)
{
    MyersBox *stack = NULL;
    size_t depth = 0, capacity = 0;
    size_t diagonal_count, bytes;
    long *diagonals = NULL;
    int ret = -1;

    if (w->old_refs > SIZE_MAX - w->new_refs - 3)
        return -1;
    diagonal_count = w->old_refs + w->new_refs + 3;
    if (diagonal_count > (size_t)LONG_MAX / 2 - 1 ||
        checked_mul(2 * diagonal_count + 2, sizeof *diagonals, &bytes) != 0)
        return -1;
    diagonals = malloc(bytes);
    if (diagonals == NULL)
        return -1;
    w->front = diagonals + w->new_refs + 1;
    w->back = diagonals + diagonal_count + w->new_refs + 1;
    size_t estimate = square_estimate(diagonal_count);
    w->max_cost = (long)(estimate > MIN_COST ? estimate : MIN_COST);

    capacity = 32;
    stack = malloc(capacity * sizeof *stack);
    if (stack == NULL)
        goto done;
    stack[depth++] = (MyersBox){0, (long)w->old_refs,
                                0, (long)w->new_refs, 0};
    while (depth) {
        MyersBox box = stack[--depth];
        while (box.old_first < box.old_last &&
               box.new_first < box.new_last &&
               ref_equal(w, box.old_first, box.new_first)) {
            ++box.old_first;
            ++box.new_first;
        }
        while (box.old_first < box.old_last &&
               box.new_first < box.new_last &&
               ref_equal(w, box.old_last - 1, box.new_last - 1)) {
            --box.old_last;
            --box.new_last;
        }
        if (box.old_first == box.old_last) {
            for (long j = box.new_first; j < box.new_last; ++j)
                w->new_change[w->new_ref[j]] = 1;
            continue;
        }
        if (box.new_first == box.new_last) {
            for (long i = box.old_first; i < box.old_last; ++i)
                w->old_change[w->old_ref[i]] = 1;
            continue;
        }
        MyersSplit split;
        if (split_box(w, box, &split) != 0 ||
            split.old_at < box.old_first || split.old_at > box.old_last ||
            split.new_at < box.new_first || split.new_at > box.new_last ||
            (split.old_at == box.old_first && split.new_at == box.new_first) ||
            (split.old_at == box.old_last && split.new_at == box.new_last))
            goto done;
        if (depth + 2 > capacity) {
            size_t next = capacity <= SIZE_MAX / 2 ? capacity * 2 : SIZE_MAX;
            if (next < depth + 2 || next > SIZE_MAX / sizeof *stack)
                goto done;
            MyersBox *grown = realloc(stack, next * sizeof *stack);
            if (grown == NULL)
                goto done;
            stack = grown;
            capacity = next;
        }
        /* Push right first, so the left recursion is processed first. */
        stack[depth++] = (MyersBox){split.old_at, box.old_last,
                                   split.new_at, box.new_last,
                                   split.minimal_right};
        stack[depth++] = (MyersBox){box.old_first, split.old_at,
                                   box.new_first, split.new_at,
                                   split.minimal_left};
    }
    ret = 0;
done:
    free(stack);
    free(diagonals);
    return ret;
}

int histogram_myers_fallback(const FileData *old_file,
                             const FileData *new_file,
                             const LineClasses *classes,
                             HistogramRange range,
                             HistogramChangeFlags *flags,
                             void *context)
{
    MyersWork w = {0};
    size_t old_len, new_len, bytes;
    int result = -1;
    (void)context;

    if (!valid_arguments(old_file, new_file, classes, range, flags))
        return -1;
    old_len = range.old_end - range.old_start;
    new_len = range.new_end - range.new_start;
    w.classes = classes;
    w.old_start = range.old_start;
    w.new_start = range.new_start;
    if (old_len && (checked_mul(old_len, sizeof *w.old_ref, &bytes) != 0 ||
                    (w.old_ref = malloc(bytes)) == NULL))
        goto done;
    if (new_len && (checked_mul(new_len, sizeof *w.new_ref, &bytes) != 0 ||
                    (w.new_ref = malloc(bytes)) == NULL))
        goto done;
    w.old_change = calloc(old_len ? old_len : 1, 1);
    w.new_change = calloc(new_len ? new_len : 1, 1);
    if (w.old_change == NULL || w.new_change == NULL)
        goto done;

    if (old_len == 0)
        memset(w.new_change, 1, new_len);
    else if (new_len == 0)
        memset(w.old_change, 1, old_len);
    else if (prepare_references(&w, old_len, new_len) != 0 ||
             run_diff(&w) != 0)
        goto done;

    if (old_len != 0)
        memcpy(flags->old_changed + range.old_start, w.old_change, old_len);
    if (new_len != 0)
        memcpy(flags->new_changed + range.new_start, w.new_change, new_len);
    result = 0;
done:
    free(w.old_ref);
    free(w.new_ref);
    free(w.old_change);
    free(w.new_change);
    return result;
}
