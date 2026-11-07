/*
 * sieve_gap.c — multithreaded segmented sieve of Eratosthenes writing primes
 * in the project's compact gap-varint block format (v1).
 *
 * Usage:
 *   sieve_gap --out FILE --limit N [--threads T] [--chunk-odd C]
 *             [--resume] [--overwrite] [--no-fsync] [--quiet]
 *
 * Format v1 (little-endian):
 *   Global header 32 B: magic "PRMGAP01", u32 version=1, u32 flags=0,
 *                       u64 chunk_odd, u64 reserved
 *   Block: 48 B header "PRMB": u16 version, u16 flags,
 *          u64 prev_prime, u64 first_prime, u64 end_prime, u64 count,
 *          u64 nbytes
 *          payload: (count-1) LEB128 varints = consecutive-prime gaps
 *   The very first block stores the single prime 2; all later blocks cover
 *   odd candidates only, so all encoded gaps are post-3 even gaps.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
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
#define MAX_VARINT_GAP (1ULL << 14)   /* our ranges have max gap < 1000 */

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

_Static_assert(sizeof(GlobalHdr) == GLOBAL_HDR_SIZE, "global header must be 32 bytes");
_Static_assert(sizeof(BlockHdr) == BLOCK_HDR_SIZE, "block header must be 48 bytes");

static double tnow(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static void die(const char *msg) {
    fprintf(stderr, "sieve_gap: FATAL: %s (errno=%d: %s)\n", msg, errno, strerror(errno));
    exit(1);
}

/* ---------------- base primes up to sqrt(limit) ---------------- */
static uint32_t *g_base = NULL;
static size_t g_nbase = 0;

static void make_base_primes(uint64_t lim) {
    if (lim < 2) return;
    uint8_t *comp = (uint8_t *)calloc((size_t)lim + 1, 1);
    if (!comp) die("calloc base sieve");
    size_t alloc = 1u << 16, n = 0;
    uint32_t *ps = (uint32_t *)malloc(alloc * sizeof(uint32_t));
    if (!ps) die("malloc base primes");
    for (uint64_t i = 2; i <= lim; i++) {
        if (comp[i]) continue;
        if (n == alloc) {
            alloc <<= 1;
            ps = (uint32_t *)realloc(ps, alloc * sizeof(uint32_t));
            if (!ps) die("realloc base primes");
        }
        ps[n++] = (uint32_t)i;
        if (i * i <= lim)
            for (uint64_t j = i * i; j <= lim; j += i) comp[j] = 1;
    }
    free(comp);
    g_base = ps;
    g_nbase = n;
}

static size_t first_odd_base(void) {
    size_t i = 0;
    while (i < g_nbase && g_base[i] < 3) i++;
    return i;
}

/* ---------------- shared pipeline state ---------------- */
typedef struct {
    uint8_t *data;
    size_t cap;
    int state;        /* 0 = free, 1 = filled */
    int64_t index;    /* chunk index while filled */
    uint64_t first, last, count;
    size_t nbytes;
} Buf;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv_free;
    pthread_cond_t cv_filled;
    int64_t next_chunk;
    int64_t consumed;
    int64_t total_chunks;
    uint64_t start_low;
    uint64_t limit;
    uint64_t chunk_odd;
    Buf bufs[64];
    int nbuf;
} Shared;

typedef struct { Shared *sh; } WorkerArg;

static inline void chunk_span(Shared *sh, int64_t idx, uint64_t *low, uint64_t *len) {
    uint64_t l = sh->start_low + 2ULL * sh->chunk_odd * (uint64_t)idx;
    uint64_t rem = (sh->limit - l) / 2 + 1;
    *low = l;
    *len = rem < sh->chunk_odd ? rem : sh->chunk_odd;
}

