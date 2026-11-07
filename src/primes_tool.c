/*
 * primes_tool.c — reader / verifier / extractor for the gap-varint prime
 * dataset format v1 produced by sieve_gap.c.
 *
 * Subcommands:
 *   info    FILE [--json]
 *   verify  FILE [--threads T] [--no-mr] [--residues] [--json]
 *   count   FILE --upto X [--json]
 *   extract FILE [--from A] [--to B] [--limit K] [--format text|u64|u32]
 *
 * verify decodes every block, checks all structural invariants, and does
 * deterministic Miller-Rabin sampling: first/middle/last prime of every
 * block, plus exhaustive compositeness checks of every odd number inside
 * sampled gaps (all gaps >= 512 everywhere, wider sampling every 2000th
 * block).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define GLOBAL_MAGIC "PRMGAP01"
#define BLOCK_MAGIC "PRMB"
#define FORMAT_VERSION 1u
#define GLOBAL_HDR_SIZE 32u
#define BLOCK_HDR_SIZE 48u

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t flags;
    uint64_t chunk_odd;
    uint64_t reserved;
} GlobalHdr;

typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t flags;
    uint64_t prev_prime;
    uint64_t first_prime;
    uint64_t end_prime;
    uint64_t count;
    uint64_t nbytes;
} BlockHdr;

_Static_assert(sizeof(GlobalHdr) == GLOBAL_HDR_SIZE, "global header size");
_Static_assert(sizeof(BlockHdr) == BLOCK_HDR_SIZE, "block header size");

static double tnow(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

typedef struct {
    uint64_t off;
    BlockHdr h;
} BlockRec;

typedef struct {
    uint64_t fsize;
    uint64_t chunk_odd;
    uint64_t max_nbytes;
    size_t n;
    BlockRec *v;
    uint64_t *pref; /* pref[i] = number of primes in blocks [0, i) */
} Dataset;

static void ds_free(Dataset *ds) {
    free(ds->v);
    free(ds->pref);
    ds->v = NULL;
    ds->pref = NULL;
}

static int ds_load(const char *path, Dataset *ds) {
    memset(ds, 0, sizeof *ds);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno)); return 0; }
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    off_t sz = ftello(f);
    if (fseeko(f, 0, SEEK_SET) != 0 || sz < (off_t)GLOBAL_HDR_SIZE) {
        fprintf(stderr, "%s: too short for global header\n", path);
        fclose(f);
        return 0;
    }
    ds->fsize = (uint64_t)sz;
    GlobalHdr gh;
    if (fread(&gh, sizeof gh, 1, f) != 1) { fclose(f); return 0; }
    if (memcmp(gh.magic, GLOBAL_MAGIC, 8) != 0 || gh.version != FORMAT_VERSION) {
        fprintf(stderr, "%s: bad global header\n", path);
        fclose(f);
        return 0;
    }
    ds->chunk_odd = gh.chunk_odd;
    size_t cap = 1024;
    ds->v = (BlockRec *)malloc(cap * sizeof(BlockRec));
    if (!ds->v) { fclose(f); return 0; }
    ds->n = 0;
    uint64_t pos = GLOBAL_HDR_SIZE;
    uint64_t last_end = 0;
    while (pos + BLOCK_HDR_SIZE <= ds->fsize) {
        BlockHdr bh;
        if (fseeko(f, (off_t)pos, SEEK_SET) != 0) break;
        if (fread(&bh, sizeof bh, 1, f) != 1) break;
        int bad = 0;
        if (memcmp(bh.magic, BLOCK_MAGIC, 4) != 0) bad = 1;
        if (bh.version != FORMAT_VERSION) bad = 1;
        if (bh.count < 1) bad = 1;
        if (bh.end_prime < bh.first_prime) bad = 1;
        if (bh.count >= 2) {
            if (bh.nbytes < bh.count - 1) bad = 1;
            if (bh.nbytes > 10 * (bh.count - 1)) bad = 1;
        } else if (bh.nbytes != 0) {
            bad = 1;
        }
        if (ds->n == 0) {
            if (bh.prev_prime != 0) bad = 1;
            if (bh.first_prime != 2) bad = 1; /* dataset always starts at prime 2 */
        } else {
            if (bh.prev_prime != last_end) bad = 1;
            if (bh.first_prime <= last_end) bad = 1;
        }
        if (pos + BLOCK_HDR_SIZE + bh.nbytes > ds->fsize) bad = 1;
        if (bad) {
            fprintf(stderr, "%s: invalid block header at offset %" PRIu64 "\n", path, pos);
            fclose(f);
            ds_free(ds);
            return 0;
        }
        if (ds->n == cap) {
            cap <<= 1;
            BlockRec *nv = (BlockRec *)realloc(ds->v, cap * sizeof(BlockRec));
            if (!nv) { fclose(f); ds_free(ds); return 0; }
            ds->v = nv;
        }
        ds->v[ds->n].off = pos;
        ds->v[ds->n].h = bh;
        ds->n++;
        if (bh.nbytes > ds->max_nbytes) ds->max_nbytes = bh.nbytes;
        last_end = bh.end_prime;
        pos += BLOCK_HDR_SIZE + bh.nbytes;
    }
    fclose(f);
    if (pos != ds->fsize) {
        fprintf(stderr, "%s: %" PRIu64 " trailing bytes after last block\n", path, ds->fsize - pos);
        ds_free(ds);
        return 0;
    }
    ds->pref = (uint64_t *)malloc((ds->n + 1) * sizeof(uint64_t));
    if (!ds->pref) { ds_free(ds); return 0; }
    ds->pref[0] = 0;
    for (size_t i = 0; i < ds->n; i++) ds->pref[i + 1] = ds->pref[i] + ds->v[i].h.count;
    return 1;
}

