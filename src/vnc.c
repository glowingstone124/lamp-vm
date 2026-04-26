//
// Created by Max Wang on 2026/4/25.
//

#include "vnc.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arpa/inet.h"
#include "errno.h"
#include "panic.h"
#include "netinet/in.h"
#include "signal.h"
#include "stdint.h"
#include "stdio.h"
#include "sys/socket.h"

typedef struct {
    uint8_t bits_per_pixel;
    uint8_t depth;
    uint8_t big_endian;
    uint8_t true_color;
    uint16_t red_max;
    uint16_t green_max;
    uint16_t blue_max;
    uint8_t red_shift;
    uint8_t green_shift;
    uint8_t blue_shift;
} VncPixelFormat;

static const VncPixelFormat default_pixel_format = {
    .bits_per_pixel = 32,
    .depth = 24,
    .big_endian = 0,
    .true_color = 1,
    .red_max = 255,
    .green_max = 255,
    .blue_max = 255,
    .red_shift = 16,
    .green_shift = 8,
    .blue_shift = 0,
};

static int read_exact(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return -1;
        }
        p += n;
        len -= (size_t) n;
    }
    return 1;
}

static int write_exact(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE || errno == ECONNRESET) return -1;
            perror("send");
            return -1;
        }
        p += n;
        len -= (size_t) n;
    }
    return 0;
}

static void write_pixel_format(uint8_t *dst, const VncPixelFormat *format) {
    memset(dst, 0, 16);
    dst[0] = format->bits_per_pixel;
    dst[1] = format->depth;
    dst[2] = format->big_endian;
    dst[3] = format->true_color;

    uint16_t red_max = htons(format->red_max);
    uint16_t green_max = htons(format->green_max);
    uint16_t blue_max = htons(format->blue_max);
    memcpy(dst + 4, &red_max, 2);
    memcpy(dst + 6, &green_max, 2);
    memcpy(dst + 8, &blue_max, 2);

    dst[10] = format->red_shift;
    dst[11] = format->green_shift;
    dst[12] = format->blue_shift;
}

static void read_pixel_format(VncPixelFormat *format, const uint8_t *src) {
    format->bits_per_pixel = src[0];
    format->depth = src[1];
    format->big_endian = src[2];
    format->true_color = src[3];

    memcpy(&format->red_max, src + 4, 2);
    memcpy(&format->green_max, src + 6, 2);
    memcpy(&format->blue_max, src + 8, 2);
    format->red_max = ntohs(format->red_max);
    format->green_max = ntohs(format->green_max);
    format->blue_max = ntohs(format->blue_max);

    format->red_shift = src[10];
    format->green_shift = src[11];
    format->blue_shift = src[12];
}

static int pixel_format_bytes_per_pixel(const VncPixelFormat *format) {
    switch (format->bits_per_pixel) {
        case 8:
            return 1;
        case 16:
            return 2;
        case 32:
            return 4;
        default:
            return 0;
    }
}

static uint32_t scale_color(uint8_t value, uint16_t max) {
    return ((uint32_t)value * max + 127) / 255;
}

static void encode_pixel(uint8_t *out, uint32_t rgb, const VncPixelFormat *format) {
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(rgb & 0xFF);
    uint32_t pixel =
        (scale_color(r, format->red_max) << format->red_shift) |
        (scale_color(g, format->green_max) << format->green_shift) |
        (scale_color(b, format->blue_max) << format->blue_shift);

    int bytes_per_pixel = pixel_format_bytes_per_pixel(format);
    for (int i = 0; i < bytes_per_pixel; i++) {
        int shift = format->big_endian ? (bytes_per_pixel - 1 - i) * 8 : i * 8;
        out[i] = (uint8_t)((pixel >> shift) & 0xFF);
    }
}

static int vnc_format_is_native(const VncPixelFormat *format) {
    return format->bits_per_pixel == 32 &&
           format->depth == 24 &&
           format->big_endian == 0 &&
           format->true_color == 1 &&
           format->red_max == 255 &&
           format->green_max == 255 &&
           format->blue_max == 255 &&
           format->red_shift == 16 &&
           format->green_shift == 8 &&
           format->blue_shift == 0;
}