/* Sieve one chunk and gap-varint encode it into b->data. */
static void sieve_and_encode(Shared *sh, int64_t idx, uint8_t *comp, Buf *b) {
    uint64_t low, len;
    chunk_span(sh, idx, &low, &len);
    const uint64_t end = low + 2 * (len - 1);
    memset(comp, 0, (size_t)len);

    for (size_t bi = first_odd_base(); bi < g_nbase; bi++) {
        const uint64_t p = g_base[bi];
        if (p > end / p) break; /* p*p > end */
        uint64_t m = ((low + p - 1) / p) * p;
        if ((m & 1ULL) == 0) m += p;
        const uint64_t pp = p * p;
        if (m < pp) m = pp;
        if (m > end) continue;
        uint64_t j = (m - low) >> 1;
        for (; j < len; j += p) comp[j] = 1;
    }

    uint8_t *out = b->data;
    uint64_t first = 0, last = 0, count = 0;
    for (uint64_t j = 0; j < len; j++) {
        if (comp[j]) continue;
        const uint64_t n = low + 2 * j;
        if (count == 0) {
            first = last = n;
            count = 1;
        } else {
            uint64_t gap = n - last;
            last = n;
            count++;
            if (gap >= MAX_VARINT_GAP) die("unexpectedly large prime gap");
            while (gap >= 0x80) {
                *out++ = (uint8_t)((gap & 0x7F) | 0x80);
                gap >>= 7;
            }
            *out++ = (uint8_t)gap;
        }
    }
    b->first = first;
    b->last = last;
    b->count = count;
    b->nbytes = (size_t)(out - b->data);
}

static void *worker_main(void *arg) {
    Shared *sh = ((WorkerArg *)arg)->sh;
    uint8_t *comp = (uint8_t *)malloc((size_t)sh->chunk_odd);
    if (!comp) die("malloc comp");
    for (;;) {
        int64_t idx;
        pthread_mutex_lock(&sh->mu);
        while (sh->next_chunk >= sh->consumed + sh->nbuf &&
               sh->next_chunk < sh->total_chunks)
            pthread_cond_wait(&sh->cv_free, &sh->mu);
        if (sh->next_chunk >= sh->total_chunks) {
            pthread_mutex_unlock(&sh->mu);
            break;
        }
        idx = sh->next_chunk++;
        pthread_mutex_unlock(&sh->mu);

        Buf *b = &sh->bufs[idx % sh->nbuf];
        sieve_and_encode(sh, idx, comp, b);

        pthread_mutex_lock(&sh->mu);
        b->index = idx;
        b->state = 1;
        pthread_cond_broadcast(&sh->cv_filled);
        pthread_mutex_unlock(&sh->mu);
    }
    free(comp);
    return NULL;
}

/* ---------------- output handling ---------------- */
static void write_block(FILE *f, BlockHdr *bh) {
    if (fwrite(bh, sizeof *bh, 1, f) != 1) die("fwrite block header");
}

static void usage(void) {
    fprintf(stderr,
            "usage: sieve_gap --out FILE --limit N [--threads T] [--chunk-odd C]\n"
            "                 [--resume] [--overwrite] [--no-fsync] [--quiet]\n");
}