static uint64_t ds_total_primes(const Dataset *ds) {
    uint64_t t = 0;
    for (size_t i = 0; i < ds->n; i++) t += ds->v[i].h.count;
    return t;
}

/* ---------------- deterministic Miller-Rabin (n < 3.3e24) ---------------- */
static inline uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m) {
    return (uint64_t)((unsigned __int128)a * b % m);
}

static uint64_t powmod(uint64_t a, uint64_t d, uint64_t m) {
    uint64_t r = 1 % m;
    a %= m;
    while (d) {
        if (d & 1) r = mulmod(r, a, m);
        a = mulmod(a, a, m);
        d >>= 1;
    }
    return r;
}

static int is_prime_mr(uint64_t n) {
    static const uint64_t bases[12] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    if (n < 2) return 0;
    for (int i = 0; i < 12; i++) {
        if (n == bases[i]) return 1;
        if (n % bases[i] == 0) return 0;
    }
    uint64_t d = n - 1;
    int s = 0;
    while (!(d & 1)) { d >>= 1; s++; }
    for (int i = 0; i < 12; i++) {
        uint64_t a = bases[i];
        uint64_t x = powmod(a, d, n);
        if (x == 1 || x == n - 1) continue;
        int composite = 1;
        for (int r = 1; r < s; r++) {
            x = mulmod(x, x, n);
            if (x == n - 1) { composite = 0; break; }
        }
        if (composite) return 0;
    }
    return 1;
}

/* Quick compositeness proof: cheap trial division first, then MR fallback. */
static int composite_quick(uint64_t x) {
    static const uint32_t sp[23] = {3,  5,  7,  11, 13, 17, 19, 23, 29, 31, 37, 41,
                                    43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89};
    for (size_t k = 0; k < 23; k++)
        if (x % sp[k] == 0) return 1;
    return !is_prime_mr(x);
}

