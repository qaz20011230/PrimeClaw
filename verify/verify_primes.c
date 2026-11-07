/*
 * verify_primes.c — fully independent verifier for the "PRMGAP01" v1
 * prime-gap dataset format.
 *
 * Written from the written format specification alone (no code from
 * src or tools was read or reused).
 *
 * Format (little-endian):
 *   global header, 32 bytes: magic[8]="PRMGAP01", u32 version=1, u32 flags=0,
 *                            u64 chunk_odd (informational), u64 reserved=0
 *   block, 48-byte header + payload:
 *     magic[4]="PRMB", u16 version=1, u16 flags=0,
 *     u64 prev_prime, u64 first_prime, u64 end_prime, u64 count, u64 nbytes
 *   payload = exactly (count-1) unsigned LEB128 gaps between consecutive
 *   primes inside the block (first prime is in the header).
 *   Very first block: prev=0, first=end=2, count=1, nbytes=0.
 *
 * Checks performed:
 *   - global header fields
 *   - block chain: magic/version/flags, prev_prime linkage, payload bounds,
 *     first block shape, per-block first/end/count/nbytes consistency,
 *     minimum/maximum possible payload sizes, parity of odd primes
 *   - full payload decode: exact LEB128 consumption, strictly increasing
 *     integers, gaps even and >=2, decoded count == header count,
 *     last decoded == end_prime
 *   - cross-block gap parity/value and payload/header coverage of the file
 *   - pi(10^k) checkpoint counts (compared against values passed in)
 *   - block first/end primality via deterministic Miller-Rabin
 *   - N random decoded primes tested prime via Miller-Rabin (bases 2..37,
 *     deterministic for n < 3.3e24, hence for all 64-bit inputs)
 *   - top-K largest gaps: endpoints prime and every odd number strictly
 *     between the two primes composite (=> the endpoints really are
 *     consecutive primes), K configurable
 *   - optional dump of all decoded primes (small datasets)
 *
 * Exit codes: 0 PASS, 1 FAIL/corrupt, 2 usage/IO, 3 incomplete trailing block
 * (benign for an append-only file that is mid-write; must not appear in a
 * final snapshot).
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    GLOBAL_HDR_SIZE = 32,
    BLOCK_HDR_SIZE  = 48,
    MAX_THREADS     = 8,
    MAX_K           = 8192,
    MAX_CP          = 64
};

typedef unsigned __int128 u128;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "fatal: out of memory (%zu bytes)\n", n); exit(2); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "fatal: out of memory (%zu bytes)\n", n); exit(2); }
    return q;
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | (uint64_t)p[i];
    return v;
}

/* ------------------------------------------------------------------ */
/* deterministic Miller-Rabin, bases 2,3,5,...,37                     */
/* deterministic for n < 3.3e24  (Sorenson & Webster) => safe for u64 */
/* ------------------------------------------------------------------ */

static const uint64_t MR_BASES[12] = {
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37
};

static uint64_t mulmod64(uint64_t a, uint64_t b, uint64_t m)
{
    return (uint64_t)(((u128)a * (u128)b) % (u128)m);
}

static uint64_t powmod64(uint64_t a, uint64_t e, uint64_t m)
{
    uint64_t r = 1 % m;
    a %= m;
    while (e) {
        if (e & 1) r = mulmod64(r, a, m);
        a = mulmod64(a, a, m);
        e >>= 1;
    }
    return r;
}

static int is_prime_u64(uint64_t n)
{
    if (n < 2) return 0;
    for (int i = 0; i < 12; i++) {
        if (n == MR_BASES[i]) return 1;
        if (n % MR_BASES[i] == 0) return 0;
    }
    uint64_t d = n - 1;
    int s = 0;
    while ((d & 1) == 0) { d >>= 1; s++; }
    for (int i = 0; i < 12; i++) {
        uint64_t a = MR_BASES[i] % n;
        if (a == 0) continue;
        uint64_t x = powmod64(a, d, n);
        if (x == 1 || x == n - 1) continue;
        int witness_ok = 0;
        for (int r = 1; r < s; r++) {
            x = mulmod64(x, x, n);
            if (x == n - 1) { witness_ok = 1; break; }
        }
        if (!witness_ok) return 0;
    }
    return 1;
}

/* slow, independent trial-division primality for MR self-test */
static int trial_prime(uint64_t n)
{
    if (n < 2) return 0;
    if (n % 2 == 0) return n == 2;
    for (uint64_t d = 3; d <= n / d; d += 2)
        if (n % d == 0) return 0;
    return 1;
}

