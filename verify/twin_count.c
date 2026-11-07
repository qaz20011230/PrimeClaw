/*
 * twin_count.c — independent second decoder for the PRMGAP01 v1 dataset,
 * used to cross-check twin-prime pair counts at 10^9, 10^10, 10^11
 * (pairs (p, p+2) with p+2 <= x).  Written from the format spec only.
 *
 * Sequential decode of every block, carrying the previous prime across
 * block boundaries so that boundary gaps are included as well.
 *
 * exit: 0 all checkpoint twin counts match expectations, 1 mismatch/error
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { GLOBAL_HDR = 32, BLOCK_HDR = 48, MAX_CP = 8 };

static uint16_t rd16(const unsigned char *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const unsigned char *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | (uint64_t)p[i]; return v; }

static int parse_u64(const char *s, uint64_t *out)
{
    if (!s || !*s) return -1;
    char *e = NULL;
    unsigned long long v = strtoull(s, &e, 10);
    if (e == s) return -1;
    if (*e == 'e' || *e == 'E') {
        char *e2 = NULL;
        long ex = strtol(e + 1, &e2, 10);
        if (*e2 || ex < 0 || ex > 19) return -1;
        for (long i = 0; i < ex; i++) v *= 10;
    } else if (*e) return -1;
    *out = (uint64_t)v;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <dataset> [x1 x2 ...]\n", argv[0]); return 2; }
    const char *path = argv[1];
    uint64_t cp[MAX_CP], expect[MAX_CP];
    int ncp = 0;
    for (int i = 2; i < argc && ncp < MAX_CP; i++) {
        char *colon = strchr(argv[i], ':');
        if (!colon) { fprintf(stderr, "bad arg %s (want x:expected)\n", argv[i]); return 2; }
        *colon = 0;
        cp[ncp] = 0; expect[ncp] = 0;
        if (parse_u64(argv[i], &cp[ncp]) != 0 || parse_u64(colon + 1, &expect[ncp]) != 0) {
            fprintf(stderr, "bad arg %s\n", argv[i]); return 2;
        }
        ncp++;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 2; }
    uint64_t fsz = (uint64_t)st.st_size;
    void *map = mmap(NULL, (size_t)fsz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 2; }
    close(fd);
    const unsigned char *d = (const unsigned char *)map;

    if (fsz < GLOBAL_HDR || memcmp(d, "PRMGAP01", 8) != 0 ||
        rd32(d + 8) != 1 || rd32(d + 12) != 0 || rd64(d + 24) != 0) {
        fprintf(stderr, "bad global header\n"); return 1;
    }

    uint64_t off = GLOBAL_HDR;
    uint64_t total = 0, blocks = 0;
    uint64_t prev = 0;         /* last prime seen in the whole file */
    int have_prev = 0;
    uint64_t twins = 0;        /* pairs (p, p+2) completed so far */
    uint64_t twin_upper_last = 0;
    uint64_t cptwin[MAX_CP];
    memset(cptwin, 0, sizeof(cptwin));
    int c = 0;                 /* checkpoint pointer */
    uint64_t last_end = 0;

    while (off < fsz) {
        if (fsz - off < BLOCK_HDR) { fprintf(stderr, "incomplete trailing header\n"); return 1; }
        const unsigned char *b = d + off;
        if (memcmp(b, "PRMB", 4) != 0 || rd16(b + 4) != 1 || rd16(b + 6) != 0) {
            fprintf(stderr, "bad block %" PRIu64 " at offset %" PRIu64 "\n", blocks, off); return 1;
        }
        uint64_t bprev = rd64(b + 8), first = rd64(b + 16), end = rd64(b + 24);
        uint64_t count = rd64(b + 32), nbytes = rd64(b + 40);
        if (blocks == 0) {
            if (bprev != 0 || first != 2 || end != 2 || count != 1 || nbytes != 0) {
                fprintf(stderr, "bad first block\n"); return 1;
            }
        } else if (bprev != last_end) {
            fprintf(stderr, "prev linkage broken at block %" PRIu64 "\n", blocks); return 1;
        }
        if (nbytes > fsz - off - BLOCK_HDR) { fprintf(stderr, "payload past EOF in block %" PRIu64 "\n", blocks); return 1; }
        const unsigned char *cur = b + BLOCK_HDR;
        const unsigned char *pend = cur + nbytes;
        uint64_t p = first;
        for (uint64_t j = 0; j < count; j++) {
            if (j > 0) {
                uint64_t g = 0; int shift = 0; unsigned char byte;
                do {
                    if (cur >= pend) { fprintf(stderr, "payload short in block %" PRIu64 "\n", blocks); return 1; }
                    byte = *cur++;
                    if (shift >= 64) { fprintf(stderr, "varint overflow\n"); return 1; }
                    g |= ((uint64_t)(byte & 0x7F)) << shift;
                    shift += 7;
                } while (byte & 0x80);
                if (g < 2 || (g & 1)) { fprintf(stderr, "bad gap %" PRIu64 " at block %" PRIu64 "\n", g, blocks); return 1; }
                p += g;
            }
            /* checkpoint counts: pairs with upper prime <= cp, evaluated
               BEFORE adding a pair that ends at p (p > cp then implies its
               pair must not be counted for cp) */
            while (c < ncp && p > cp[c]) { cptwin[c] = twins; c++; }
            if (have_prev && p - prev == 2) { twins++; twin_upper_last = p; }
            prev = p;
            have_prev = 1;
            total++;
        }
        if (cur != pend || p != end) { fprintf(stderr, "block %" PRIu64 " payload/end mismatch\n", blocks); return 1; }
        last_end = end;
        blocks++;
        off += BLOCK_HDR + nbytes;
    }
    if (off != fsz) { fprintf(stderr, "trailing bytes\n"); return 1; }
    for (; c < ncp; c++) cptwin[c] = twins;

    printf("DATASET            : %s\n", path);
    printf("FILE_SIZE          : %" PRIu64 " bytes\n", fsz);
    printf("BLOCKS             : %" PRIu64 "\n", blocks);
    printf("TOTAL_PRIMES       : %" PRIu64 "\n", total);
    printf("LAST_PRIME         : %" PRIu64 "\n", last_end);
    printf("TWIN_PAIRS_TOTAL   : %" PRIu64 " (all (p,p+2); last upper prime %" PRIu64 ")\n",
           twins, twin_upper_last);
    int fail = 0;
    for (int i = 0; i < ncp; i++) {
        int ok = 1;
        if (expect[i] != 0) ok = (cptwin[i] == expect[i]);
        else ok = 1;
        printf("TWIN_PAIRS<=%-7" PRIu64 " : %" PRIu64 " (expected %" PRIu64 ") %s\n",
               cp[i], cptwin[i], expect[i], ok ? "PASS" : "FAIL");
        if (!ok) fail = 1;
    }
    printf("RESULT             : %s\n", fail ? "FAIL" : "PASS");
    munmap(map, (size_t)fsz);
    return fail ? 1 : 0;
}