/* ---------------- info ---------------- */
static int cmd_info(const char *path, int json) {
    Dataset ds;
    double t0 = tnow();
    if (!ds_load(path, &ds)) return 1;
    uint64_t total = ds_total_primes(&ds);
    uint64_t first = ds.n ? ds.v[0].h.first_prime : 0;
    uint64_t last = ds.n ? ds.v[ds.n - 1].h.end_prime : 0;
    if (json) {
        printf("{\"tool\":\"info\",\"file\":\"%s\",\"bytes\":%" PRIu64 ",\"blocks\":%zu,"
               "\"primes\":%" PRIu64 ",\"first_prime\":%" PRIu64 ",\"last_prime\":%" PRIu64
               ",\"chunk_odd\":%" PRIu64 ",\"ok\":1,\"elapsed_sec\":%.3f}\n",
               path, ds.fsize, ds.n, total, first, last, ds.chunk_odd, tnow() - t0);
    } else {
        printf("file:        %s\n", path);
        printf("bytes:       %" PRIu64 " (%.2f GB)\n", ds.fsize, (double)ds.fsize / 1e9);
        printf("blocks:      %zu\n", ds.n);
        printf("primes:      %" PRIu64 "\n", total);
        printf("first prime: %" PRIu64 "\n", first);
        printf("last prime:  %" PRIu64 "\n", last);
        printf("chunk_odd:   %" PRIu64 "\n", ds.chunk_odd);
    }
    ds_free(&ds);
    return 0;
}

/* ---------------- prefix-sum helpers: pi(x) in one scan ---------------- */
static uint64_t count_upto(const char *path, const Dataset *ds, uint64_t x) {
    if (ds->n == 0 || x < ds->v[0].h.first_prime) return 0;
    if (x >= ds->v[ds->n - 1].h.end_prime) return ds->pref[ds->n];
    size_t lo = 0, hi = ds->n - 1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ds->v[mid].h.end_prime >= x) hi = mid;
        else lo = mid + 1;
    }
    const size_t i = lo;
    const BlockHdr *h = &ds->v[i].h;
    if (x >= h->end_prime) return ds->pref[i] + h->count;
    if (x < h->first_prime) return ds->pref[i];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return UINT64_MAX;
    uint8_t *payload = (uint8_t *)malloc((size_t)h->nbytes + 16);
    if (!payload) { close(fd); return UINT64_MAX; }
    if (h->nbytes > 0 &&
        pread(fd, payload, (size_t)h->nbytes, (off_t)(ds->v[i].off + BLOCK_HDR_SIZE)) !=
            (ssize_t)h->nbytes) {
        free(payload);
        close(fd);
        return UINT64_MAX;
    }
    uint64_t cur = h->first_prime, c = 1;
    size_t p = 0;
    while (p < h->nbytes) {
        uint64_t g = 0;
        int shift = 0;
        while (p < h->nbytes) {
            uint8_t byte = payload[p++];
            g |= (uint64_t)(byte & 0x7F) << shift;
            if (!(byte & 0x80)) break;
            shift += 7;
        }
        cur += g;
        if (cur > x) break;
        c++;
    }
    free(payload);
    close(fd);
    return ds->pref[i] + c;
}

static int cmd_summary(const char *path, const uint64_t *targets, size_t ntargets, int json) {
    Dataset ds;
    double t0 = tnow();
    if (!ds_load(path, &ds)) return 1;
    const uint64_t total = ds.pref[ds.n];
    const uint64_t first = ds.n ? ds.v[0].h.first_prime : 0;
    const uint64_t last = ds.n ? ds.v[ds.n - 1].h.end_prime : 0;
    uint64_t *counts = (uint64_t *)malloc((ntargets ? ntargets : 1) * sizeof(uint64_t));
    if (!counts) { ds_free(&ds); return 1; }
    for (size_t i = 0; i < ntargets; i++) counts[i] = count_upto(path, &ds, targets[i]);
    if (json) {
        printf("{\"tool\":\"summary\",\"file\":\"%s\",\"bytes\":%" PRIu64
               ",\"blocks\":%zu,\"primes\":%" PRIu64 ",\"first_prime\":%" PRIu64
               ",\"last_prime\":%" PRIu64 ",\"chunk_odd\":%" PRIu64
               ",\"ok\":1,\"elapsed_sec\":%.3f,\"checkpoints\":[",
               path, ds.fsize, ds.n, total, first, last, ds.chunk_odd, tnow() - t0);
        for (size_t i = 0; i < ntargets; i++)
            printf("%s{\"upto\":%" PRIu64 ",\"count\":%" PRIu64 "}", i ? "," : "", targets[i], counts[i]);
        printf("]}\n");
    } else {
        printf("file:        %s\n", path);
        printf("bytes:       %" PRIu64 "\n", ds.fsize);
        printf("blocks:      %zu\n", ds.n);
        printf("primes:      %" PRIu64 "\n", total);
        printf("first prime: %" PRIu64 "\n", first);
        printf("last prime:  %" PRIu64 "\n", last);
        for (size_t i = 0; i < ntargets; i++)
            printf("pi(x<=%" PRIu64 ") = %" PRIu64 "\n", targets[i], counts[i]);
    }
    free(counts);
    ds_free(&ds);
    return 0;
}

