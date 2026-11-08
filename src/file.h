#ifndef FILE_H
#define FILE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LINE_END_NONE,
    LINE_END_LF,
    LINE_END_CRLF
} LineEnding;

typedef struct {
    size_t offset; // Start of content in FileData.bytes
    size_t length; // Content length, excluding line endings
    LineEnding ending;
} Line;

typedef struct {
    unsigned char *bytes; // Owned file buffer
    size_t byte_count;
    Line *lines; // Owned array of line descriptors
    size_t line_count;
} FileData;

int file_load(const char *path, FileData *out); // gets bytes and byte count

int file_split_lines(FileData *file); // gets lines and line count

uint64_t line_hash(const unsigned char *bytes, size_t length); // hashes a line for != comparison

bool file_lines_equal(  // two different lines might have the same hash; requires exact check
    const FileData *a, size_t index_a,
    const FileData *b, size_t index_b
);

void file_free(FileData *file);

#endif