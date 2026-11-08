#include "file.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char *argv[]) {

    if (argc != 3) {
        fprintf(stderr, "Usage: histdiff OLD NEW\n");
        return 2;
    }

    FileData old = {0}, new = {0};

    // hashed line arrays
    uint64_t *hashed_old = NULL;
    uint64_t *hashed_new = NULL;
    int status = 1;

    // load files to FileData objects
    if (file_load(argv[1], &old) != 0 || file_load(argv[2], &new) != 0) {
        goto cleanup;
    }

    // Split lines in FileData objects
    if (file_split_lines(&old) != 0 || file_split_lines(&new) != 0) {
        fprintf(stderr, "Error: failed to split lines\n");
        goto cleanup;
    }

    // prevent multiplication overflow
    if (old.line_count > SIZE_MAX / sizeof *hashed_old || new.line_count > SIZE_MAX / sizeof *hashed_new) {
        fprintf(stderr, "Error: hash array size overflow\n");
        goto cleanup;
    }

    if (old.line_count > 0) {
        hashed_old = malloc(old.line_count * sizeof *hashed_old);

        if (hashed_old == NULL) {
            fprintf(stderr, "Error: old hash allocation failed\n");
            goto cleanup;
        }
    }

    if (new.line_count > 0) {
        hashed_new = malloc(new.line_count * sizeof *hashed_new);

        if (hashed_new == NULL) {
            fprintf(stderr, "Error: new hash allocation failed\n");
            goto cleanup;
        }
    }

    for (size_t i = 0; i < old.line_count; i++){
        Line line = old.lines[i];
        hashed_old[i] = line_hash(old.bytes + line.offset, line.length);
    }

    for (size_t i = 0; i < new.line_count; i++){
        Line line = new.lines[i];
        hashed_new[i] = line_hash(new.bytes + line.offset, line.length);
    }

    status = 0;

    cleanup:
        file_free(&old);
        file_free(&new);

        free(hashed_old);
        free(hashed_new);

        return status;
}