/* ---------------- verify ---------------- */
typedef struct {
    uint64_t count;
    uint64_t twin;
    uint64_t sum_gap;
    uint64_t max_gap;
    uint64_t max_gap_after;
    uint64_t mr_tests;   /* primality tests on stored primes */
    uint64_t comp_tests; /* compositeness checks inside gaps */
    uint64_t bad;
    uint64_t res12[12];
    uint64_t hist[1026];
} Agg;

typedef struct {
    const Dataset *ds;
    const char *path;
    atomic_size_t next;
    int do_mr;
    int do_res;
    Agg *agg;
} VShared;

typedef struct {
    VShared *vs;
    Agg *a;
} VArg;

static inline void agg_gap(Agg *a, uint64_t g, uint64_t after) {
    a->sum_gap += g;
    if (g == 2) a->twin++;
    if (g > a->max_gap) { a->max_gap = g; a->max_gap_after = after; }
    if (g <= 1024) a->hist[g]++; else a->hist[1025]++;
}

static void check_gap_composites(Agg *a, uint64_t g, uint64_t after) {
    for (uint64_t x = after - g + 2; x < after; x += 2) {
        a->comp_tests++;
        if (!composite_quick(x)) a->bad++;
    }
}

static void *verify_thread(void *arg) {
    VArg *va = (VArg *)arg;
    VShared *vs = va->vs;
    Agg *a = va->a;
    int fd = open(vs->path, O_RDONLY);
    if (fd < 0) { a->bad++; return NULL; }
    size_t cap = (size_t)vs->ds->max_nbytes + 64;
    uint8_t *payload = (uint8_t *)malloc(cap);
    if (!payload) { close(fd); a->bad++; return NULL; }
    for (;;) {
        size_t i = atomic_fetch_add(&vs->next, 1);
        if (i >= vs->ds->n) break;
        const BlockRec *r = &vs->ds->v[i];
        const BlockHdr *h = &r->h;
        if (h->nbytes > 0) {
            ssize_t want = (ssize_t)h->nbytes;
            if (pread(fd, payload, (size_t)want, (off_t)(r->off + BLOCK_HDR_SIZE)) != want) {
                a->bad++;
                continue;
            }
        }
        const size_t intensive_step = (vs->ds->n > 5000) ? 2000 : 100;
        const int intensive = ((i % intensive_step) == 0);
        uint64_t cnt = 1;
        uint64_t cur = h->first_prime;
        const uint64_t mid_target = h->count / 2;
        uint64_t mid_val = 0;
        if (h->prev_prime != 0) {
            uint64_t g0 = h->first_prime - h->prev_prime;
            int ok_g0 = (g0 != 0) && (((g0 & 1) == 0) || (h->prev_prime == 2 && g0 == 1));
            if (!ok_g0) a->bad++;
            else {
                agg_gap(a, g0, h->first_prime);
                if (vs->do_mr && intensive && h->prev_prime != 2 && g0 >= 16)
                    check_gap_composites(a, g0, h->first_prime);
            }
        }
        if (h->first_prime == 2 && (h->count != 1 || h->nbytes != 0)) a->bad++;
        if (vs->do_res && cur >= 2) a->res12[cur % 12]++;
        size_t p = 0;
        while (p < h->nbytes) {
            uint64_t g = 0;
            int shift = 0;
            int done = 0;
            while (p < h->nbytes) {
                uint8_t byte = payload[p++];
                g |= (uint64_t)(byte & 0x7F) << shift;
                if (!(byte & 0x80)) { done = 1; break; }
                shift += 7;
                if (shift > 63) { a->bad++; break; }
            }
            if (!done) break;
            if (g == 0 || (g & 1)) a->bad++;
            cur += g;
            cnt++;
            if (cnt > h->count) { a->bad++; break; }
            agg_gap(a, g, cur);
            if (vs->do_res) a->res12[cur % 12]++;
            if (vs->do_mr) {
                if (g >= 256) check_gap_composites(a, g, cur);
                else if (intensive && g >= 16) check_gap_composites(a, g, cur);
            }
            if (cnt == mid_target) mid_val = cur;
        }
        if (p != h->nbytes) a->bad++;
        if (cnt != h->count) a->bad++;
        if (cur != h->end_prime) a->bad++;
        a->count += h->count;
        if (vs->do_mr) {
            a->mr_tests += 2;
            if (!is_prime_mr(h->first_prime)) a->bad++;
            if (!is_prime_mr(h->end_prime)) a->bad++;
            if (mid_val) {
                a->mr_tests++;
                if (!is_prime_mr(mid_val)) a->bad++;
            }
        }
    }
    free(payload);
    close(fd);
    return NULL;
}

