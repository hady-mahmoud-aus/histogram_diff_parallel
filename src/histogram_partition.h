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

#endif
