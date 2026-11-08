#ifndef HISTOGRAM_MYERS_H
#define HISTOGRAM_MYERS_H

#include "histogram_partition.h"

/*
 * Git-style classic fallback for a 0-based, half-open subrange.
 *
 * Register this function directly as HistogramClassicFallback:
 *
 *   histogram_partition_all(old_file, new_file, classes, flags,
 *                           histogram_myers_fallback, NULL, &pending);
 *
 * Returns 0 on success and -1 on invalid input, representation overflow,
 * allocation failure or an internal diff error. Only the supplied sections
 * of old_changed/new_changed can change. If the operation fails, every flag
 * byte remains exactly as it was (the callback is transactional).
 *
 * The context is reserved and ignored. Does not own any caller data.
 * This module is serial and does not write a patch or run compaction.
 */
int histogram_myers_fallback(const FileData *old_file,
                             const FileData *new_file,
                             const LineClasses *classes,
                             HistogramRange range,
                             HistogramChangeFlags *flags,
                             void *context);

#endif