static int cmd_verify(const char *path, int nthreads, int do_mr, int do_res, int json) {
    Dataset ds;
    double t0 = tnow();
    if (!ds_load(path, &ds)) return 1;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;
    if ((size_t)nthreads > ds.n) nthreads = (int)(ds.n ? ds.n : 1);
    VShared vs;
    memset(&vs, 0, sizeof vs);
    vs.ds = &ds;
    vs.path = path;
    vs.do_mr = do_mr;
    vs.do_res = do_res;
    atomic_init(&vs.next, 0);
    vs.agg = (Agg *)calloc((size_t)nthreads, sizeof(Agg));
    if (!vs.agg) { ds_free(&ds); return 1; }
    pthread_t *tids = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)nthreads);
    VArg *args = (VArg *)malloc(sizeof(VArg) * (size_t)nthreads);
    if (!tids || !args) { ds_free(&ds); free(vs.agg); return 1; }
    for (int i = 0; i < nthreads; i++) {
        args[i].vs = &vs;
        args[i].a = &vs.agg[i];
        if (pthread_create(&tids[i], NULL, verify_thread, &args[i]) != 0) {
            ds_free(&ds); free(tids); free(args); free(vs.agg);
            return 1;
        }
    }
    for (int i = 0; i < nthreads; i++) pthread_join(tids[i], NULL);

    Agg tot;
    memset(&tot, 0, sizeof tot);
    for (int i = 0; i < nthreads; i++) {
        Agg *a = &vs.agg[i];
        tot.count += a->count;
        tot.twin += a->twin;
        tot.sum_gap += a->sum_gap;
        tot.mr_tests += a->mr_tests;
        tot.comp_tests += a->comp_tests;
        tot.bad += a->bad;
        if (a->max_gap > tot.max_gap) {
            tot.max_gap = a->max_gap;
            tot.max_gap_after = a->max_gap_after;
        }
        for (int k = 0; k < 1026; k++) tot.hist[k] += a->hist[k];
        for (int k = 0; k < 12; k++) tot.res12[k] += a->res12[k];
    }
    const double elapsed = tnow() - t0;
    const int ok = (tot.bad == 0);
    if (json) {
        printf("{\"tool\":\"verify\",\"file\":\"%s\",\"blocks\":%zu,\"primes\":%" PRIu64
               ",\"first_prime\":%" PRIu64 ",\"last_prime\":%" PRIu64 ",\"bad\":%" PRIu64
               ",\"mr_tests\":%" PRIu64 ",\"comp_tests\":%" PRIu64 ",\"max_gap\":%" PRIu64
               ",\"max_gap_after\":%" PRIu64 ",\"twin_pairs\":%" PRIu64
               ",\"sum_gaps\":%" PRIu64 ",\"ok\":%d,\"elapsed_sec\":%.3f",
               path, ds.n, tot.count,
               ds.n ? ds.v[0].h.first_prime : 0,
               ds.n ? ds.v[ds.n - 1].h.end_prime : 0,
               tot.bad, tot.mr_tests, tot.comp_tests, tot.max_gap, tot.max_gap_after,
               tot.twin, tot.sum_gap, ok, elapsed);
        if (do_res) {
            uint64_t r3[3] = {0, 0, 0}, r4[4] = {0, 0, 0, 0}, r6[6] = {0, 0, 0, 0, 0, 0};
            for (int q = 0; q < 12; q++) {
                r3[q % 3] += tot.res12[q];
                r4[q % 4] += tot.res12[q];
                r6[q % 6] += tot.res12[q];
            }
            printf(",\"res3\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64
                   "],\"res4\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                   "],\"res6\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                   ",%" PRIu64 "]",
                   r3[0], r3[1], r3[2], r4[0], r4[1], r4[2], r4[3],
                   r6[0], r6[1], r6[2], r6[3], r6[4], r6[5]);
        }
        int first_hist = 1;
        printf(",\"gap_hist\":{");
        for (int k = 2; k <= 1024; k++) {
            if (!tot.hist[k]) continue;
            printf("%s\"%d\":%" PRIu64, first_hist ? "" : ",", k, tot.hist[k]);
            first_hist = 0;
        }
        if (tot.hist[1025]) printf("%s\"1025+\":%" PRIu64, first_hist ? "" : ",", tot.hist[1025]);
        printf("}}\n");
    } else {
        printf("file:            %s\n", path);
        printf("blocks:          %zu\n", ds.n);
        printf("primes:          %" PRIu64 "\n", tot.count);
        printf("first prime:     %" PRIu64 "\n", ds.n ? ds.v[0].h.first_prime : 0);
        printf("last prime:      %" PRIu64 "\n", ds.n ? ds.v[ds.n - 1].h.end_prime : 0);
        printf("bad checks:      %" PRIu64 "\n", tot.bad);
        printf("MR tests:        %" PRIu64 "\n", tot.mr_tests);
        printf("composite tests: %" PRIu64 "\n", tot.comp_tests);
        printf("max gap:         %" PRIu64 " (after prime %" PRIu64 ")\n", tot.max_gap, tot.max_gap_after);
        printf("twin pairs:      %" PRIu64 "\n", tot.twin);
        printf("sum of gaps:     %" PRIu64 "\n", tot.sum_gap);
        printf("elapsed:         %.2f s\n", elapsed);
        printf("STATUS:          %s\n", ok ? "PASS" : "FAIL");
    }
    free(vs.agg);
    free(tids);
    free(args);
    ds_free(&ds);
    return ok ? 0 : 4;
}

