/*
 * ngmap_cli: one map file through the ngmap codec (ngmap.h).
 *
 * The home page's backdrop ships as ngmap instead of VXL (`make menu-map`,
 * menuworld.c decodes it at startup), and choosing the quality it ships at
 * means encoding the map at a few, decoding each back and comparing the
 * result with the original VXL. This is that tool, so the comparison runs
 * on exactly the codec the client and the server use, and so does anyone
 * who wants to look at one map without the game:
 *
 *   ngmap_cli encode <in.vxl> <out.ngmap> [quality] [w h d]
 *   ngmap_cli decode <in.ngmap> <out.vxl>
 *
 * quality is 1..100 (80 by default) and w h d the VXL's size (512 512 64,
 * the Ace of Spades map) for maps whose file does not say. Both halves
 * print the sizes and the time they took. Exit 1 on any failure: bad
 * arguments, a malformed file, or no room for the output.
 */
#include <ngmap/ngmap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Milliseconds on a monotonic clock: the tool exists to time the codec, so
   it must not care what the wall clock does (or in what units it ticks). */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void usage(void) {
    fprintf(stderr, "usage: ngmap_cli encode <in.vxl> <out.ngmap> [quality] [w h d]\n"
                    "       ngmap_cli decode <in.ngmap> <out.vxl>\n");
}

/* The whole file at once: a map is a couple of megabytes, and both the
   encoder and the decoder want the bytes contiguous anyway. */
static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ngmap_cli: %s: cannot open\n", path);
        return NULL;
    }
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "ngmap_cli: %s: cannot measure\n", path);
        fclose(f);
        return NULL;
    }
    uint8_t* buf = malloc(len > 0 ? (size_t)len : 1);
    if (!buf) {
        fprintf(stderr, "ngmap_cli: %s: out of memory\n", path);
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        fprintf(stderr, "ngmap_cli: %s: read %zu of %ld bytes\n", path, got, len);
        free(buf);
        return NULL;
    }
    *size = got;
    return buf;
}

static int write_file(const char* path, const uint8_t* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "ngmap_cli: %s: cannot create\n", path);
        return 0;
    }
    size_t wrote = fwrite(data, 1, size, f);
    int ok = wrote == size && fclose(f) == 0;
    if (!ok) {
        fprintf(stderr, "ngmap_cli: %s: wrote %zu of %zu bytes\n", path, wrote, size);
        remove(path);
    }
    return ok;
}

/* strtol with a refusal to guess: the quality and the dimensions are typed
   by hand once per run, so a typo has to be an error, not a zero. */
static int parse_int(const char* text, const char* what, long lo, long hi, int* out) {
    char* end = NULL;
    long v = strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "ngmap_cli: %s: %s must be a number %ld..%ld\n", what, text, lo, hi);
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int cmd_encode(int argc, char** argv) {
    if (argc != 4 && argc != 5 && argc != 8) {
        usage();
        return 1;
    }
    int quality = 80;
    int w = 512, h = 512, d = 64;
    if (argc >= 5 && !parse_int(argv[4], "quality", 1, 100, &quality))
        return 1;
    if (argc == 8 && (!parse_int(argv[5], "width", 1, NGMAP_MAX_WIDTH, &w)
                      || !parse_int(argv[6], "height", 1, NGMAP_MAX_HEIGHT, &h)
                      || !parse_int(argv[7], "depth", 1, NGMAP_MAX_DEPTH, &d)))
        return 1;

    size_t vxl_size = 0;
    uint8_t* vxl = read_file(argv[2], &vxl_size);
    if (!vxl)
        return 1;
    printf("ngmap_cli: %s: %zu bytes of VXL, %ix%ix%i, quality %i\n", argv[2], vxl_size, w, h, d, quality);

    uint8_t* out = NULL;
    size_t out_size = 0;
    double start = now_ms();
    int err = ngmap_encode(vxl, vxl_size, w, h, d, quality, &out, &out_size);
    double elapsed = now_ms() - start;
    free(vxl);
    if (err != NGMAP_OK) {
        fprintf(stderr, "ngmap_cli: encode: %s\n", ngmap_strerror(err));
        free(out);
        return 1;
    }

    int ok = write_file(argv[3], out, out_size);
    if (ok)
        printf("ngmap_cli: %s: %zu bytes (%.1f%% of the VXL) in %.0f ms\n", argv[3], out_size,
               100.0 * (double)out_size / (double)(vxl_size ? vxl_size : 1), elapsed);
    free(out);
    return ok ? 0 : 1;
}

static int cmd_decode(int argc, char** argv) {
    if (argc != 4) {
        usage();
        return 1;
    }
    size_t ngmap_size = 0;
    uint8_t* data = read_file(argv[2], &ngmap_size);
    if (!data)
        return 1;
    printf("ngmap_cli: %s: %zu bytes of ngmap\n", argv[2], ngmap_size);

    uint8_t* vxl = NULL;
    size_t vxl_size = 0;
    int w = 0, h = 0, d = 0;
    double start = now_ms();
    int err = ngmap_decode(data, ngmap_size, &vxl, &vxl_size, &w, &h, &d);
    double elapsed = now_ms() - start;
    free(data);
    if (err != NGMAP_OK) {
        fprintf(stderr, "ngmap_cli: decode: %s\n", ngmap_strerror(err));
        free(vxl);
        return 1;
    }

    int ok = write_file(argv[3], vxl, vxl_size);
    if (ok)
        printf("ngmap_cli: %s: %ix%ix%i, %zu bytes of VXL in %.0f ms\n", argv[3], w, h, d, vxl_size, elapsed);
    free(vxl);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "encode") == 0)
        return cmd_encode(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "decode") == 0)
        return cmd_decode(argc, argv);
    usage();
    return 1;
}
