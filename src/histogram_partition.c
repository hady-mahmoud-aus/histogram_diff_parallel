#include "histogram_partition.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

int histogram_change_flags_init(HistogramChangeFlags *out,
                                size_t old_count, size_t new_count)
{
    HistogramChangeFlags result = {0};
    if (out == NULL)
        return -1;
    *out = (HistogramChangeFlags){0};
    result.old_count = old_count;
    result.new_count = new_count;
    if (old_count) {
        result.old_changed = calloc(old_count, sizeof *result.old_changed);
        if (result.old_changed == NULL)
            goto fail;
    }
    if (new_count) {
        result.new_changed = calloc(new_count, sizeof *result.new_changed);
        if (result.new_changed == NULL)
            goto fail;
    }
    *out = result;
    return 0;

fail:
    histogram_change_flags_free(&result);
    return -1;
}

void histogram_change_flags_free(HistogramChangeFlags *flags)
{
    if (flags == NULL)
        return;
    free(flags->old_changed);
    free(flags->new_changed);
    *flags = (HistogramChangeFlags){0};
}

int histogram_range_to_git(HistogramRange range,
                           int *line1, int *count1, int *line2, int *count2)
{
    if (line1 == NULL || count1 == NULL || line2 == NULL || count2 == NULL ||
        range.old_start > range.old_end ||
        range.new_start > range.new_end ||
        range.old_start >= INT_MAX || range.new_start >= INT_MAX ||
        range.old_end - range.old_start > INT_MAX ||
        range.new_end - range.new_start > INT_MAX)
        return -1;
    *line1 = (int)(range.old_start + 1);
    *count1 = (int)(range.old_end - range.old_start);
    *line2 = (int)(range.new_start + 1);
    *count2 = (int)(range.new_end - range.new_start);
    return 0;
}

static int is_valid_range(const FileData *old_file, const FileData *new_file,
                          const LineClasses *classes, HistogramRange range,
                          const HistogramChangeFlags *flags)
{
    if (old_file == NULL || new_file == NULL || classes == NULL ||
        flags == NULL ||
        range.old_start > range.old_end ||
        range.new_start > range.new_end ||
        range.old_end > old_file->line_count ||
        range.new_end > new_file->line_count ||
        flags->old_count != old_file->line_count ||
        flags->new_count != new_file->line_count ||
        (flags->old_count != 0 && flags->old_changed == NULL) ||
        (flags->new_count != 0 && flags->new_changed == NULL) ||
        (old_file->line_count != 0 && classes->old_ids == NULL) ||
        (new_file->line_count != 0 && classes->new_ids == NULL) ||
        ((old_file->line_count != 0 || new_file->line_count != 0) &&
         classes->class_count == 0))
        return 0;
    return 1;
}

/* Per-comparison configuration. Only diagnostic arrays are mutable;
 * each worker owns its own array elements. NULL selects pure serial recursion. */
typedef struct {
    size_t cutoff;
    size_t *tasks_by_thread;
    unsigned char *region_seen;
} ParallelState;

#ifdef _OPENMP
/* Half-open range is already validated. Avoid overflow in old_len + new_len.
 * One-sided regions need only a memset, so never spawn tasks for them. */
static int task_eligible(HistogramRange range, size_t cutoff)
{
    size_t a = range.old_end - range.old_start;
    size_t b = range.new_end - range.new_start;
    return a != 0 && b != 0 && (a >= cutoff || b >= cutoff - a);
}
#endif