/* ---------------- count primes <= X ---------------- */
static int cmd_count(const char *path, uint64_t upto, int json) {
    Dataset ds;
    if (!ds_load(path, &ds)) return 1;
    uint64_t total = 0;
    int fd = -1;
    uint8_t *payload = NULL;
    for (size_t i = 0; i < ds.n; i++) {
        const BlockHdr *h = &ds.v[i].h;
        if (h->end_prime <= upto) {
            total += h->count;
            continue;
        }
        if (h->first_prime > upto) break;
        if (fd < 0) {
            fd = open(path, O_RDONLY);
            payload = (uint8_t *)malloc((size_t)ds.max_nbytes + 64);
            if (fd < 0 || !payload) { ds_free(&ds); return 1; }
        }
        if (pread(fd, payload, (size_t)h->nbytes, (off_t)(ds.v[i].off + BLOCK_HDR_SIZE)) !=
            (ssize_t)h->nbytes) {
            fprintf(stderr, "count: short read\n");
            free(payload);
            close(fd);
            ds_free(&ds);
            return 1;
        }
        uint64_t cur = h->first_prime;
        uint64_t c = 1;
        size_t p = 0;
        while (p < h->nbytes) {
            uint64_t g = 0;
            int shift = 0;
            while (p < h->nbytes) {
                uint8_t byte = payload[p++];
                g |= (uint64_t)(byte & 0x7F) << shift;
                if (!(byte & 0x80)) break;
                shift += 7;
            }
            cur += g;
            if (cur > upto) break;
            c++;
        }
        total += c;
        break;
    }
    if (json) printf("{\"tool\":\"count\",\"file\":\"%s\",\"upto\":%" PRIu64 ",\"count\":%" PRIu64 "}\n", path, upto, total);
    else printf("%" PRIu64 "\n", total);
    free(payload);
    if (fd >= 0) close(fd);
    ds_free(&ds);
    return 0;
}

