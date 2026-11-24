#include "histogram_output.h"

#include <stdio.h>
#include <string.h>

#ifdef _OPENMP
#include <errno.h>
#include <inttypes.h>

/* Reject signs, whitespace, empty values, zero and out-of-range numbers. */
static int parse_positive(const char *s, uintmax_t limit, uintmax_t *out)
{
    if (*s == '\0') return -1;
    for (const char *p = s; *p; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end;
    uintmax_t n = strtoumax(s, &end, 10);
    if (errno != 0 || *end != '\0' || n == 0 || n > limit) return -1;
    *out = n;
    return 0;
}
#endif

/*
 * Histogram-diff command-line entry point. Serial is the default.
 * Usage: histdiff [--threads N [--cutoff LINES] [--diagnostics]] OLD NEW
 * Exit: 0 for identical inputs, 1 for differences, 2 for errors.
 * Prints unified hunks without file headers (the project's oracle contract).
 */
int main(int argc, char *argv[])
{
    FileData old_file = {0};
    FileData new_file = {0};
    LineClasses classes = {0};
    HistogramChangeFlags flags = {0};
    HistogramEditScript script = {0};
    HistogramRange pending = {0};
    int status = 2;

    int first = 1;
#ifdef _OPENMP
    int threads = 0, diagnostics = 0, cutoff_set = 0;
    size_t cutoff = HISTOGRAM_DEFAULT_CUTOFF;
    HistogramParallelStats stats = {0};
    /* Parse only options before the final two paths; OLD NEW remains valid
     * even when a filename starts with '--'. */
    while (argc - first > 2) {
        uintmax_t value;
        if (strcmp(argv[first], "--threads") == 0 && !threads && argc-first >= 4 &&
            parse_positive(argv[first+1], HISTOGRAM_MAX_THREADS, &value) == 0) {
            threads = (int)value;
            first += 2;
        } else if (strcmp(argv[first], "--cutoff") == 0 && !cutoff_set && argc-first >= 4 &&
                   parse_positive(argv[first+1], SIZE_MAX, &value) == 0) {
            cutoff = (size_t)value;
            cutoff_set = 1;
            first += 2;
        } else if (strcmp(argv[first], "--diagnostics") == 0 && !diagnostics) {
            diagnostics = 1;
            ++first;
        } else {
            goto usage;
        }
    }
    if (argc - first != 2 || ((cutoff_set || diagnostics) && !threads)) goto usage;
#else
    if (argc != 3) goto usage;
#endif

    if (file_load(argv[first], &old_file) != 0 ||
        file_load(argv[first+1], &new_file) != 0 ||
        file_split_lines(&old_file) != 0 ||
        file_split_lines(&new_file) != 0 ||
        line_classes_build(&old_file, &new_file, &classes) != 0 ||
        histogram_change_flags_init(&flags, old_file.line_count,
                                    new_file.line_count) != 0) {
        fputs("histdiff: failed to prepare input\n", stderr);
        goto cleanup;
    }

    HistogramPartitionStatus partition_status;
#ifdef _OPENMP
    if (threads != 0) {
        partition_status = histogram_partition_parallel(
            &old_file, &new_file, &classes, &flags, histogram_myers_fallback,
            NULL, &pending, threads, cutoff, diagnostics ? &stats : NULL);
        if (diagnostics)
            fprintf(stderr, "histdiff: requested=%d actual=%d cutoff=%zu tasks=%zu task_workers=%d region_workers=%d\n",
                    threads, stats.actual_threads, cutoff, stats.explicit_tasks,
                    stats.task_workers, stats.region_workers);
    } else
#endif
    {
        partition_status = histogram_partition_all(&old_file, &new_file, &classes,
                                                  &flags, histogram_myers_fallback,
                                                  NULL, &pending);
    }
    if (partition_status != HISTOGRAM_PARTITION_OK ||
        histogram_compact(&old_file, &new_file, &classes, &flags) != 0 ||
        histogram_build_script(&flags, &script) != 0) {
        fputs("histdiff: failed to compute diff\n", stderr);
        goto cleanup;
    }

    if (histogram_emit_unified(stdout, &old_file, &new_file, &script) != 0 ||
        fflush(stdout) != 0) {
        fputs("histdiff: failed to write patch\n", stderr);
        goto cleanup;
    }

    status = script.count != 0 ? 1 : 0;

cleanup:
    histogram_script_free(&script);
    histogram_change_flags_free(&flags);
    line_classes_free(&classes);
    file_free(&old_file);
    file_free(&new_file);
    return status;

usage:
#ifdef _OPENMP
    fputs("Usage: histdiff [--threads N [--cutoff LINES] [--diagnostics]] OLD NEW\n", stderr);
#else
    fputs("Usage: histdiff OLD NEW (build with -fopenmp for parallel mode)\n", stderr);
#endif
    return 2;
}
