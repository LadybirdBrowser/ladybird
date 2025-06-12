/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size);

int fuzz_from_file(char const* filename)
{
    struct stat file_stats;

    if (stat(filename, &file_stats) < 0) {
        perror("EntryShim: Failed to stat the input file");
        return 1;
    }

    size_t file_size = file_stats.st_size;

    uint8_t* file_buffer = (uint8_t*)malloc(file_size);

    if (!file_buffer) {
        fprintf(stderr, "EntryShim: Failed to allocate file buffer\n");
        return 1;
    }

    int fd = open(filename, O_RDONLY);

    if (fd < 0) {
        perror("EntryShim: Failed to open the input file");
        return 1;
    }

    ssize_t bytes_read = read(fd, file_buffer, file_size);
    if (bytes_read < 0) {
        fprintf(stderr, "EntryShim: Failed to read the input file\n");
        return 1;
    }

    LLVMFuzzerTestOneInput(file_buffer, bytes_read);
    return 0;
}

int fuzz_from_stdin()
{
    size_t chunk_size = 4096;

    uint8_t* file_buffer = nullptr;
    size_t file_size = 0;

    while (true) {
        file_buffer = (uint8_t*)realloc(file_buffer, file_size + chunk_size);

        if (!file_buffer) {
            fprintf(stderr, "EntryShim: Failed to reallocate buffer to a size of %lu bytes\n", file_size + chunk_size);
            return 1;
        }

        ssize_t bytes_read = read(STDIN_FILENO, file_buffer + file_size, chunk_size);

        if (bytes_read < 0) {
            perror("EntryShim: Failed to read from stdin");
            return 1;
        }

        if (bytes_read == 0)
            break;

        file_size += bytes_read;
    }

    LLVMFuzzerTestOneInput(file_buffer, file_size);
    return 0;
}

extern "C" int main(int argc, char** argv)
{
    if (argc > 1)
        return fuzz_from_file(argv[1]);

    return fuzz_from_stdin();
}
