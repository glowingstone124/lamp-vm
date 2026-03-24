#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "loadbin.h"

#include "vm.h"

uint64_t *load_program(const char *filename, size_t *out_size) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("fopen");
        vm_error("Error opening program binary.");
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

uint8_t *load_data(const char *filename, size_t *out_size) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *data = malloc(file_size);
    if (!data) {
        perror("malloc");
        fclose(fp);
        return NULL;
    }

    size_t read_count = fread(data, 1, file_size, fp);
    if (read_count != file_size) {
        perror("fread");
        free(data);
        fclose(fp);
        return NULL;
    }

    fclose(fp);
    *out_size = file_size;
    return data;
}

static uint32_t parse_u32(const char *s) {
    if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0) {
        return (uint32_t)strtoul(s + 2, NULL, 16);
    }
    return (uint32_t)strtoul(s, NULL, 10);
}

int load_layout(const char *filename, ProgramLayout *out_layout) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return 0;
    }

    memset(out_layout, 0, sizeof(*out_layout));
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        char *newline = strchr(val, '\n');
        if (newline) {
            *newline = '\0';
        }

        if (strcmp(key, "TEXT_BASE") == 0) {
            out_layout->text_base = parse_u32(val);
        } else if (strcmp(key, "TEXT_SIZE") == 0) {
            out_layout->text_size = parse_u32(val);
        } else if (strcmp(key, "DATA_BASE") == 0) {
            out_layout->data_base = parse_u32(val);
        } else if (strcmp(key, "DATA_SIZE") == 0) {
            out_layout->data_size = parse_u32(val);
        } else if (strcmp(key, "BSS_BASE") == 0) {
            out_layout->bss_base = parse_u32(val);
        } else if (strcmp(key, "BSS_SIZE") == 0) {
            out_layout->bss_size = parse_u32(val);
        }
    }

    fclose(fp);
    return 1;
}

static uint32_t read_u32_le(const uint8_t bytes[4]) {
    return (uint32_t)bytes[0]
        | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16)
        | ((uint32_t)bytes[3] << 24);
}

int load_program_single(const char *filename,
                        uint64_t **out_program,
                        size_t *out_program_size,
                        uint8_t **out_data,
                        size_t *out_data_size,
                        ProgramLayout *out_layout) {
    if (!out_program || !out_program_size || !out_data || !out_data_size || !out_layout) {
        return 0;
    }

    *out_program = NULL;
    *out_data = NULL;
    *out_program_size = 0;
    *out_data_size = 0;
    memset(out_layout, 0, sizeof(*out_layout));

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("fopen");
        return 0;
    }

    uint8_t header[24];
    size_t read_count = fread(header, 1, sizeof(header), fp);
    if (read_count != sizeof(header)) {
        perror("fread");
        fclose(fp);
        return 0;
    }

    out_layout->text_base = read_u32_le(&header[0]);
    out_layout->text_size = read_u32_le(&header[4]);
    out_layout->data_base = read_u32_le(&header[8]);
    out_layout->data_size = read_u32_le(&header[12]);
    out_layout->bss_base = read_u32_le(&header[16]);
    out_layout->bss_size = read_u32_le(&header[20]);

    if (out_layout->text_size % sizeof(uint64_t) != 0) {
        fprintf(stderr, "TEXT_SIZE not aligned to 8 bytes\n");
        fclose(fp);
        return 0;
    }

    size_t text_bytes = out_layout->text_size;
    size_t data_bytes = out_layout->data_size;
    size_t program_size = text_bytes / sizeof(uint64_t);

    if (text_bytes > 0) {
        uint64_t *program = malloc(text_bytes);
        if (!program) {
            perror("malloc");
            fclose(fp);
            return 0;
        }
        if (fread(program, 1, text_bytes, fp) != text_bytes) {
            perror("fread");
            free(program);
            fclose(fp);
            return 0;
        }
        *out_program = program;
        *out_program_size = program_size;
    }

    if (data_bytes > 0) {
        uint8_t *data = malloc(data_bytes);
        if (!data) {
            perror("malloc");
            free(*out_program);
            *out_program = NULL;
            *out_program_size = 0;
            fclose(fp);
            return 0;
        }
        if (fread(data, 1, data_bytes, fp) != data_bytes) {
            perror("fread");
            free(data);
            free(*out_program);
            *out_program = NULL;
            *out_program_size = 0;
            fclose(fp);
            return 0;
        }
        *out_data = data;
        *out_data_size = data_bytes;
    }

    fclose(fp);
    return 1;
}
