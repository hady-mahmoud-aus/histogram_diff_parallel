#include "histogram_partition.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static HistogramPartitionStatus partition_internal(
    const FileData *old_file, const FileData *new_file,
    const LineClasses *classes, HistogramRange range,
    HistogramChangeFlags *flags, HistogramClassicFallback callback,
    void *context, HistogramRange *pending)
{
    for (;;) {
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
        left_status = partition_internal(old_file, new_file, classes, left,
                                         flags, callback, context, pending);
        if (left_status != HISTOGRAM_PARTITION_OK)
            return left_status;
        range = (HistogramRange){anchor.old_end, old_end,
                                 anchor.new_end, new_end};
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
                              flags, callback, context, pending);
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