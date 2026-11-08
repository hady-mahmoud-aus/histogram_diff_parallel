
#ifndef HISTOGRAM_ANCHOR_H
#define HISTOGRAM_ANCHOR_H

#include "histogram_index.h"

typedef enum {
    HISTOGRAM_ANCHOR_ERROR = -1,
    HISTOGRAM_ANCHOR_NONE = 0,
    HISTOGRAM_ANCHOR_FOUND = 1,
    HISTOGRAM_ANCHOR_UNSUPPORTED = 2, /* legacy M2.1c.1 state, not emitted here */
    HISTOGRAM_ANCHOR_FALLBACK = 3      /* request only; fallback not executed */
} HistogramAnchorStatus;

typedef struct {
    size_t old_start;
    size_t old_end;
    size_t new_start;
    size_t new_end;
    size_t min_old_count;
} HistogramAnchor;

/* 0-based half-open ranges. Output is zeroed unless FOUND. */
HistogramAnchorStatus histogram_anchor_find(
    const FileData *old_file,
    const FileData *new_file,
    const LineClasses *classes,
    size_t old_start,
    size_t old_end,
    size_t new_start,
    size_t new_end,
    HistogramAnchor *out
);

#endif
