#include "histogram_anchor.h"

#include <limits.h>
#include <stdint.h>

/* The index already owns the hash table; use its exact bucket mapping. */
static size_t anchor_bucket_for_class(size_t class_id, size_t bucket_count)
{
    unsigned int bits = 0;
    for (size_t n = bucket_count; n > 1; n >>= 1)
        ++bits;

    unsigned long key = (unsigned long)class_id;
    return (size_t)((key + (key >> bits)) &
                    ((unsigned long)bucket_count - 1UL));
}

static size_t count_at(const HistogramIndex *index, size_t old_pos)
{
    size_t record_id = index->line_to_record[old_pos - index->start];
    return index->records[record_id].count;
}

static int same_class(const LineClasses *classes, size_t old_pos,
                      size_t new_pos)
{
    return classes->old_ids[old_pos] == classes->new_ids[new_pos];
}

HistogramAnchorStatus histogram_anchor_find(
    const FileData *old_file,
    const FileData *new_file,
    const LineClasses *classes,
    size_t old_start,
    size_t old_end,
    size_t new_start,
    size_t new_end,
    HistogramAnchor *out)
{
    HistogramIndex index = (HistogramIndex){0};
    HistogramAnchor best = (HistogramAnchor){0};
    HistogramAnchorStatus result = HISTOGRAM_ANCHOR_NONE;
    size_t best_count = 65;
    int has_common = 0;

    if (out == NULL)
        return HISTOGRAM_ANCHOR_ERROR;
    *out = (HistogramAnchor){0};

    if (old_file == NULL || new_file == NULL || classes == NULL ||
        old_start > old_end || old_end > old_file->line_count ||
        new_start > new_end || new_end > new_file->line_count)
        return HISTOGRAM_ANCHOR_ERROR;
    if (old_start == old_end || new_start == new_end)
        return HISTOGRAM_ANCHOR_NONE;
    if (classes->old_ids == NULL || classes->new_ids == NULL ||
        classes->class_count == 0)
        return HISTOGRAM_ANCHOR_ERROR;
    if (histogram_index_build(old_file, classes, old_start, old_end,
                              &index) != 0)
        return HISTOGRAM_ANCHOR_ERROR;

    /* NEW positions are scanned in order; the extended region may skip ahead. */
    for (size_t b = new_start; b < new_end;) {
        size_t b_next = b + 1;
        size_t new_id = classes->new_ids[b];
        if (new_id >= classes->class_count || new_id > ULONG_MAX) {
            result = HISTOGRAM_ANCHOR_ERROR;
            goto done;
        }
        size_t bucket = anchor_bucket_for_class(new_id, index.bucket_count);

        for (size_t rec_id = index.bucket_heads[bucket]; rec_id != SIZE_MAX;
             rec_id = index.records[rec_id].next_record) {
            const HistogramRecord *rec = &index.records[rec_id];
            if (rec->count > best_count) {
                if (!has_common && rec->class_id == new_id)
                    has_common = 1;
                continue;
            }
            if (rec->class_id != new_id)
                continue;
            has_common = 1;

            for (size_t a = rec->first_position; a != SIZE_MAX;) {
                /* Capture this link before extending a backward. */
                size_t next = index.next_occurrence[a - old_start];
                size_t as = a, ae = a, bs = b, be = b;
                size_t rarest = rec->count;

                while (as > old_start && bs > new_start &&
                       same_class(classes, as - 1, bs - 1)) {
                    --as;
                    --bs;
                    if (rarest > 1) {
                        size_t n = count_at(&index, as);
                        if (n < rarest)
                            rarest = n;
                    }
                }
                while (ae + 1 < old_end && be + 1 < new_end &&
                       same_class(classes, ae + 1, be + 1)) {
                    ++ae;
                    ++be;
                    if (rarest > 1) {
                        size_t n = count_at(&index, ae);
                        if (n < rarest)
                            rarest = n;
                    }
                }

                if (b_next <= be)
                    b_next = be + 1;
                size_t candidate_length = ae - as + 1;
                size_t best_length = best.old_end - best.old_start;
                if (candidate_length > best_length || rarest < best_count) {
                    best = (HistogramAnchor){
                        .old_start = as, .old_end = ae + 1,
                        .new_start = bs, .new_end = be + 1,
                        .min_old_count = rarest
                    };
                    best_count = rarest;
                }

                /* Skip OLD occurrences already subsumed by this extension. */
                while (next != SIZE_MAX && next <= ae)
                    next = index.next_occurrence[next - old_start];
                a = next;
            }
        }
        b = b_next;
    }

    if (has_common && best_count > 64)
        result = HISTOGRAM_ANCHOR_FALLBACK;
    else if (best.old_end > best.old_start) {
        *out = best;
        result = HISTOGRAM_ANCHOR_FOUND;
    }

done:
    histogram_index_free(&index);
    return result;
}