int main(int argc, char **argv) {
    const char *out = NULL;
    uint64_t limit = 0;
    int nthreads = 6;
    uint64_t chunk_odd = 1ULL << 21;
    int resume = 0, overwrite = 0, do_fsync = 1, quiet = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chunk-odd") && i + 1 < argc) chunk_odd = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--resume")) resume = 1;
        else if (!strcmp(argv[i], "--overwrite")) overwrite = 1;
        else if (!strcmp(argv[i], "--no-fsync")) do_fsync = 0;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else { usage(); return 2; }
    }
    if (!out || limit == 0) { usage(); return 2; }
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;
    if (chunk_odd < 1024) chunk_odd = 1024;
    if (chunk_odd > (1ULL << 25)) chunk_odd = 1ULL << 25;

    const double t0 = tnow();

    /* base primes up to floor(sqrt(limit)) */
    uint64_t sq = (uint64_t)sqrtl((long double)limit);
    while ((sq + 1) * (sq + 1) <= limit) sq++;
    while (sq * sq > limit) sq--;
    make_base_primes(sq);

    /* open output (new or resume) */
    struct stat st;
    int exists = (stat(out, &st) == 0);
    FILE *f = NULL;
    int have_blocks = 0;
    uint64_t last_end = 0;
    uint64_t blocks_written = 0, primes_added = 0;

    if (!resume) {
        if (exists && st.st_size > 0 && !overwrite) {
            fprintf(stderr, "sieve_gap: refusing to overwrite %s (use --overwrite)\n", out);
            return 2;
        }
        f = fopen(out, "wb+");
        if (!f) die("create output");
        GlobalHdr gh;
        memset(&gh, 0, sizeof gh);
        memcpy(gh.magic, GLOBAL_MAGIC, 8);
        gh.version = FORMAT_VERSION;
        gh.chunk_odd = chunk_odd;
        if (fwrite(&gh, sizeof gh, 1, f) != 1) die("write global header");
    } else if (!exists || st.st_size == 0) {
        f = fopen(out, "wb+");
        if (!f) die("create output");
        GlobalHdr gh;
        memset(&gh, 0, sizeof gh);
        memcpy(gh.magic, GLOBAL_MAGIC, 8);
        gh.version = FORMAT_VERSION;
        gh.chunk_odd = chunk_odd;
        if (fwrite(&gh, sizeof gh, 1, f) != 1) die("write global header");
    } else {
        f = fopen(out, "r+b");
        if (!f) die("open for resume");
        uint64_t pos = 0, fsize = (uint64_t)st.st_size;
        if (fsize < GLOBAL_HDR_SIZE) {
            /* torn header: restart from scratch */
            if (ftruncate(fileno(f), 0) != 0) die("ftruncate");
            if (fseeko(f, 0, SEEK_SET) != 0) die("fseek");
            GlobalHdr gh;
            memset(&gh, 0, sizeof gh);
            memcpy(gh.magic, GLOBAL_MAGIC, 8);
            gh.version = FORMAT_VERSION;
            gh.chunk_odd = chunk_odd;
            if (fwrite(&gh, sizeof gh, 1, f) != 1) die("write global header");
        } else {
            GlobalHdr gh;
            if (fseeko(f, 0, SEEK_SET) != 0) die("fseek");
            if (fread(&gh, sizeof gh, 1, f) != 1) die("read global header");
            if (memcmp(gh.magic, GLOBAL_MAGIC, 8) || gh.version != FORMAT_VERSION) {
                fprintf(stderr, "sieve_gap: bad global header in %s\n", out);
                return 3;
            }
            pos = GLOBAL_HDR_SIZE;
            uint64_t nblocks = 0;
            while (pos + BLOCK_HDR_SIZE <= fsize) {
                BlockHdr bh;
                if (fseeko(f, (off_t)pos, SEEK_SET) != 0) die("fseek");
                if (fread(&bh, sizeof bh, 1, f) != 1) break;
                if (memcmp(bh.magic, BLOCK_MAGIC, 4) || bh.version != FORMAT_VERSION) break;
                if (bh.count < 1 || bh.end_prime < bh.first_prime) break;
                if (nblocks == 0) {
                    if (bh.prev_prime != 0) break;
                } else {
                    if (bh.prev_prime != last_end || bh.first_prime <= last_end) break;
                }
                if (pos + BLOCK_HDR_SIZE + bh.nbytes > fsize) break;
                pos += BLOCK_HDR_SIZE + bh.nbytes;
                last_end = bh.end_prime;
                nblocks++;
            }
            if (pos != fsize) {
                fprintf(stderr, "sieve_gap: truncating %s from %" PRIu64 " to %" PRIu64 " bytes\n",
                        out, fsize, pos);
                if (ftruncate(fileno(f), (off_t)pos) != 0) die("ftruncate tail");
            }
            have_blocks = nblocks > 0;
            if (fseeko(f, 0, SEEK_END) != 0) die("seek end");
            if (!quiet)
                fprintf(stderr, "[resume] %s: %" PRIu64 " blocks, last prime %" PRIu64 "\n",
                        out, nblocks, last_end);
        }
    }

    /* Emit prime 2 as its own singleton block for a fresh dataset. */
    if (!have_blocks && limit >= 2) {
        BlockHdr bh;
        memset(&bh, 0, sizeof bh);
        memcpy(bh.magic, BLOCK_MAGIC, 4);
        bh.version = FORMAT_VERSION;
        bh.prev_prime = 0;
        bh.first_prime = 2;
        bh.end_prime = 2;
        bh.count = 1;
        bh.nbytes = 0;
        write_block(f, &bh);
        last_end = 2;
        blocks_written++;
        primes_added++;
        have_blocks = 1;
    }

    uint64_t start_low = 0;
    if (limit >= 2) start_low = (last_end == 2) ? 3 : last_end + 2;

    int64_t total_chunks = 0;
    if (start_low != 0 && start_low <= limit)
        total_chunks = (int64_t)(((limit - start_low) / 2 + 1 + chunk_odd - 1) / chunk_odd);

    if (total_chunks > 0) {
        int nbuf = 2 * nthreads + 4;
        if (nbuf > 64) nbuf = 64;
        Shared sh;
        memset(&sh, 0, sizeof sh);
        pthread_mutex_init(&sh.mu, NULL);
        pthread_cond_init(&sh.cv_free, NULL);
        pthread_cond_init(&sh.cv_filled, NULL);
        sh.next_chunk = 0;
        sh.consumed = 0;
        sh.total_chunks = total_chunks;
        sh.start_low = start_low;
        sh.limit = limit;
        sh.chunk_odd = chunk_odd;
        sh.nbuf = nbuf;
        size_t cap = (size_t)(2 * chunk_odd + 64);
        for (int i = 0; i < nbuf; i++) {
            sh.bufs[i].data = (uint8_t *)malloc(cap);
            if (!sh.bufs[i].data) die("malloc pipeline buffer");
            sh.bufs[i].cap = cap;
            sh.bufs[i].state = 0;
            sh.bufs[i].index = -1;
        }

        int nworkers = nthreads;
        if ((int64_t)nworkers > total_chunks) nworkers = (int)total_chunks;
        pthread_t *tids = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)nworkers);
        if (!tids) die("malloc tids");
        WorkerArg *wargs = (WorkerArg *)malloc(sizeof(WorkerArg) * (size_t)nworkers);
        if (!wargs) die("malloc wargs");
        for (int i = 0; i < nworkers; i++) {
            wargs[i].sh = &sh;
            if (pthread_create(&tids[i], NULL, worker_main, &wargs[i]) != 0) die("pthread_create");
        }

        double t_progress = tnow();
        uint64_t progress_blocks = 0;
        for (int64_t i = 0; i < total_chunks; i++) {
            Buf *b = &sh.bufs[i % sh.nbuf];
            pthread_mutex_lock(&sh.mu);
            while (!(b->state == 1 && b->index == i))
                pthread_cond_wait(&sh.cv_filled, &sh.mu);
            pthread_mutex_unlock(&sh.mu);

            if (b->count > 0) {
                BlockHdr bh;
                memset(&bh, 0, sizeof bh);
                memcpy(bh.magic, BLOCK_MAGIC, 4);
                bh.version = FORMAT_VERSION;
                bh.prev_prime = last_end;
                bh.first_prime = b->first;
                bh.end_prime = b->last;
                bh.count = b->count;
                bh.nbytes = b->nbytes;
                write_block(f, &bh);
                if (b->nbytes > 0 && fwrite(b->data, 1, b->nbytes, f) != b->nbytes)
                    die("fwrite payload");
                last_end = b->last;
                blocks_written++;
                primes_added += b->count;
            }

            pthread_mutex_lock(&sh.mu);
            b->state = 0;
            b->index = -1;
            sh.consumed = i + 1;
            pthread_cond_broadcast(&sh.cv_free);
            pthread_mutex_unlock(&sh.mu);

            if (!quiet) {
                double tn = tnow();
                if (tn - t_progress >= 2.0 || i == total_chunks - 1) {
                    uint64_t db = (uint64_t)(i + 1 - (int64_t)progress_blocks);
                    double dt = tn - t_progress;
                    fprintf(stderr,
                            "[sieve] chunk %" PRId64 "/%" PRId64 " (%5.1f%%), last prime %" PRIu64
                            ", %.0f blocks/s\n",
                            i + 1, total_chunks,
                            100.0 * (double)(i + 1) / (double)total_chunks,
                            last_end, dt > 0 ? (double)db / dt : 0.0);
                    t_progress = tn;
                    progress_blocks = (uint64_t)(i + 1);
                }
            }
        }
        for (int i = 0; i < nworkers; i++) pthread_join(tids[i], NULL);
        for (int i = 0; i < nbuf; i++) free(sh.bufs[i].data);
        free(tids);
        free(wargs);
        pthread_mutex_destroy(&sh.mu);
        pthread_cond_destroy(&sh.cv_free);
        pthread_cond_destroy(&sh.cv_filled);
    }

    if (fflush(f) != 0) die("fflush");
    if (do_fsync && fsync(fileno(f)) != 0) die("fsync");
    if (fclose(f) != 0) die("fclose");

    struct stat st2;
    if (stat(out, &st2) != 0) die("stat output");
    const double elapsed = tnow() - t0;
    printf("{\"tool\":\"sieve_gap\",\"out\":\"%s\",\"limit\":%" PRIu64
           ",\"chunk_odd\":%" PRIu64 ",\"threads\":%d,\"blocks_written\":%" PRIu64
           ",\"primes_added\":%" PRIu64 ",\"last_prime\":%" PRIu64
           ",\"file_bytes\":%" PRIu64 ",\"elapsed_sec\":%.3f}\n",
           out, limit, chunk_odd, nthreads, blocks_written, primes_added, last_end,
           (uint64_t)st2.st_size, elapsed);
    free(g_base);
    return 0;
}
