#include "file.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

int file_load(const char *path, FileData *out){
    // open file and read in binary
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
    perror(path);
    return -1;
    }

    // move pointer to end of file; returns 0 on success
    if (fseek(file, 0, SEEK_END) != 0) {
    perror("fseek");
    fclose(file);
    return -1;
    }

    // current pointer position in bytes; used to get file size in bytes
    long size = ftell(file);

    if (size < 0){
        perror("ftell");
        fclose(file);
        return -1;
    }

    // convert size from long to size_t
    if ((uintmax_t)size > SIZE_MAX) {
    fclose(file);
    return -1;
    }

    size_t byte_count = (size_t)size;

    // return pointer to origin
    if (fseek(file, 0, SEEK_SET) != 0) {
    perror("fseek");
    fclose(file);
    return -1;
    }

    // allocate memory
    unsigned char *buffer = NULL;

    if (byte_count > 0) {
        buffer = malloc(byte_count);

        if (buffer == NULL) {
            fclose(file);
            return -1;
        }
    }

    // read bytes to buffer
    if (buffer != NULL) {
        size_t bytes_read = fread(buffer, 1, byte_count, file);

        if (bytes_read != byte_count) {
            free(buffer);
            buffer = NULL;
            fclose(file);
            return -1;
        }
    }
    
    // close the file
    if (fclose(file) != 0) {
        perror("fclose");
        free(buffer);
        return -1;
    }
    
    out->bytes = buffer;
    out->byte_count = byte_count;
    out->lines=NULL;
    out->line_count = 0;

    return 0;
}



int file_split_lines(FileData *file){
    if (file == NULL){ // null file object passed
        return -1;
    }

    if (file->lines != NULL || file->line_count != 0) { // lines is already constructed 
        return -1;
    }

    if (file->byte_count > 0 && file->bytes == NULL)
    return -1;

    if (file->byte_count == 0) { // empty file
        return 0;
    }

    Line *lines;
    size_t line_count = // if last line is not terminated, add the line
        file->bytes[file->byte_count - 1] == '\n' ? 0 : 1;
    size_t offset = 0;

    size_t current_line = 0;

    for (size_t i = 0; i < file->byte_count; i++) {
        char byte = file->bytes[i];

        if (byte == '\n')
                line_count++;
    }

    // memory allocation
    if (line_count > SIZE_MAX / sizeof *lines) return -1;

    lines = malloc(line_count * sizeof *lines); // bytes of lines object times number of lines

    if (lines == NULL) return -1;

    for (size_t i = 0; i < file->byte_count; i++) {
        unsigned char byte = file->bytes[i];

        if (byte == '\n'){
            LineEnding ending = LINE_END_LF;
            size_t line_length = i - offset;

            if (line_length > 0 && file->bytes[i - 1] == '\r') { // exclude CR from CRLF line content
                line_length--;
                ending = LINE_END_CRLF;
            }

            lines[current_line] = (Line){offset, line_length, ending};
            current_line++;

            offset = i + 1;
        }
        else if (i == file->byte_count - 1){ // end of file has no \n
            lines[current_line] = (Line){offset, i - offset + 1, LINE_END_NONE};
            current_line++;

            offset = i + 1;
        }
    }

    file->lines = lines;
    file->line_count = line_count;

    return 0;
}


// FNV-1a 64-bit hashing; initial hash and prime are standard
uint64_t line_hash(const unsigned char *bytes, size_t length){
    uint64_t hash = UINT64_C(14695981039346656037);
    const uint64_t prime = UINT64_C(1099511628211);

    for (size_t i = 0; i < length; i++){
        hash ^= bytes[i];
        hash *= prime;
    }

    return hash;
}



bool file_lines_equal(  // two different lines might have the same hash; requires exact check
    const FileData *a, size_t index_a,
    const FileData *b, size_t index_b){
    
    if (a == NULL || b == NULL) return false;
    if (a->lines == NULL || b->lines == NULL) return false;

    if (index_a >= a->line_count || index_b >= b->line_count) return false;

    const Line *line_a = &a->lines[index_a];
    const Line *line_b = &b->lines[index_b];

    if (line_a->length != line_b->length) return false;
    if (line_a->ending != line_b->ending) return false;

    if (line_a->length == 0)
    return true;

    return memcmp(a->bytes + line_a->offset, b->bytes + line_b->offset, line_a->length) == 0;
}