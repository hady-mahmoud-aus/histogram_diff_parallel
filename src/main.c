#include "histogram_output.h"

#include <stdio.h>

/*
 * Serial histogram-diff command-line entry point.
 * Usage: histdiff OLD NEW
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

    if (argc != 3) {
        fputs("Usage: histdiff OLD NEW\n", stderr);
        return 2;
    }

    if (file_load(argv[1], &old_file) != 0 ||
        file_load(argv[2], &new_file) != 0 ||
        file_split_lines(&old_file) != 0 ||
        file_split_lines(&new_file) != 0 ||
        line_classes_build(&old_file, &new_file, &classes) != 0 ||
        histogram_change_flags_init(&flags, old_file.line_count,
                                    new_file.line_count) != 0 ||
        histogram_partition_all(&old_file, &new_file, &classes, &flags,
                                histogram_myers_fallback, NULL, &pending) !=
                                    HISTOGRAM_PARTITION_OK ||
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
}