static int mr_selftest(uint64_t limit)
{
    int fails = 0;
    if (limit > 5000000ULL) limit = 5000000ULL;

    for (uint64_t n = 2; n <= limit; n++) {
        int t = trial_prime(n);
        int m = is_prime_u64(n);
        if (t != m) {
            printf("MR_SELFTEST_FAIL n=%" PRIu64 " trial=%d mr=%d\n", n, t, m);
            fails++;
            if (fails > 20) break;
        }
    }
    printf("MR_SELFTEST_RANGE  : n=2..%" PRIu64 " compared with trial division: %s\n",
           limit, fails ? "FAIL" : "all match");

    struct { uint64_t n; int prime; const char *why; } vec[] = {
        {0, 0, "zero"}, {1, 0, "one"}, {2, 1, "two"}, {3, 1, "three"},
        {4, 0, "square"}, {9, 0, "square"}, {15, 0, "composite"},
        {49, 0, "7*7"}, {121, 0, "11*11"},
        {561, 0, "Carmichael"}, {1105, 0, "Carmichael"}, {1729, 0, "Carmichael"},
        {2465, 0, "Carmichael"}, {2821, 0, "Carmichael"}, {6601, 0, "Carmichael"},
        {8911, 0, "Carmichael"},
        {2047, 0, "strong pseudoprime base 2"},
        {3215031751ULL, 0, "strong pseudoprime bases 2,3,5,7"},
        {2305843009213693951ULL, 1, "2^61-1 Mersenne prime"},
        {99999999977ULL, 1, "largest prime < 10^11"},
        {100000000003ULL, 1, "10^11+3 is prime"},
        {18446744073709551557ULL, 1, "2^64-59 (largest 64-bit prime)"},
        {18446744073709551615ULL, 0, "2^64-1 composite"}
    };
    int nvec = (int)(sizeof(vec) / sizeof(vec[0]));
    for (int i = 0; i < nvec; i++) {
        int m = is_prime_u64(vec[i].n);
        if (m != vec[i].prime) {
            printf("MR_SELFTEST_FAIL vector n=%" PRIu64 " (%s): got %d want %d\n",
                   vec[i].n, vec[i].why, m, vec[i].prime);
            fails++;
        }
    }
    printf("MR_SELFTEST_VECTORS: %d known vectors tested: %s\n", nvec,
           fails ? "FAIL" : "all match");
    printf("MR_SELFTEST_RESULT : %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* top-K gap tracking                                                 */
/* ------------------------------------------------------------------ */

typedef struct { uint64_t gap, prime; } GapEntry;

typedef struct {
    GapEntry e[MAX_K];
    int n;
} TopGaps;

static void top_init(TopGaps *t) { t->n = 0; }

static void top_add(TopGaps *t, uint64_t gap, uint64_t prime, int k)
{
    if (k > MAX_K) k = MAX_K;
    if (k <= 0) return;
    if (t->n < k) {
        int i = t->n++;
        while (i > 0 && t->e[i - 1].gap < gap) { t->e[i] = t->e[i - 1]; i--; }
        t->e[i].gap = gap;
        t->e[i].prime = prime;
    } else if (gap > t->e[t->n - 1].gap) {
        int i = t->n - 1;
        while (i > 0 && t->e[i - 1].gap < gap) { t->e[i] = t->e[i - 1]; i--; }
        t->e[i].gap = gap;
        t->e[i].prime = prime;
    }
}

/* ------------------------------------------------------------------ */
/* dataset metadata / header scan                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t off;          /* file offset of 48-byte block header */
    uint64_t prev, first, end, count, nbytes;
    uint64_t start_index;  /* global index of first_prime in this block */
} Blk;

typedef struct {
    const unsigned char *data;
    size_t size;
    Blk *blks;
    size_t nblks, cap;
    uint64_t total;
    uint64_t last_prime;
    uint64_t chunk_odd;
} Meta;

static void meta_free(Meta *m)
{
    if (m->data && m->data != MAP_FAILED) munmap((void *)m->data, m->size);
    free(m->blks);
    m->data = NULL;
    m->blks = NULL;
}

/* Returns 0 ok, 1 corrupt, 2 io, 3 incomplete trailing block. */
static int scan_meta(const char *path, int gaps_k, TopGaps *boundary_gaps,
                     Meta *m, char *err, size_t errcap)
{
    memset(m, 0, sizeof(*m));
    err[0] = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errcap, "cannot open '%s': %s", path, strerror(errno));
        return 2;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        snprintf(err, errcap, "fstat('%s') failed: %s", path, strerror(errno));
        close(fd);
        return 2;
    }
    uint64_t fsz = (uint64_t)st.st_size;
    if (fsz < GLOBAL_HDR_SIZE) {
        snprintf(err, errcap,
                 "file is %" PRIu64 " bytes, smaller than the 32-byte global header",
                 fsz);
        close(fd);
        return 1;
    }
    void *map = mmap(NULL, (size_t)fsz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        snprintf(err, errcap, "mmap('%s') failed: %s", path, strerror(errno));
        close(fd);
        return 2;
    }
    close(fd);

    const unsigned char *d = (const unsigned char *)map;
    m->data = d;
    m->size = (size_t)fsz;

    if (memcmp(d, "PRMGAP01", 8) != 0) {
        snprintf(err, errcap, "global header magic is not 'PRMGAP01'");
        return 1;
    }
    uint32_t gver = rd32(d + 8);
    uint32_t gflags = rd32(d + 12);
    m->chunk_odd = rd64(d + 16);
    uint64_t gres = rd64(d + 24);
    if (gver != 1) {
        snprintf(err, errcap, "global version is %" PRIu32 ", expected 1", gver);
        return 1;
    }
    if (gflags != 0) {
        snprintf(err, errcap, "global flags is %" PRIu32 ", expected 0", gflags);
        return 1;
    }
    if (gres != 0) {
        snprintf(err, errcap, "global reserved field is %" PRIu64 ", expected 0", gres);
        return 1;
    }

    uint64_t off = GLOBAL_HDR_SIZE;
    uint64_t last_end = 0;

    while (off < fsz) {
        if (fsz - off < BLOCK_HDR_SIZE) {
            snprintf(err, errcap,
                     "INCOMPLETE: %" PRIu64 "-byte fragment at offset %" PRIu64
                     " is not a full 48-byte block header (append in progress?)",
                     fsz - off, off);
            return 3;
        }
        const unsigned char *b = d + off;
        if (memcmp(b, "PRMB", 4) != 0) {
            snprintf(err, errcap,
                     "block %zu at offset %" PRIu64 ": magic is not 'PRMB'",
                     m->nblks, off);
            return 1;
        }
        uint16_t bver = rd16(b + 4);
        uint16_t bflags = rd16(b + 6);
        uint64_t prev = rd64(b + 8);
        uint64_t first = rd64(b + 16);
        uint64_t end = rd64(b + 24);
        uint64_t count = rd64(b + 32);
        uint64_t nbytes = rd64(b + 40);

        if (bver != 1) {
            snprintf(err, errcap, "block %zu at offset %" PRIu64
                     ": version is %u, expected 1", m->nblks, off, (unsigned)bver);
            return 1;
        }
        if (bflags != 0) {
            snprintf(err, errcap, "block %zu at offset %" PRIu64
                     ": flags is %u, expected 0", m->nblks, off, (unsigned)bflags);
            return 1;
        }
        if (m->nblks == 0) {
            if (prev != 0 || first != 2 || end != 2 || count != 1 || nbytes != 0) {
                snprintf(err, errcap,
                         "first block must be the prime-2 singleton "
                         "{prev=0, first=2, end=2, count=1, nbytes=0}; got "
                         "{prev=%" PRIu64 ", first=%" PRIu64 ", end=%" PRIu64
                         ", count=%" PRIu64 ", nbytes=%" PRIu64 "}",
                         prev, first, end, count, nbytes);
                return 1;
            }
        } else {
            if (prev != last_end) {
                snprintf(err, errcap,
                         "block %zu at offset %" PRIu64 ": prev_prime=%" PRIu64
                         " but previous block end_prime=%" PRIu64 " (chain broken)",
                         m->nblks, off, prev, last_end);
                return 1;
            }
            if (count == 0) {
                snprintf(err, errcap, "block %zu at offset %" PRIu64
                         ": count is 0", m->nblks, off);
                return 1;
            }
            if (first <= prev) {
                snprintf(err, errcap,
                         "block %zu at offset %" PRIu64 ": first_prime=%" PRIu64
                         " is not greater than prev_prime=%" PRIu64,
                         m->nblks, off, first, prev);
                return 1;
            }
            uint64_t bg = first - prev;
            if (prev == 2) {
                if (bg != 1) {
                    snprintf(err, errcap,
                             "block %zu at offset %" PRIu64
                             ": first block after the prime-2 singleton must "
                             "start at 3, but first_prime=%" PRIu64,
                             m->nblks, off, first);
                    return 1;
                }
            } else if (bg < 2 || (bg & 1)) {
                snprintf(err, errcap,
                         "block %zu at offset %" PRIu64
                         ": cross-block gap first_prime-prev_prime = %" PRIu64
                         " is not an even number >= 2", m->nblks, off, bg);
                return 1;
            }
            if ((first & 1) == 0) {
                snprintf(err, errcap, "block %zu at offset %" PRIu64
                         ": first_prime=%" PRIu64 " is even (must be odd)",
                         m->nblks, off, first);
                return 1;
            }
            if (gaps_k > 0 && boundary_gaps)
                top_add(boundary_gaps, bg, prev, gaps_k);
        }
        if (count >= 1) {
            if (end < first) {
                snprintf(err, errcap,
                         "block %zu at offset %" PRIu64
                         ": end_prime=%" PRIu64 " < first_prime=%" PRIu64,
                         m->nblks, off, end, first);
                return 1;
            }
            if (count == 1 && end != first) {
                snprintf(err, errcap,
                         "block %zu at offset %" PRIu64
                         ": count is 1 but first_prime=%" PRIu64
                         " != end_prime=%" PRIu64,
                         m->nblks, off, first, end);
                return 1;
            }
            if (count > 1 && end <= first) {
                snprintf(err, errcap,
                         "block %zu at offset %" PRIu64
                         ": count is %" PRIu64 " but end_prime=%" PRIu64
                         " <= first_prime=%" PRIu64,
                         m->nblks, off, count, end, first);
                return 1;
            }
            if (m->nblks > 0) {
                if ((end & 1) == 0) {
                    snprintf(err, errcap, "block %zu at offset %" PRIu64
                             ": end_prime=%" PRIu64 " is even (must be odd)",
                             m->nblks, off, end);
                    return 1;
                }
                if (((end - first) & 1) != 0) {
                    snprintf(err, errcap,
                             "block %zu at offset %" PRIu64
                             ": end_prime-first_prime = %" PRIu64
                             " is odd for a block of odd primes",
                             m->nblks, off, end - first);
                    return 1;
                }
            }
        }
        /* count vs payload-size plausibility (each gap is 1..10 bytes LEB128) */
        uint64_t ngaps = count - 1;
        if (nbytes < ngaps) {
            snprintf(err, errcap,
                     "block %zu at offset %" PRIu64
                     ": nbytes=%" PRIu64 " < count-1=%" PRIu64
                     " (payload cannot hold all gaps)",
                     m->nblks, off, nbytes, ngaps);
            return 1;
        }
        if (ngaps <= UINT64_MAX / 10 && nbytes > ngaps * 10) {
            snprintf(err, errcap,
                     "block %zu at offset %" PRIu64
                     ": nbytes=%" PRIu64 " > 10*(count-1)=%" PRIu64
                     " (payload too large to be LEB128 gaps)",
                     m->nblks, off, nbytes, ngaps * 10);
            return 1;
        }
        if (count > 1 && ngaps <= UINT64_MAX / 2 &&
            (end - first) < 2 * ngaps) {
            snprintf(err, errcap,
                     "block %zu at offset %" PRIu64
                     ": end-first=%" PRIu64 " < 2*(count-1)=%" PRIu64
                     " (gaps must be even and >= 2)",
                     m->nblks, off, end - first, 2 * ngaps);
            return 1;
        }
        if (nbytes > fsz - off - BLOCK_HDR_SIZE) {
            snprintf(err, errcap,
                     "INCOMPLETE: block %zu at offset %" PRIu64
                     ": nbytes=%" PRIu64 " extends past end of file "
                     "(file size %" PRIu64 ")",
                     m->nblks, off, nbytes, fsz);
            return 3;
        }

        if (m->nblks == m->cap) {
            m->cap = m->cap ? m->cap * 2 : 1024;
            m->blks = (Blk *)xrealloc(m->blks, m->cap * sizeof(Blk));
        }
        Blk *B = &m->blks[m->nblks];
        B->off = off;
        B->prev = prev;
        B->first = first;
        B->end = end;
        B->count = count;
        B->nbytes = nbytes;
        B->start_index = m->total;
        if (UINT64_MAX - m->total < count) {
            snprintf(err, errcap, "total prime count overflows u64");
            return 1;
        }
        m->total += count;
        m->last_prime = end;
        last_end = end;
        m->nblks++;
        off += BLOCK_HDR_SIZE + nbytes;
    }
    if (off != fsz) {
        snprintf(err, errcap, "internal: scan ended at offset %" PRIu64
                 " but file size is %" PRIu64, off, fsz);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* full decode (multithreaded)                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t count;
    uint64_t cpcnt[MAX_CP];
    uint64_t gaps_sum, n_gaps;
    uint64_t sample_checked, sample_fail;
    uint64_t sample_examples[8];
    int nsample_examples;
    uint64_t endpoints_checked, endpoint_fail;
    uint64_t imin_gaps, imin_numbers, imin_primes, imin_fail;
    TopGaps top;
    int error;
    size_t err_block;
    char err[512];
} ThreadRes;

