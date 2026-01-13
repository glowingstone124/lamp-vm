#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

uint64_t *load_program(const char *filename, size_t *out_size) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("fopen");
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size % sizeof(uint64_t) != 0) {
        fprintf(stderr, "Program file size not aligned to 8 bytes\n");
        fclose(fp);
        return NULL;
    }

    size_t num_insts = file_size / sizeof(uint64_t);
    uint64_t *program = malloc(file_size);
    if (!program) {
        perror("malloc");
        fclose(fp);
        return NULL;
    }

    size_t read_count = fread(program, sizeof(uint64_t), num_insts, fp);
    if (read_count != num_insts) {
        perror("fread");
        free(program);
        fclose(fp);
        return NULL;
    }

    fclose(fp);
    *out_size = num_insts;
    return program;
}
