#ifndef HISTOGRAM_INDEX_H
#define HISTOGRAM_INDEX_H

#include "line_classes.h"

typedef struct {
    size_t class_id;
    size_t first_position;
    size_t count;
    size_t next_record;
} HistogramRecord;

typedef struct {
    HistogramRecord *records;
    size_t record_count;

    size_t *bucket_heads;
    size_t bucket_count;
    size_t *line_to_record;
    size_t *next_occurrence;

    size_t start;
    size_t end;
} HistogramIndex;

int histogram_index_build(
    const FileData *old_file,
    const LineClasses *classes,
    size_t start,
    size_t end,
    HistogramIndex *out
);

void histogram_index_free(HistogramIndex *index);

#endif