typedef struct {
    const Meta *m;
    size_t b0, b1;            /* block range [b0,b1) */
    size_t sp0, sp1;          /* sample position range */
    const uint64_t *samples;
    const uint64_t *cp;
    int ncp;
    int gaps_k;
    int interior_min;
    ThreadRes res;
} WorkerArg;

static void tset_error(ThreadRes *r, size_t block, const char *fmt, ...)
{
    if (r->error) return;
    r->error = 1;
    r->err_block = block;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, sizeof(r->err), fmt, ap);
    va_end(ap);
}

static void *worker_main(void *vp)
{
    WorkerArg *a = (WorkerArg *)vp;
    ThreadRes *r = &a->res;
    const Meta *m = a->m;
    memset(r, 0, sizeof(*r));
    top_init(&r->top);

    size_t sp = a->sp0;
    int c = 0;                 /* checkpoint pointer */
    uint64_t produced = 0;     /* primes decoded by this thread so far */
    uint64_t idx = 0;

    if (a->b0 < a->b1) idx = m->blks[a->b0].start_index;

    for (size_t b = a->b0; b < a->b1; b++) {
        const Blk *B = &m->blks[b];
        if (idx != B->start_index) {
            tset_error(r, b, "internal: thread start index mismatch "
                       "(have %" PRIu64 ", block header chain says %" PRIu64 ")",
                       idx, B->start_index);
            goto done;
        }
        /* endpoint primality: cheap and catches scrambled data */
        if (!is_prime_u64(B->first)) {
            tset_error(r, b, "first_prime=%" PRIu64 " is not prime (Miller-Rabin)",
                       B->first);
            goto done;
        }
        r->endpoints_checked++;
        if (B->count > 1) {
            r->endpoints_checked++;
            if (!is_prime_u64(B->end)) {
                r->endpoint_fail++;
                tset_error(r, b, "end_prime=%" PRIu64 " is not prime (Miller-Rabin)",
                           B->end);
                goto done;
            }
        }
        const unsigned char *cur = m->data + B->off + BLOCK_HDR_SIZE;
        const unsigned char *pend = cur + B->nbytes;
        uint64_t p = B->first;
        uint64_t j = 0;

        for (;;) {
            /* -- process prime p (global index idx) -- */
            if (sp < a->sp1) {
                if (idx > a->samples[sp]) {
                    tset_error(r, b, "internal: missed sample index %" PRIu64,
                               a->samples[sp]);
                    goto done;
                }
                if (idx == a->samples[sp]) {
                    if (!is_prime_u64(p)) {
                        r->sample_fail++;
                        tset_error(r, b, "sampled decoded prime %" PRIu64
                                   " is composite (Miller-Rabin)", p);
                    }
                    if (r->nsample_examples < 8)
                        r->sample_examples[r->nsample_examples++] = p;
                    r->sample_checked++;
                    sp++;
                }
            }
            while (c < a->ncp) {
                if (p > a->cp[c]) { r->cpcnt[c] = produced; c++; }
                else if (p == a->cp[c]) { r->cpcnt[c] = produced + 1; c++; }
                else break;
            }
            produced++;
            idx++;
            j++;
            if (j == B->count) break;

            /* -- decode next gap (unsigned LEB128) -- */
            uint64_t g = 0;
            int shift = 0;
            unsigned char byte;
            do {
                if (cur >= pend) {
                    tset_error(r, b, "payload ended prematurely after %" PRIu64
                               " of %" PRIu64 " primes", j + 1, B->count);
                    goto done;
                }
                byte = *cur++;
                if (shift >= 64 || (shift == 63 && (byte & 0x7F) > 1)) {
                    tset_error(r, b, "LEB128 gap value overflows u64");
                    goto done;
                }
                g |= ((uint64_t)(byte & 0x7F)) << shift;
                shift += 7;
            } while (byte & 0x80);

            if (g < 2 || (g & 1)) {
                tset_error(r, b, "gap %" PRIu64 " from prime %" PRIu64
                           " is not an even number >= 2", g, p);
                goto done;
            }
            if (a->gaps_k > 0 &&
                (r->top.n < a->gaps_k || g > r->top.e[r->top.n - 1].gap))
                top_add(&r->top, g, p, a->gaps_k);
            if (a->interior_min > 0 && g >= (uint64_t)a->interior_min) {
                uint64_t q = p + g;
                r->imin_gaps++;
                if (!is_prime_u64(p) || !is_prime_u64(q)) r->imin_fail++;
                uint64_t inter = 0;
                for (uint64_t mm = p + 2; mm < q; mm += 2) {
                    inter++;
                    if (is_prime_u64(mm)) r->imin_primes++;
                }
                r->imin_numbers += inter;
            }
            r->gaps_sum += g;
            r->n_gaps++;
            p += g;
        }
        if (cur != pend) {
            tset_error(r, b, "payload is %" PRIu64
                       " bytes but decoded %" PRIu64 " gaps consumed %" PRIu64
                       " payload bytes", B->nbytes, B->count - 1,
                       (uint64_t)(cur - (m->data + B->off + BLOCK_HDR_SIZE)));
            goto done;
        }
        if (p != B->end) {
            tset_error(r, b, "last decoded prime %" PRIu64
                       " != block end_prime %" PRIu64, p, B->end);
            goto done;
        }
    }

done:
    for (; c < a->ncp; c++) r->cpcnt[c] = produced;
    r->count = produced;
    (void)idx;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* optional dump of all primes (small datasets only)                  */
/* ------------------------------------------------------------------ */

static int dump_primes(const Meta *m, const char *path, char *err, size_t errcap)
{
    if (m->total > 20000000ULL) {
        snprintf(err, errcap, "refusing to dump %" PRIu64
                 " primes (limit 20000000)", m->total);
        return 1;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, errcap, "cannot open '%s' for writing: %s", path,
                 strerror(errno));
        return 2;
    }
    uint64_t written = 0;
    for (size_t b = 0; b < m->nblks; b++) {
        const Blk *B = &m->blks[b];
        const unsigned char *cur = m->data + B->off + BLOCK_HDR_SIZE;
        const unsigned char *pend = cur + B->nbytes;
        uint64_t p = B->first;
        fprintf(f, "%" PRIu64 "\n", p);
        written++;
        for (uint64_t j = 1; j < B->count; j++) {
            uint64_t g = 0;
            int shift = 0;
            unsigned char byte;
            do {
                if (cur >= pend) {
                    snprintf(err, errcap, "internal dump error");
                    fclose(f);
                    return 1;
                }
                byte = *cur++;
                g |= ((uint64_t)(byte & 0x7F)) << shift;
                shift += 7;
            } while (byte & 0x80);
            p += g;
            fprintf(f, "%" PRIu64 "\n", p);
            written++;
        }
    }
    fclose(f);
    if (written != m->total) {
        snprintf(err, errcap, "internal: dumped %" PRIu64 " != total %" PRIu64,
                 written, m->total);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                */
/* ------------------------------------------------------------------ */

enum { MODE_FULL = 0, MODE_META = 1, MODE_MR_SELFTEST = 2 };

typedef struct {
    const char *path;
    int mode;
    int threads;
    int gaps_k;
    uint64_t samples;
    uint64_t seed;
    uint64_t mr_limit;
    uint64_t cps[MAX_CP];
    int ncp;
    uint64_t ex_x[MAX_CP], ex_v[MAX_CP];
    int nex;
    uint64_t expect_last;
    int have_expect_last;
    const char *dump;
    int interior_min;
} Opts;

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
        "usage: %s <dataset.bin> [options]\n"
        "\n"
        "  --meta                  parse/validate headers only (fast; safe on a\n"
        "                          dataset that is still being appended)\n"
        "  --full                  full payload decode + all checks (default)\n"
        "  --threads N             decode threads (default 4, max 8)\n"
        "  --samples N             random decoded primes to Miller-Rabin test\n"
        "                          (default 2000)\n"
        "  --gaps K                largest gaps whose interior is fully checked\n"
        "                          for compositeness (default 500, max %d)\n"
        "  --interior-min G        additionally verify EVERY gap >= G (stored and\n"
        "                          block-boundary): endpoints prime and every odd\n"
        "                          number strictly inside composite (parallel)\n"
        "  --seed N                base-10 seed for deterministic sampling\n"
        "  --checkpoints LIST      comma list, e.g. 1e6,1e8,1e11\n"
        "  --expect-pi LIST        comma list x:pi(x), e.g. 1e8:5761455\n"
        "  --expect-last N         expected last prime in the file\n"
        "  --dump-primes FILE      write all decoded primes as decimal text\n"
        "                          (small datasets only)\n"
        "  --mr-selftest N         check Miller-Rabin against trial division for\n"
        "                          every n in 2..N and against known vectors;\n"
        "                          then exit (no dataset needed)\n"
        "  --help\n"
        "\n"
        "exit: 0 PASS, 1 FAIL, 2 usage/io, 3 incomplete trailing block\n",
        argv0, MAX_K);
}

