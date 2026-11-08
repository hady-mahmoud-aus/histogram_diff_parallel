#include "histogram_index.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Per-range occurrence index for histogram_anchor_find().
 *
 * The table size is the smallest power of two >= the OLD range length,
 * with a one-bucket minimum. The bucket calculation intentionally matches
 * histogram_anchor.c. Position chains contain absolute OLD line indexes.
 *
 * No memory is retained on failure. The caller supplies an empty/freed out.
 */
static int table_size_for_length(size_t length, size_t *bucket_count,
                                 unsigned int *bits)
{
    size_t n = 1;
    unsigned int b = 0;
    while (n < length) {
        if (n > SIZE_MAX / 2)
            return -1;
        n *= 2;
        ++b;
    }
    *bucket_count = n;
    *bits = b;
    return 0;
}

static size_t bucket_for_class(size_t class_id, size_t bucket_count,
                               unsigned int bits)
{
    unsigned long key = (unsigned long)class_id;
    return (size_t)((key + (key >> bits)) &
                    ((unsigned long)bucket_count - 1UL));
}

int histogram_index_build(const FileData *old_file,
                          const LineClasses *classes,
                          size_t start, size_t end,
                          HistogramIndex *out)
{
    HistogramIndex index = {0};
    size_t length, buckets;
    unsigned int bits;

    if (out == NULL || old_file == NULL || classes == NULL ||
        start > end || end > old_file->line_count)
        return -1;
    *out = (HistogramIndex){0};

    length = end - start;
    if (length && (classes->old_ids == NULL || classes->class_count == 0))
        return -1;
    if (table_size_for_length(length, &buckets, &bits) != 0)
        return -1;
    if (length > SIZE_MAX / sizeof *index.records ||
        length > SIZE_MAX / sizeof *index.line_to_record ||
        length > SIZE_MAX / sizeof *index.next_occurrence ||
        buckets > SIZE_MAX / sizeof *index.bucket_heads)
        return -1;

    index.bucket_heads = malloc(buckets * sizeof *index.bucket_heads);
    if (index.bucket_heads == NULL)
        goto fail;
    for (size_t k = 0; k < buckets; ++k)
        index.bucket_heads[k] = SIZE_MAX;
    index.bucket_count = buckets;
    index.start = start;
    index.end = end;

    if (length != 0) {
        index.records = malloc(length * sizeof *index.records);
        index.line_to_record = malloc(length * sizeof *index.line_to_record);
        index.next_occurrence = malloc(length * sizeof *index.next_occurrence);
        if (index.records == NULL || index.line_to_record == NULL ||
            index.next_occurrence == NULL)
            goto fail;
    }

    /* Reverse scan: prepend new records, keep duplicate positions ascending. */
    for (size_t pos = end; pos > start;) {
        --pos;
        size_t class_id = classes->old_ids[pos];
        size_t rec_id, bucket, chain_length = 0;
        size_t offset = pos - start;
        if (class_id >= classes->class_count || class_id > ULONG_MAX)
            goto fail;
        bucket = bucket_for_class(class_id, buckets, bits);
        rec_id = index.bucket_heads[bucket];
        while (rec_id != SIZE_MAX) {
            if (index.records[rec_id].class_id == class_id)
                break;
            ++chain_length;
            rec_id = index.records[rec_id].next_record;
        }
        if (rec_id == SIZE_MAX) {
            /* Git aborts when the 65th distinct record enters a bucket. */
            if (chain_length >= 64)
                goto fail;
            rec_id = index.record_count++;
            index.records[rec_id] = (HistogramRecord){
                .class_id = class_id,
                .first_position = pos,
                .count = 1,
                .next_record = index.bucket_heads[bucket]
            };
            index.bucket_heads[bucket] = rec_id;
            index.next_occurrence[offset] = SIZE_MAX;
        } else {
            index.next_occurrence[offset] = index.records[rec_id].first_position;
            index.records[rec_id].first_position = pos;
            if (index.records[rec_id].count < UINT_MAX)
                ++index.records[rec_id].count;
        }
        index.line_to_record[offset] = rec_id;
    }

    *out = index;
    return 0;

fail:
    histogram_index_free(&index);
    return -1;
}

void histogram_index_free(HistogramIndex *index)
{
    if (index == NULL)
        return;
    free(index->records);
    free(index->bucket_heads);
    free(index->line_to_record);
    free(index->next_occurrence);
    *index = (HistogramIndex){0};
}
