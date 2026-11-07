#include "file.h"
#include <stdio.h>

int main(int argc, char *argv[]) {

    if (argc != 3) {
        fprintf(stderr, "Usage: histdiff OLD NEW\n");
        return 2;
    }

    printf("OLD: %s\nNEW: %s\n", argv[1], argv[2]);
    return 0;
}