static HistogramPartitionStatus partition_internal(
    const FileData *old_file, const FileData *new_file,
    const LineClasses *classes, HistogramRange range,
    HistogramChangeFlags *flags, HistogramClassicFallback callback,
    void *context, HistogramRange *pending, ParallelState *parallel)
{
#ifndef _OPENMP
    (void)parallel;
#endif
    for (;;) {
#ifdef _OPENMP
        if (parallel != NULL) {
            if (parallel->region_seen != NULL)
                parallel->region_seen[omp_get_thread_num()] = 1;
            if (!task_eligible(range, parallel->cutoff))
                parallel = NULL; /* Descendants can only be smaller. */
        }
#endif
        HistogramAnchor anchor;
        HistogramAnchorStatus found;
        HistogramRange left;
        HistogramPartitionStatus left_status;
        size_t old_start = range.old_start, old_end = range.old_end;
        size_t new_start = range.new_start, new_end = range.new_end;

        if (old_start == old_end && new_start == new_end)
            return HISTOGRAM_PARTITION_OK;
        if (old_start == old_end) {
            memset(flags->new_changed + new_start, 1, new_end - new_start);
            return HISTOGRAM_PARTITION_OK;
        }
        if (new_start == new_end) {
            memset(flags->old_changed + old_start, 1, old_end - old_start);
            return HISTOGRAM_PARTITION_OK;
        }

        found = histogram_anchor_find(old_file, new_file, classes,
                                      old_start, old_end, new_start, new_end,
                                      &anchor);
        if (found == HISTOGRAM_ANCHOR_ERROR)
            return HISTOGRAM_PARTITION_ERROR;
        if (found == HISTOGRAM_ANCHOR_FALLBACK) {
            if (callback == NULL) {
                if (pending != NULL)
                    *pending = range;
                return HISTOGRAM_PARTITION_NEEDS_FALLBACK;
            }
            if (callback(old_file, new_file, classes, range,
                         flags, context) != 0) {
                if (pending != NULL)
                    *pending = range;
                return HISTOGRAM_PARTITION_ERROR;
            }
            return HISTOGRAM_PARTITION_OK;
        }
        if (found == HISTOGRAM_ANCHOR_NONE) {
            memset(flags->old_changed + old_start, 1, old_end - old_start);
            memset(flags->new_changed + new_start, 1, new_end - new_start);
            return HISTOGRAM_PARTITION_OK;
        }
        if (found != HISTOGRAM_ANCHOR_FOUND ||
            anchor.old_start < old_start || anchor.old_end > old_end ||
            anchor.new_start < new_start || anchor.new_end > new_end ||
            anchor.old_start >= anchor.old_end ||
            anchor.new_start >= anchor.new_end ||
            anchor.old_end - anchor.old_start !=
                anchor.new_end - anchor.new_start)
            return HISTOGRAM_PARTITION_ERROR;

        /* Git processes the left child first and tail-loops on the right. */
        left = (HistogramRange){old_start, anchor.old_start,
                                new_start, anchor.new_start};
        HistogramRange right = {anchor.old_end, old_end,
                                anchor.new_end, new_end};
#ifdef _OPENMP
        /* Conservative cutoff: both siblings must warrant concurrent work.
         * Otherwise retain left-first recursion and the right tail loop.
         * A large sibling may still find eligible splits farther down. */
        if (parallel != NULL && task_eligible(left, parallel->cutoff) &&
            task_eligible(right, parallel->cutoff)) {
            HistogramPartitionStatus right_status;
            HistogramRange left_pending = {0}, right_pending = {0};
            /* The taskgroup joins all descendant work before the parent's
             * stack results go out of scope, including inline recursion. */
            #pragma omp taskgroup
            {
                #pragma omp task default(none) \
                    firstprivate(old_file, new_file, classes, left, flags, callback, context, parallel) \
                    shared(left_status, left_pending)
                {
                    if (parallel->tasks_by_thread != NULL)
                        ++parallel->tasks_by_thread[omp_get_thread_num()];
                    left_status = partition_internal(old_file, new_file, classes,
                                                     left, flags, callback, context,
                                                     &left_pending, parallel);
                }
                right_status = partition_internal(old_file, new_file, classes,
                                                  right, flags, callback, context,
                                                  &right_pending, parallel);
            }
            /* Deterministic left-first status precedence. A failed sibling
             * does not cancel the other: both are joined, flags are invalid
             * for output on any non-OK return. */
            if (left_status != HISTOGRAM_PARTITION_OK) {
                if (pending != NULL) *pending = left_pending;
                return left_status;
            }
            if (right_status != HISTOGRAM_PARTITION_OK && pending != NULL)
                *pending = right_pending;
            return right_status;
        }
#endif
        left_status = partition_internal(old_file, new_file, classes, left,
                                         flags, callback, context, pending, parallel);
        if (left_status != HISTOGRAM_PARTITION_OK)
            return left_status;
        range = right;
    }
}

