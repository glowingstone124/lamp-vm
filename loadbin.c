#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

uint64_t* load_program(const char* filename, size_t *out_size)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("fopen");
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint64_t* program = malloc(file_size);
    if (!program) {
        perror("malloc");
        fclose(fp);
        return NULL;
    }
    size_t read_count = fread(program, 1, file_size, fp);
    if (read_count != file_size) {
        perror("fread");
        free(program);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_size = file_size / sizeof(uint64_t);
    return program;
}