/* ---------------- extract ---------------- */
static int cmd_extract(const char *path, uint64_t from, uint64_t to, uint64_t limit, const char *fmt) {
    Dataset ds;
    if (!ds_load(path, &ds)) return 1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { ds_free(&ds); return 1; }
    uint8_t *payload = (uint8_t *)malloc((size_t)ds.max_nbytes + 64);
    if (!payload) { close(fd); ds_free(&ds); return 1; }
    static char obuf[1 << 22];
    setvbuf(stdout, obuf, _IOFBF, sizeof obuf);
    uint64_t emitted = 0;
    const int text = !strcmp(fmt, "text");
    const int u32 = !strcmp(fmt, "u32");
    for (size_t i = 0; i < ds.n && (limit == 0 || emitted < limit); i++) {
        const BlockHdr *h = &ds.v[i].h;
        if (h->end_prime < from) continue;
        if (h->first_prime > to) break;
        if (h->nbytes > 0 &&
            pread(fd, payload, (size_t)h->nbytes, (off_t)(ds.v[i].off + BLOCK_HDR_SIZE)) !=
                (ssize_t)h->nbytes) {
            fprintf(stderr, "extract: short read\n");
            break;
        }
        uint64_t cur = h->first_prime;
        size_t p = 0;
        for (;;) {
            if (cur > to) break;
            if (cur >= from) {
                if (text) printf("%" PRIu64 "\n", cur);
                else if (u32) { uint32_t v = (uint32_t)cur; fwrite(&v, 4, 1, stdout); }
                else fwrite(&cur, 8, 1, stdout);
                emitted++;
                if (limit != 0 && emitted >= limit) break;
            }
            if (p >= h->nbytes) break;
            uint64_t g = 0;
            int shift = 0;
            while (p < h->nbytes) {
                uint8_t byte = payload[p++];
                g |= (uint64_t)(byte & 0x7F) << shift;
                if (!(byte & 0x80)) break;
                shift += 7;
            }
            cur += g;
        }
        if (cur > to) break;
    }
    fflush(stdout);
    free(payload);
    close(fd);
    ds_free(&ds);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  primes_tool info    FILE [--json]\n"
            "  primes_tool verify  FILE [--threads T] [--no-mr] [--residues] [--json]\n"
            "  primes_tool count   FILE --upto X [--json]\n"
            "  primes_tool summary FILE --upto-list X1,X2,... [--json]\n"
            "  primes_tool extract FILE [--from A] [--to B] [--limit K] [--format text|u64|u32]\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }
    const char *cmd = argv[1];
    const char *path = argv[2];
    int json = 0, do_mr = 1, do_res = 0, nthreads = 4;
    uint64_t upto = 0, from = 2, to = UINT64_MAX, limit = 0;
    const char *fmt = "text";
    uint64_t targets[64];
    size_t ntargets = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--no-mr")) do_mr = 0;
        else if (!strcmp(argv[i], "--residues")) do_res = 1;
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--upto") && i + 1 < argc) upto = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--upto-list") && i + 1 < argc) {
            char *s = argv[++i];
            while (*s && ntargets < 64) {
                char *end = NULL;
                uint64_t v = strtoull(s, &end, 10);
                if (end == s) break;
                targets[ntargets++] = v;
                s = end;
                if (*s == ',') s++;
            }
        }
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--format") && i + 1 < argc) fmt = argv[++i];
        else { usage(); return 2; }
    }
    if (!strcmp(cmd, "info")) return cmd_info(path, json);
    if (!strcmp(cmd, "verify")) return cmd_verify(path, nthreads, do_mr, do_res, json);
    if (!strcmp(cmd, "count")) {
        if (upto == 0) { usage(); return 2; }
        return cmd_count(path, upto, json);
    }
    if (!strcmp(cmd, "summary")) {
        if (ntargets == 0) { usage(); return 2; }
        return cmd_summary(path, targets, ntargets, json);
    }
    if (!strcmp(cmd, "extract")) return cmd_extract(path, from, to, limit, fmt);
    usage();
    return 2;
}
