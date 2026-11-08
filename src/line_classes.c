#include "line_classes.h"
#include <stdlib.h>
#include <stdint.h>

// find line in previously discovered classes
static size_t find_class_id(const FileData *file, size_t line_index, LineClass *classes, size_t class_count){
    
    for (size_t i = 0; i < class_count; i++){

        if (file_lines_equal(file, line_index, classes[i].file, classes[i].line_index) == true)
                return i;
    }
    return SIZE_MAX;
}

int line_classes_build(
    const FileData *old_file,
    const FileData *new_file,
    LineClasses *out
){
    if (old_file == NULL || new_file == NULL || out == NULL) return -1;

    int status = 0;

    *out = (LineClasses){0};

    // per line class ids
    size_t *old_ids = NULL, *new_ids = NULL;

    // array of previously discovered line classes
    LineClass *classes = NULL;

    // prevent memory overflow
    if (old_file->line_count > SIZE_MAX / sizeof *old_ids || new_file->line_count > SIZE_MAX / sizeof *new_ids){
        status = -1; 
        goto cleanup;
    }

    if (old_file->line_count > 0){
        old_ids = malloc(old_file->line_count * sizeof *old_ids);
        
        if (old_ids == NULL){
            status = -1; 
            goto cleanup;
        }
    }

    if (new_file->line_count > 0){
        new_ids = malloc(new_file->line_count * sizeof *new_ids);
        if (new_ids == NULL){
                status = -1; 
                goto cleanup;
        }
    }
    size_t class_count = 0, max_classes = old_file->line_count + new_file->line_count;
    
    if (max_classes > SIZE_MAX / sizeof *classes){
        status = -1; 
        goto cleanup;
    }
    
    if (max_classes > 0){
        classes = malloc(max_classes * sizeof *classes);
        if (classes == NULL){
            status = -1; 
            goto cleanup;
        }
    }

    for (size_t i = 0; i < old_file->line_count; i++){
        // check if line belongs to a previously discovered class
        size_t id = find_class_id(old_file, i, classes, class_count);
        if (id == SIZE_MAX){ // if not, add it to class array
            LineClass class;
            class.file = old_file;
            class.line_index = i;

            classes[class_count] = class;

            id = class_count;

            class_count++;
        }

        old_ids[i] = id;
    }

    for (size_t i = 0; i < new_file->line_count; i++){ 
        // check if line belongs to a previously discovered class
        size_t id = find_class_id(new_file, i, classes, class_count);
        if (id == SIZE_MAX){ // if not, add it to class array
            LineClass class;
            class.file = new_file;
            class.line_index = i;

            classes[class_count] = class;

            id = class_count;

            class_count++;
        }

        new_ids[i] = id;
    }

    out->old_ids = old_ids;
    out->new_ids = new_ids;
    out->class_count = class_count;

    cleanup:
        if (status == -1){
            free(old_ids);
            free(new_ids);
        }
        free(classes);

        return status;
}

void line_classes_free(LineClasses *classes){
    if (classes == NULL) return;

    free(classes->old_ids);
    classes->old_ids = NULL;

    free(classes->new_ids);
    classes->new_ids = NULL;

    classes->class_count = 0;
}