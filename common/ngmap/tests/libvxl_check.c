/* ngmap_decode_libvxl (ngmap_libvxl.h) against what it replaced:
   ngmap_decode's VXL bytes parsed by libvxl_create. The two maps must be the
   same, cell for cell and block for block (the same sorted colour list in
   every chunk), so a map loaded either way plays the same.
     libvxl_check <map.ngmap>...
   A 512 x 512 x 64 .vxl is checked against libvxl_write instead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ngmap_libvxl.h"
#include <vxlwrite/vxlwrite.h>

static uint8_t* read_file(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = (uint8_t*) malloc(n > 0 ? (size_t) n : 1);
    *size         = data && n > 0 ? fread(data, 1, (size_t) n, f) : 0;
    fclose(f);
    return data;
}

static int solid(struct libvxl_map* m, size_t x, size_t y, size_t z)
{
    return libvxl_map_issolid(m, (int) x, (int) y, (int) z);
}

static int check(const char* path)
{
    size_t   size;
    uint8_t* data = read_file(path, &size);
    if (!data) {
        printf("FAIL %s: unreadable\n", path);
        return 1;
    }
    uint8_t* vxl;
    size_t   vxl_size;
    int      w, h, d;
    if (ngmap_decode(data, size, &vxl, &vxl_size, &w, &h, &d) != NGMAP_OK) {
        printf("FAIL %s: ngmap_decode\n", path);
        return 1;
    }
    struct libvxl_map a, b;
    if (!libvxl_create(&a, (size_t) w, (size_t) h, (size_t) d, vxl, vxl_size)) {
        printf("FAIL %s: libvxl_create\n", path);
        return 1;
    }
    int err = ngmap_decode_libvxl(data, size, &b, w, h, d);
    if (err != NGMAP_OK) {
        printf("FAIL %s: ngmap_decode_libvxl %s\n", path, ngmap_strerror(err));
        return 1;
    }
    int bad = 0;
    for (size_t y = 0; y < (size_t) h && !bad; y++) {
        for (size_t x = 0; x < (size_t) w && !bad; x++) {
            for (size_t z = 0; z < (size_t) d; z++) {
                if (solid(&a, x, y, z) != solid(&b, x, y, z)) {
                    printf("FAIL %s: cell %zu,%zu,%zu solid %d against %d\n", path, x, y, z, solid(&a, x, y, z),
                           solid(&b, x, y, z));
                    bad = 1;
                    break;
                }
            }
        }
    }
    size_t chunks = (((size_t) w + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE) *
                    (((size_t) h + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE);
    size_t blocks = 0, cap_a = 0, cap_b = 0;
    for (size_t k = 0; k < chunks && !bad; k++) {
        cap_a += a.chunks[k].length;
        cap_b += b.chunks[k].length;
        blocks += a.chunks[k].index;
        if (a.chunks[k].index != b.chunks[k].index ||
            memcmp(a.chunks[k].blocks, b.chunks[k].blocks, a.chunks[k].index * sizeof(struct libvxl_block)) != 0) {
            printf("FAIL %s: chunk %zu has %zu blocks against %zu, or other ones\n", path, k, a.chunks[k].index,
                   b.chunks[k].index);
            bad = 1;
        }
    }
    /* written back (common/vxlwrite, a 0.75 join, a VXL transfer, the
       client's F9) and read again: the same solid cells, the same colour in
       every one of them */
    size_t written = 0;
    if (!bad) {
        uint8_t* out = (uint8_t*) malloc(vxlwrite_bound(&b));
        written      = vxlwrite_map(&b, out);
        struct libvxl_map c;
        if (written == 0 || !libvxl_create(&c, (size_t) w, (size_t) h, (size_t) d, out, written)) {
            printf("FAIL %s: vxlwrite's %zu bytes do not read back\n", path, written);
            bad = 1;
        } else {
            for (size_t y = 0; y < (size_t) h && !bad; y++) {
                for (size_t x = 0; x < (size_t) w && !bad; x++) {
                    for (size_t z = 0; z < (size_t) d; z++) {
                        uint32_t cb = libvxl_map_get(&b, (int) x, (int) y, (int) z) & 0xFFFFFFu;
                        uint32_t cc = libvxl_map_get(&c, (int) x, (int) y, (int) z) & 0xFFFFFFu;
                        if (solid(&b, x, y, z) != solid(&c, x, y, z) || cb != cc) {
                            printf("FAIL %s: written back, cell %zu,%zu,%zu is %d/%06x against %d/%06x\n", path, x, y,
                                   z, solid(&c, x, y, z), cc, solid(&b, x, y, z), cb);
                            bad = 1;
                            break;
                        }
                    }
                }
            }
            libvxl_free(&c);
        }
        free(out);
    }
    if (!bad) {
        printf("ok   %s: %dx%dx%d, %zu colour blocks, capacity %zu against libvxl_create's %zu, VXL %zu written against %zu\n",
               path, w, h, d, blocks, cap_b, cap_a, written, vxl_size);
    }
    libvxl_free(&a);
    libvxl_free(&b);
    free(vxl);
    free(data);
    return bad;
}

/* a VXL file (512 x 512 x 64): vxlwrite must write what libvxl_write
   writes, byte for byte, where libvxl can (no column without a solid cell) */
static int check_vxl(const char* path)
{
    size_t   size;
    uint8_t* data = read_file(path, &size);
    struct libvxl_map m;
    if (!data || !libvxl_create(&m, 512, 512, 64, data, size)) {
        printf("FAIL %s: not a 512x512x64 VXL\n", path);
        free(data);
        return 1;
    }
    int empty = 0;
    for (int y = 0; y < 512 && !empty; y++) {
        for (int x = 0; x < 512 && !empty; x++) {
            int any = 0;
            for (int z = 0; z < 64 && !any; z++) {
                any = libvxl_map_issolid(&m, x, y, z);
            }
            empty = !any;
        }
    }
    uint8_t* ours   = (uint8_t*) malloc(vxlwrite_bound(&m));
    size_t   n_ours = vxlwrite_map(&m, ours), n_theirs = 0;
    int      bad    = 0;
    if (!empty) {
        uint8_t* theirs = (uint8_t*) malloc(vxlwrite_bound(&m));
        libvxl_write(&m, theirs, &n_theirs);
        bad = n_ours != n_theirs || memcmp(ours, theirs, n_ours) != 0;
        free(theirs);
    }
    printf("%s %s: %zu bytes written, libvxl_write %s\n", bad ? "FAIL" : "ok  ", path, n_ours,
           empty ? "cannot (a column of air)" : bad ? "differs" : "the same");
    free(ours);
    libvxl_free(&m);
    free(data);
    return bad;
}

int main(int argc, char** argv)
{
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        failed += n > 4 && !strcmp(argv[i] + n - 4, ".vxl") ? check_vxl(argv[i]) : check(argv[i]);
    }
    printf("%d of %d maps differ\n", failed, argc - 1);
    return failed != 0;
}