static int parse_u64(const char *s, uint64_t *out)
{
    if (!s || !*s || s[0] == '-') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || errno == ERANGE) return -1;
    if (*end == 'e' || *end == 'E') {
        char *e2 = NULL;
        long ex = strtol(end + 1, &e2, 10);
        if (*e2 != 0 || ex < 0 || ex > 19) return -1;
        for (long i = 0; i < ex; i++) {
            if (v > (UINT64_MAX / 10)) return -1;
            v *= 10;
        }
    } else if (*end != 0) {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static int cmp_u64(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *)pa, b = *(const uint64_t *)pb;
    return (a > b) - (a < b);
}

/* parse "x:y,x:y" into arrays */
static int parse_pairs(const char *s, uint64_t *xs, uint64_t *vs, int *n, int maxn)
{
    char *copy = strdup(s);
    if (!copy) return -1;
    int cnt = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (!colon) { free(copy); return -1; }
        *colon = 0;
        if (cnt >= maxn) { free(copy); return -1; }
        if (parse_u64(tok, &xs[cnt]) != 0) { free(copy); return -1; }
        if (parse_u64(colon + 1, &vs[cnt]) != 0) { free(copy); return -1; }
        cnt++;
    }
    free(copy);
    *n = cnt;
    return 0;
}