static int send_server_init(int client_fd) {
    uint8_t msg[24];
    memset(msg, 0, sizeof(msg));
    uint16_t w = htons(FB_WIDTH);
    uint16_t h = htons(FB_HEIGHT);
    memcpy(msg, &w, 2);
    memcpy(msg+2, &h, 2);
    write_pixel_format(msg + 4, &default_pixel_format);

    const char *name = "LampVM VNC Server";

    uint32_t name_len = htonl((uint32_t)strlen(name));
    memcpy(msg+20, &name_len, 4);

    if (write_exact(client_fd, msg, sizeof(msg)) < 0) return -1;
    if (write_exact(client_fd, name, strlen(name)) < 0) return -1;

    return 0;
}

static int send_framebuffer_update(
    VM *vm,
    int client_fd,
    uint16_t x,
    uint16_t y,
    uint16_t w,
    uint16_t h,
    const VncPixelFormat *format
) {
    if (vm == NULL || vm->fb == NULL) return -1;
    if (x >= FB_WIDTH || y >= FB_HEIGHT) return 0;
    if (x + w > FB_WIDTH) w = FB_WIDTH - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
    int bytes_per_pixel = pixel_format_bytes_per_pixel(format);
    if (!format->true_color || bytes_per_pixel == 0) {
        fprintf(stderr,
                "unsupported VNC pixel format: bpp=%u true_color=%u\n",
                format->bits_per_pixel,
                format->true_color);
        return -1;
    }

    uint8_t header[4];
    header[0] = 0;
    header[1] = 0;
    uint16_t rect_cnt = htons(1);
    memcpy(header+2, &rect_cnt, 2);
    if (write_exact(client_fd, header, sizeof(header)) < 0) return -1;
    uint8_t rect[12];
    uint16_t nx = htons(x);
    uint16_t ny = htons(y);
    uint16_t nw = htons(w);
    uint16_t nh = htons(h);
    uint32_t encoding_raw = htonl(0);
    memcpy(rect, &nx, 2);
    memcpy(rect + 2, &ny, 2);
    memcpy(rect + 4, &nw, 2);
    memcpy(rect + 6, &nh, 2);
    memcpy(rect + 8, &encoding_raw, 4);

    if (write_exact(client_fd, rect, sizeof(rect)) < 0) return -1;

    size_t row_bytes = (size_t) w * (size_t) bytes_per_pixel;
    uint8_t *row_buffer = malloc(row_bytes);
    if (row_buffer == NULL) {
        perror("Unable to allocate VNC row buffer");
        return -1;
    }

    int native_format = vnc_format_is_native(format);
    for (uint16_t row = 0; row < h; row++) {
        uint32_t *line = &vm->fb[(size_t)(y + row) * FB_WIDTH + x];
        vm_shared_lock(vm);
        if (native_format) {
            memcpy(row_buffer, line, row_bytes);
        } else {
            for (uint16_t col = 0; col < w; col++) {
                encode_pixel(row_buffer + (size_t) col * (size_t) bytes_per_pixel, line[col], format);
            }
        }
        vm_shared_unlock(vm);
        if (write_exact(client_fd, row_buffer, row_bytes) < 0) {
            free(row_buffer);
            return -1;
        }
    }

    free(row_buffer);
    return 0;
}

