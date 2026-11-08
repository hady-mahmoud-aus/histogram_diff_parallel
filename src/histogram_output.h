#ifndef HISTOGRAM_OUTPUT_H
#define HISTOGRAM_OUTPUT_H

#include "histogram_myers.h"
#include <stdio.h>

typedef struct {
    size_t old_start, old_length, new_start, new_length;
} HistogramEdit;

typedef struct {
    HistogramEdit *edits;
    size_t count;
} HistogramEditScript;

/* Mutates flags only on success. Git 2.47.3-style two-way change-group
 * compaction does NOT perform histogram-specific post-compaction re-diff.
 * Myers fallback during partitioning is separate and remains enabled.
 * Callers must supply completed M2.3 change flags. */
int histogram_compact(const FileData *old_file, const FileData *new_file,
                      const LineClasses *classes, HistogramChangeFlags *flags);
/* Build script from compacted change flags, 0-based coordinates. */
int histogram_build_script(const HistogramChangeFlags *flags,
                           HistogramEditScript *out);
void histogram_script_free(HistogramEditScript *script);
/* Three context lines; no ---/+++ headers. Returns -1 on I/O/invalid data. */
int histogram_emit_unified(FILE *out, const FileData *old_file,
                           const FileData *new_file,
                           const HistogramEditScript *script);
#endif