static int parse_list(const char *s, uint64_t *arr, int *n, int maxn)
{
    char *copy = strdup(s);
    if (!copy) return -1;
    int cnt = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        if (cnt >= maxn) { free(copy); return -1; }
        if (parse_u64(tok, &arr[cnt]) != 0) { free(copy); return -1; }
        cnt++;
    }
    free(copy);
    *n = cnt;
    return 0;
}

static void cp_label(char *buf, size_t cap, uint64_t x)
{
    uint64_t p = 1;
    int k = 0;
    while (p < x && p <= UINT64_MAX / 10) { p *= 10; k++; }
    if (p == x && x >= 10) snprintf(buf, cap, "1e%d", k);
    else snprintf(buf, cap, "%" PRIu64, x);
}

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* distinct deterministic samples in [0,N), sorted ascending */
static int gen_samples(uint64_t N, uint64_t want, uint64_t seed,
                       uint64_t *out, uint64_t *nout)
{
    if (N == 0 || want == 0) { *nout = 0; return 0; }
    if (want > N) want = N;
    uint64_t state = seed;
    uint64_t n = 0;
    if (want == N) {
        for (uint64_t i = 0; i < N; i++) out[n++] = i;
    } else {
        int guard = 0;
        while (n < want) {
            uint64_t v = splitmix64(&state) % N;
            int dup = 0;
            for (uint64_t i = 0; i < n; i++)
                if (out[i] == v) { dup = 1; break; }
            if (!dup) out[n++] = v;
            if (++guard > (int)(want * 100 + 100000)) return -1;
        }
    }
    qsort(out, (size_t)n, sizeof(uint64_t), cmp_u64);
    *nout = n;
    return 0;
}

