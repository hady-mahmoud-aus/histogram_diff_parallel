
#ifndef LINE_CLASSES_H
#define LINE_CLASSES_H

#include "file.h"

typedef struct {
        const FileData *file;
        size_t line_index;
    } LineClass;

typedef struct {
    size_t *old_ids;
    size_t *new_ids;
    size_t class_count;
} LineClasses;

int line_classes_build(
    const FileData *old_file,
    const FileData *new_file,
    LineClasses *out
);

void line_classes_free(LineClasses *classes);

#endif
