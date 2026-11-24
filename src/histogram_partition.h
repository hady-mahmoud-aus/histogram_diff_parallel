#ifndef HISTOGRAM_PARTITION_H
#define HISTOGRAM_PARTITION_H

#include "histogram_anchor.h"

/* All ranges are 0-based half-open. Changed flags are indexed 0-based. */
typedef struct {
    size_t old_start, old_end;
    size_t new_start, new_end;
} HistogramRange;

typedef struct {
    unsigned char *old_changed;
    unsigned char *new_changed;
    size_t old_count;
    size_t new_count;
} HistogramChangeFlags;

typedef enum {
    HISTOGRAM_PARTITION_ERROR = -1,
    HISTOGRAM_PARTITION_OK = 0,
    HISTOGRAM_PARTITION_NEEDS_FALLBACK = 1
} HistogramPartitionStatus;

/*
 * Callback for the later classic/Myers implementation (M2.3).
 * Called on a 0-based half-open subrange. Return 0 on success, nonzero on
 * failure. It must update only the supplied region of the flag arrays.
 */
typedef int (*HistogramClassicFallback)(
    const FileData *old_file, const FileData *new_file,
    const LineClasses *classes, HistogramRange range,
    HistogramChangeFlags *flags, void *context);

/* Initialize zero-filled change arrays (out must be empty); free when done. */
int histogram_change_flags_init(HistogramChangeFlags *out,
                                size_t old_count, size_t new_count);
void histogram_change_flags_free(HistogramChangeFlags *flags);

/*
 * Diff exactly 'range', writing only the corresponding sections of flags.
 * Required buffer counts must equal the respective FileData.line_count.
 * Existing flag bytes are preserved outside the active region.
 *
 * If a fallback is requested and callback == NULL, returns NEEDS_FALLBACK,
 * sets *pending to that exact subrange (if provided), and stops. Earlier
 * writes are retained: flags are not a complete diff until the fallback is
 * implemented. With a callback, success resumes normal recursion/tail loop.
 * Returns ERROR for invalid inputs, index errors or callback failures.
 */
HistogramPartitionStatus histogram_partition_range(
    const FileData *old_file, const FileData *new_file,
    const LineClasses *classes, HistogramRange range,
    HistogramChangeFlags *flags, HistogramClassicFallback callback,
    void *context, HistogramRange *pending);

HistogramPartitionStatus histogram_partition_all(
    const FileData *old_file, const FileData *new_file,
    const LineClasses *classes, HistogramChangeFlags *flags,
    HistogramClassicFallback callback, void *context,
    HistogramRange *pending);

/*
 * Checked translation for a Git-style fallback expecting 1-based line1 and
 * line2, with signed-int counts. Empty subranges use start+1 and count=0.
 * Returns -1 if a coordinate is outside Git's signed-int representation.
 */
int histogram_range_to_git(HistogramRange range,
                           int *line1, int *count1, int *line2, int *count2);

#ifdef _OPENMP
/* Initial task-only implementation. Limit CLI/API requests to a practical
 * positive range; the OpenMP runtime may impose a smaller actual team. */
#define HISTOGRAM_MAX_THREADS 1024
#define HISTOGRAM_DEFAULT_CUTOFF ((size_t)1024)

typedef struct {
    int actual_threads;
    size_t explicit_tasks; /* all explicit child tasks executed */
    int task_workers;      /* workers executing an explicit child task */
    int region_workers;    /* workers entering a region in task mode */
} HistogramParallelStats;

/* Diff the entire file pair with one OpenMP team. cutoff is the combined
 * old/new line count per child. Both children must meet it before spawning.
 * Inputs/classes and flag metadata must remain read-only, and each sibling
 * owns its disjoint flag ranges. A callback must be reentrant and may write
 * only the provided range; context must be immutable or independently safe.
 * All tasks finish before return. On non-OK status flags are incomplete and
 * must not be used for output; pending has left-first error precedence.
 * Pass stats == NULL for normal, non-instrumented execution. */
HistogramPartitionStatus histogram_partition_parallel(
    const FileData *old_file, const FileData *new_file,
    const LineClasses *classes, HistogramChangeFlags *flags,
    HistogramClassicFallback callback, void *context, HistogramRange *pending,
    int threads, size_t cutoff, HistogramParallelStats *stats);
#endif

#endif