static size_t lower_bound_u64(const uint64_t *a, size_t n, uint64_t v)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid] < v) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    Opts o;
    memset(&o, 0, sizeof(o));
    o.threads = 4;
    o.gaps_k = 500;
    o.samples = 2000;
    o.seed = 0x5EED123456789ABCULL;
    o.mode = MODE_FULL;
    uint64_t default_cps[] = { 1000000ULL, 10000000ULL, 100000000ULL,
                               1000000000ULL, 10000000000ULL, 100000000000ULL };
    for (int i = 0; i < 6; i++) o.cps[i] = default_cps[i];
    o.ncp = 6;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout, argv[0]); return 0; }
        else if (!strcmp(a, "--meta")) o.mode = MODE_META;
        else if (!strcmp(a, "--full")) o.mode = MODE_FULL;
        else if (!strcmp(a, "--threads") && i + 1 < argc) {
            uint64_t v;
            if (parse_u64(argv[++i], &v) != 0 || v < 1 || v > MAX_THREADS) {
                fprintf(stderr, "error: bad --threads\n"); return 2;
            }
            o.threads = (int)v;
        } else if (!strcmp(a, "--samples") && i + 1 < argc) {
            if (parse_u64(argv[++i], &o.samples) != 0) { fprintf(stderr, "error: bad --samples\n"); return 2; }
        } else if (!strcmp(a, "--gaps") && i + 1 < argc) {
            uint64_t v;
            if (parse_u64(argv[++i], &v) != 0 || v > MAX_K) { fprintf(stderr, "error: bad --gaps\n"); return 2; }
            o.gaps_k = (int)v;
        } else if (!strcmp(a, "--interior-min") && i + 1 < argc) {
            uint64_t v;
            if (parse_u64(argv[++i], &v) != 0 || v > (1ULL << 40)) {
                fprintf(stderr, "error: bad --interior-min\n"); return 2;
            }
            o.interior_min = (int)v;
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            if (parse_u64(argv[++i], &o.seed) != 0) { fprintf(stderr, "error: bad --seed\n"); return 2; }
        } else if (!strcmp(a, "--checkpoints") && i + 1 < argc) {
            if (parse_list(argv[++i], o.cps, &o.ncp, MAX_CP) != 0) {
                fprintf(stderr, "error: bad --checkpoints\n"); return 2;
            }
        } else if (!strcmp(a, "--expect-pi") && i + 1 < argc) {
            if (parse_pairs(argv[++i], o.ex_x, o.ex_v, &o.nex, MAX_CP) != 0) {
                fprintf(stderr, "error: bad --expect-pi\n"); return 2;
            }
        } else if (!strcmp(a, "--expect-last") && i + 1 < argc) {
            if (parse_u64(argv[++i], &o.expect_last) != 0) {
                fprintf(stderr, "error: bad --expect-last\n"); return 2;
            }
            o.have_expect_last = 1;
        } else if (!strcmp(a, "--dump-primes") && i + 1 < argc) {
            o.dump = argv[++i];
        } else if (!strcmp(a, "--mr-selftest") && i + 1 < argc) {
            if (parse_u64(argv[++i], &o.mr_limit) != 0) {
                fprintf(stderr, "error: bad --mr-selftest\n"); return 2;
            }
            o.mode = MODE_MR_SELFTEST;
        } else if (a[0] != '-') {
            if (o.path) { fprintf(stderr, "error: more than one dataset path\n"); return 2; }
            o.path = a;
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            return 2;
        }
    }

    if (o.mode == MODE_MR_SELFTEST) return mr_selftest(o.mr_limit);
    if (!o.path) { usage(stderr, argv[0]); return 2; }

    /* union of checkpoints and expected-pi arguments, sorted + deduped */
    {
        uint64_t tmp[MAX_CP * 2];
        int n = 0;
        if (o.ncp > MAX_CP) { fprintf(stderr, "error: too many checkpoints\n"); return 2; }
        for (int i = 0; i < o.ncp; i++) tmp[n++] = o.cps[i];
        for (int i = 0; i < o.nex; i++) tmp[n++] = o.ex_x[i];
        qsort(tmp, (size_t)n, sizeof(uint64_t), cmp_u64);
        int m2 = 0;
        for (int i = 0; i < n; i++)
            if (m2 == 0 || tmp[i] != tmp[m2 - 1]) tmp[m2++] = tmp[i];
        if (m2 > MAX_CP) { fprintf(stderr, "error: too many distinct checkpoints\n"); return 2; }
        for (int i = 0; i < m2; i++) o.cps[i] = tmp[i];
        o.ncp = m2;
    }

    double t_start = now_s();
    TopGaps boundary_gaps;
    top_init(&boundary_gaps);
    Meta m;
    char err[1024];
    int st = scan_meta(o.path, o.gaps_k, &boundary_gaps, &m, err, sizeof(err));
    double t_meta = now_s() - t_start;

    if (st == 1) {
        printf("DATASET            : %s\n", o.path);
        printf("RESULT             : FAIL — %s\n", err);
        meta_free(&m);
        return 1;
    }
    if (st == 2) {
        printf("RESULT             : ERROR — %s\n", err);
        meta_free(&m);
        return 2;
    }
    if (st == 3) {
        printf("DATASET            : %s\n", o.path);
        printf("FILE_SIZE          : %" PRIu64 " bytes\n", (uint64_t)m.size);
        printf("BLOCKS             : %zu\n", m.nblks);
        printf("TOTAL_PRIMES       : %" PRIu64 " (from complete blocks)\n", m.total);
        printf("LAST_PRIME         : %" PRIu64 "\n", m.last_prime);
        printf("RESULT             : INCOMPLETE — %s\n", err);
        printf("NOTE               : trailing partial block is expected only while\n"
               "                     the append-only dataset is being written\n");
        meta_free(&m);
        return 3;
    }

    printf("DATASET            : %s\n", o.path);
    printf("MODE               : %s\n", o.mode == MODE_META ? "meta (headers only)" : "full");
    printf("FILE_SIZE          : %" PRIu64 " bytes (%.2f MiB)\n",
           (uint64_t)m.size, (double)m.size / 1048576.0);
    printf("GLOBAL             : magic=PRMGAP01 version=1 flags=0 reserved=0 "
           "chunk_odd=%" PRIu64 "\n", m.chunk_odd);
    printf("BLOCKS             : %zu\n", m.nblks);
    printf("TOTAL_PRIMES       : %" PRIu64 " (sum of block headers)\n", m.total);
    printf("LAST_PRIME         : %" PRIu64 "\n", m.last_prime);
    if (m.nblks > 0) printf("FIRST_PRIME        : %" PRIu64 "\n", m.blks[0].first);
    printf("HEADER_SCAN        : PASS (%.3f s)\n", t_meta);

    if (o.mode == MODE_META) {
        if (boundary_gaps.n > 0)
            printf("LARGEST_BLOCK_GAP  : %" PRIu64 " (at prime %" PRIu64 ")\n",
                   boundary_gaps.e[0].gap, boundary_gaps.e[0].prime);
        printf("RESULT             : PASS\n");
        meta_free(&m);
        return 0;
    }

    /* ---------------- full decode ---------------- */
    int fail = 0;
    double t_dec0 = now_s();

    uint64_t *samples = NULL;
    uint64_t nsamp = 0;
    uint64_t want_samples = o.samples < m.total ? o.samples : m.total;
    if (want_samples > 0) {
        samples = (uint64_t *)xmalloc((size_t)want_samples * sizeof(uint64_t));
        uint64_t seed = o.seed;
        if (gen_samples(m.total, want_samples, seed, samples, &nsamp) != 0) {
            printf("RESULT             : FAIL — could not generate sample indices\n");
            free(samples);
            meta_free(&m);
            return 1;
        }
        if (nsamp != want_samples) {
            printf("RESULT             : FAIL — generated %" PRIu64
                   " samples, wanted %" PRIu64 "\n", nsamp, want_samples);
            free(samples);
            meta_free(&m);
            return 1;
        }
    }

    int T = o.threads;
    if (T < 1) T = 1;
    if (T > MAX_THREADS) T = MAX_THREADS;
    if (m.nblks > 0 && (size_t)T > m.nblks) T = (int)m.nblks;

    size_t *bounds = (size_t *)xmalloc(((size_t)T + 1) * sizeof(size_t));
    {
        size_t b = 0;
        uint64_t cum = 0;
        bounds[0] = 0;
        for (int t = 1; t <= T; t++) {
            u128 target = (u128)m.total * (uint64_t)t / (uint64_t)T;
            while (b < m.nblks && (u128)cum < target) { cum += m.blks[b].count; b++; }
            bounds[t] = b;
        }
        bounds[T] = m.nblks;
    }

    WorkerArg *wa = (WorkerArg *)xmalloc((size_t)T * sizeof(WorkerArg));
    pthread_t *th = (pthread_t *)xmalloc((size_t)T * sizeof(pthread_t));
    for (int t = 0; t < T; t++) {
        memset(&wa[t], 0, sizeof(wa[t]));
        wa[t].m = &m;
        wa[t].b0 = bounds[t];
        wa[t].b1 = bounds[t + 1];
        wa[t].samples = samples;
        wa[t].cp = o.cps;
        wa[t].ncp = o.ncp;
        wa[t].gaps_k = o.gaps_k;
        wa[t].interior_min = o.interior_min;
        uint64_t start_idx = m.nblks ? m.blks[wa[t].b0].start_index : 0;
        if (wa[t].b0 >= m.nblks) start_idx = m.total;
        uint64_t end_idx = (wa[t].b1 < m.nblks) ? m.blks[wa[t].b1].start_index : m.total;
        wa[t].sp0 = lower_bound_u64(samples, (size_t)nsamp, start_idx);
        wa[t].sp1 = lower_bound_u64(samples, (size_t)nsamp, end_idx);
        if (pthread_create(&th[t], NULL, worker_main, &wa[t]) != 0) {
            fprintf(stderr, "fatal: pthread_create failed\n");
            return 2;
        }
    }
    for (int t = 0; t < T; t++) pthread_join(th[t], NULL);
    double t_decode = now_s() - t_dec0;

    uint64_t decoded_total = 0;
    for (int t = 0; t < T; t++) {
        if (wa[t].res.error) {
            printf("ERROR              : decode thread %d, block %zu: %s\n",
                   t, wa[t].res.err_block, wa[t].res.err);
            fail = 1;
        }
        decoded_total += wa[t].res.count;
    }
    if (decoded_total != m.total) {
        printf("ERROR              : decoded total %" PRIu64
               " != header total %" PRIu64 "\n", decoded_total, m.total);
        fail = 1;
    }
    printf("DECODED_PRIMES     : %" PRIu64 " (%d threads, %.3f s)\n",
           decoded_total, T, t_decode);

    /* checkpoints */
    for (int k = 0; k < o.ncp; k++) {
        uint64_t sum = 0;
        for (int t = 0; t < T; t++) sum += wa[t].res.cpcnt[k];
        int e = -1;
        for (int i = 0; i < o.nex; i++) if (o.ex_x[i] == o.cps[k]) { e = i; break; }
        char label[32];
        cp_label(label, sizeof(label), o.cps[k]);
        if (e >= 0) {
            int ok = (sum == o.ex_v[e]);
            printf("CHECKPOINT %-8s : %" PRIu64 " (expected %" PRIu64 ") %s\n",
                   label, sum, o.ex_v[e], ok ? "PASS" : "FAIL");
            if (!ok) fail = 1;
        } else {
            printf("CHECKPOINT %-8s : %" PRIu64 "\n", label, sum);
        }
    }

    /* endpoint primality */
    {
        uint64_t ec = 0, ef = 0;
        for (int t = 0; t < T; t++) { ec += wa[t].res.endpoints_checked; ef += wa[t].res.endpoint_fail; }
        printf("MR_BLOCK_ENDPOINTS : %" PRIu64 " block first/end primes checked, "
               "%" PRIu64 " failures\n", ec, ef);
        if (ef) fail = 1;
    }

    /* random sampled primes */
    {
        uint64_t sc = 0, sf = 0, ex[8];
        int nex = 0;
        for (int t = 0; t < T; t++) {
            sc += wa[t].res.sample_checked;
            sf += wa[t].res.sample_fail;
            for (int i = 0; i < wa[t].res.nsample_examples && nex < 8; i++)
                ex[nex++] = wa[t].res.sample_examples[i];
        }
        printf("MR_SAMPLES         : %" PRIu64 " decoded primes checked "
               "(requested %" PRIu64 ", seed %" PRIu64 "), %" PRIu64
               " failures; examples:", sc, o.samples, o.seed, sf);
        for (int i = 0; i < nex; i++) printf(" %" PRIu64, ex[i]);
        printf("\n");
        if (sc != want_samples) {
            printf("ERROR              : sample coverage %" PRIu64
                   " != expected %" PRIu64 "\n", sc, want_samples);
            fail = 1;
        }
        if (sf) fail = 1;
    }

    /* merge top gaps */
    TopGaps all;
    top_init(&all);
    for (int t = 0; t < T; t++)
        for (int i = 0; i < wa[t].res.top.n; i++)
            top_add(&all, wa[t].res.top.e[i].gap, wa[t].res.top.e[i].prime, o.gaps_k);
    for (int i = 0; i < boundary_gaps.n; i++)
        top_add(&all, boundary_gaps.e[i].gap, boundary_gaps.e[i].prime, o.gaps_k);

    /* every block-boundary gap >= interior_min: endpoints prime, interiors composite */
    uint64_t bg_gaps = 0, bg_numbers = 0, bg_primes = 0, bg_fail = 0;
    if (o.interior_min > 0) {
        for (size_t b = 1; b < m.nblks; b++) {
            uint64_t pv = m.blks[b].prev, qv = m.blks[b].first;
            uint64_t g = qv - pv;
            if (g < (uint64_t)o.interior_min) continue;
            bg_gaps++;
            if (!is_prime_u64(pv) || !is_prime_u64(qv)) bg_fail++;
            uint64_t inter = 0;
            for (uint64_t mm = pv + 2; mm < qv; mm += 2) {
                inter++;
                if (is_prime_u64(mm)) bg_primes++;
            }
            bg_numbers += inter;
        }
    }
    if (o.interior_min > 0) {
        uint64_t ig = bg_gaps, inum = bg_numbers, ipr = bg_primes + bg_fail;
        for (int t = 0; t < T; t++) {
            ig += wa[t].res.imin_gaps;
            inum += wa[t].res.imin_numbers;
            ipr += wa[t].res.imin_primes + wa[t].res.imin_fail;
        }
        printf("INTERIOR_MIN_CHECK : every gap >= %d: %" PRIu64
               " gaps, %" PRIu64 " odd numbers strictly inside tested, "
               "%" PRIu64 " unexpected primes\n",
               o.interior_min, ig, inum, ipr);
        if (ipr) fail = 1;
    }
    uint64_t gsum = 0, ngaps = 0;
    for (int t = 0; t < T; t++) { gsum += wa[t].res.gaps_sum; ngaps += wa[t].res.n_gaps; }
    double avg_gap = ngaps ? (double)gsum / (double)ngaps : 0.0;

    if (all.n > 0) {
        printf("MAX_GAP            : %" PRIu64 " (consecutive primes %" PRIu64
               " .. %" PRIu64 ")\n",
               all.e[0].gap, all.e[0].prime, all.e[0].prime + all.e[0].gap);
    }
    printf("AVG_STORED_GAP     : %.4f (over %" PRIu64 " stored gaps)\n",
           avg_gap, ngaps);

    /* interior compositeness of the largest gaps */
    int sel = o.gaps_k < all.n ? o.gaps_k : all.n;
    uint64_t gi_gaps = 0, gi_interiors = 0, gi_bad = 0;
    uint64_t gi_interior_primes = 0;
    for (int i = 0; i < sel; i++) {
        uint64_t p = all.e[i].prime;
        uint64_t g = all.e[i].gap;
        uint64_t q = p + g;
        int ok = 1;
        if (!is_prime_u64(p) || !is_prime_u64(q)) ok = 0;
        uint64_t inter = 0;
        int interior_bad = 0;
        for (uint64_t mm = p + 2; mm < q; mm += 2) {
            inter++;
            if (!is_prime_u64(mm)) continue;
            interior_bad++;
            ok = 0;
        }
        gi_gaps++;
        gi_interiors += inter;
        gi_interior_primes += (uint64_t)interior_bad;
        if (!ok) gi_bad++;
        if (i < 5) {
            printf("LARGE_GAP          : gap=%" PRIu64 " interval=[%" PRIu64
                   ", %" PRIu64 "] odd-interiors-checked=%" PRIu64
                   " unexpected-primes=%d %s\n",
                   g, p, q, inter, interior_bad,
                   ok ? "OK (endpoints prime, all interiors composite)" : "FAIL");
        }
    }
    printf("GAP_INTERIOR_CHECK : %" PRIu64 " largest gaps, %" PRIu64
           " odd numbers inside them tested composite, %" PRIu64
           " unexpected primes, %" PRIu64 " failing gaps\n",
           gi_gaps, gi_interiors, gi_interior_primes, gi_bad);
    if (gi_bad) fail = 1;

    if (o.have_expect_last) {
        int ok = (m.last_prime == o.expect_last);
        printf("EXPECTED_LAST      : %" PRIu64 " (expected %" PRIu64 ") %s\n",
               m.last_prime, o.expect_last, ok ? "PASS" : "FAIL");
        if (!ok) fail = 1;
    }

    if (o.dump && !fail) {
        char derr[512];
        if (dump_primes(&m, o.dump, derr, sizeof(derr)) != 0) {
            printf("DUMP_PRIMES        : FAIL — %s\n", derr);
            fail = 1;
        } else {
            printf("DUMP_PRIMES        : %" PRIu64 " primes written to %s\n",
                   m.total, o.dump);
        }
    }

    double t_total = now_s() - t_start;
    printf("ELAPSED            : %.3f s total (header scan %.3f s, decode %.3f s)\n",
           t_total, t_meta, t_decode);
    printf("RESULT             : %s\n", fail ? "FAIL" : "PASS");

    free(wa);
    free(th);
    free(bounds);
    free(samples);
    meta_free(&m);
    return fail ? 1 : 0;
}