HistogramPartitionStatus histogram_partition_range(
    const FileData *old_file, const FileData *new_file,
    const LineClasses *classes, HistogramRange range,
    HistogramChangeFlags *flags, HistogramClassicFallback callback,
    void *context, HistogramRange *pending)
{
    if (pending != NULL)
        *pending = (HistogramRange){0};
    if (!is_valid_range(old_file, new_file, classes, range, flags))
        return HISTOGRAM_PARTITION_ERROR;
    return partition_internal(old_file, new_file, classes, range,
                              flags, callback, context, pending, NULL);
}

HistogramPartitionStatus histogram_partition_all(
    const FileData *old_file, const FileData *new_file,
    const LineClasses *classes, HistogramChangeFlags *flags,
    HistogramClassicFallback callback, void *context,
    HistogramRange *pending)
{
    if (pending != NULL)
        *pending = (HistogramRange){0};
    if (old_file == NULL || new_file == NULL)
        return HISTOGRAM_PARTITION_ERROR;
    HistogramRange range = {0, old_file->line_count,
                            0, new_file->line_count};
    return histogram_partition_range(old_file, new_file, classes, range,
                                      flags, callback, context, pending);
}

#ifdef _OPENMP
HistogramPartitionStatus histogram_partition_parallel(
    const FileData *old_file, const FileData *new_file,
    const LineClasses *classes, HistogramChangeFlags *flags,
    HistogramClassicFallback callback, void *context, HistogramRange *pending,
    int threads, size_t cutoff, HistogramParallelStats *stats)
{
    ParallelState state = { .cutoff = cutoff };
    HistogramPartitionStatus status = HISTOGRAM_PARTITION_ERROR;
    int actual_threads = 0;
    if (pending != NULL) *pending = (HistogramRange){0};
    if (stats != NULL) *stats = (HistogramParallelStats){0};
    if (old_file == NULL || new_file == NULL ||
        threads < 1 || threads > HISTOGRAM_MAX_THREADS || cutoff == 0 ||
        omp_get_level() != 0)
        return HISTOGRAM_PARTITION_ERROR;
    HistogramRange range = {0, old_file->line_count, 0, new_file->line_count};
    if (!is_valid_range(old_file, new_file, classes, range, flags))
        return HISTOGRAM_PARTITION_ERROR;

    if (stats != NULL) {
        state.tasks_by_thread = calloc((size_t)threads, sizeof *state.tasks_by_thread);
        state.region_seen = calloc((size_t)threads, sizeof *state.region_seen);
        if (state.tasks_by_thread == NULL || state.region_seen == NULL)
            goto done;
    }

    /* One team per comparison, one initial recursive entry. No nested teams.
     * The actual team may be smaller due to runtime limits. */
    int previous_dynamic = omp_get_dynamic();
    omp_set_dynamic(0);
    #pragma omp parallel num_threads(threads) default(none) \
        shared(state, status, actual_threads) \
        firstprivate(old_file, new_file, classes, range, flags, callback, context, pending)
    {
        #pragma omp single
        {
            actual_threads = omp_get_num_threads();
            status = partition_internal(old_file, new_file, classes, range,
                                        flags, callback, context, pending, &state);
        }
    }
    omp_set_dynamic(previous_dynamic);

    if (stats != NULL) {
        stats->actual_threads = actual_threads;
        for (int i = 0; i < actual_threads; ++i) {
            stats->explicit_tasks += state.tasks_by_thread[i];
            if (state.tasks_by_thread[i] != 0) ++stats->task_workers;
            if (state.region_seen[i] != 0) ++stats->region_workers;
        }
    }
done:
    free(state.tasks_by_thread);
    free(state.region_seen);
    return status;
}
#endif