static int handle_client(VM *vm, int client_fd) {
    VncPixelFormat client_pixel_format = default_pixel_format;

    const char *version = "RFB 003.008\n"; //RFB
    if (write_exact(client_fd, version, 12) < 0) return -1;

    char client_version[12];
    int r = read_exact(client_fd, client_version, 12);
    if (r <= 0) return -1;

    uint8_t security_types[2] = {1, 1}; //TODO: security
    if (write_exact(client_fd, security_types, 2) < 0) return -1;
    uint8_t selected_security;
    r = read_exact(client_fd, &selected_security, 1);
    if (r <= 0) return -1;

    if (selected_security != 1) {
        fprintf(stderr, "client selected unsupported security type: %u\n", selected_security);
        return -1;
    }

    uint32_t security_result = htonl(0);
    if (write_exact(client_fd, &security_result, sizeof(security_result)) < 0) return -1;

    uint8_t shared_flag;
    r = read_exact(client_fd, &shared_flag, 1);
    if (r <= 0) return -1;

    if (send_server_init(client_fd) < 0) return -1;

    while (1) {
        uint8_t type;
        r = read_exact(client_fd, &type, 1);
        if (r == 0) {
            printf("Client disconnected\n");
            return 0;
        }
        if (r < 0) return -1;
        switch (type) {
            case 0: {
                //SetPixelFormat
                uint8_t payload[19];
                r = read_exact(client_fd, payload, 19);
                if (r <= 0) return -1;
                read_pixel_format(&client_pixel_format, payload + 3);
                break;
            }

            case 2: {
                //SetEncodings
                uint8_t head[3];
                r = read_exact(client_fd, head, sizeof(head));
                if (r <= 0) return -1;

                uint16_t n;
                memcpy(&n, head + 1, sizeof(n));
                n = ntohs(n);
                for (uint16_t i = 0; i < n; i++) {
                    uint8_t enc[4];
                    r = read_exact(client_fd, enc, 4);
                    if (r <= 0) return -1;
                }
                //TODO: use client-side's encoding, now using RAW.
                break;
            }
            case 3: {
                //FramebufferUpdateRequest
                uint8_t payload[9];
                r = read_exact(client_fd, payload, sizeof(payload));
                if (r <= 0) return -1;
                uint16_t x, y, w, h;
                memcpy(&x, payload + 1, 2);
                memcpy(&y, payload + 3, 2);
                memcpy(&w, payload + 5, 2);
                memcpy(&h, payload + 7, 2);

                x = ntohs(x);
                y = ntohs(y);
                w = ntohs(w);
                h = ntohs(h);

                if (send_framebuffer_update(vm, client_fd, x, y, w, h, &client_pixel_format) < 0) return -1;
                break;
            }
            case 4: {
                //KeyDownEvent
                uint8_t payload[7];
                r = read_exact(client_fd, payload, sizeof(payload));
                if (r <= 0) return -1;
                //TODO: handle keyboard input
                break;
            }
            case 5: {
                //PointerEvent
                uint8_t payload[5];
                r = read_exact(client_fd, payload, sizeof(payload));
                if (r <= 0) return -1;
                //TODO: handle mouse input
                break;
            }
            case 6: {
                //ClientCutText
                uint8_t head[7];
                r = read_exact(client_fd, head, sizeof(head));
                if (r <= 0) return -1;
                uint32_t len;
                memcpy(&len, head+3, 4);
                len = ntohl(len);

                char buffer[1024];
                while (len > 0) {
                    size_t chunk = len > sizeof(buffer) ? sizeof(buffer) : len;
                    r = read_exact(client_fd, buffer, chunk);
                    if (r <= 0) return -1;
                    len -= (uint32_t) chunk;
                }
                break;
            }

            default: {
                fprintf(stderr, "Unknown client type: %u\n", type);
                return -1;
            }
        }
    }
}

void vnc_exit(void) {
}

static void *vnc_main(void *arg) {
    VM* vm = arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        panic("vnc_main: socket", vm);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        panic("vnc_main: setsockopt", vm);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(VNC_PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        panic("vnc_main: bind", vm);
    }

    if (listen(server_fd, 8) < 0) {
        perror("listen");
        close(server_fd);
        panic("vnc_main: listen", vm);
    }

    printf("vnc_main: listening for connections on port %u\n", VNC_PORT);
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        printf("Client connected\n");

        handle_client(vm, client_fd);

        close(client_fd);
        printf("Client disconnected\n");
    }
    close(server_fd);
}

void vnc_run(VM* vm) {
    signal(SIGPIPE, SIG_IGN);
    if (pthread_create(&vm->vnc_server_thread, NULL, vnc_main, vm) != 0) {
        panic("Failed to create VNC worker", vm);
    }
}
