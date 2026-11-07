/* fast_stats.c -- fast streaming scanner for the PRMGAP01 v1 prime dataset.
 *
 * Written from the published format spec only (independent of the producer).
 * Single-threaded, O(file size) streaming decode with per-block payload
 * validation.  Emits a JSON summary for the human-readable reports.
 *
 * Build:  make fast_stats     (see Makefile in this directory)
 * Usage:  fast_stats DATASET [--max X] [--pi t1,t2,...] [--first-n N]
 *                  [--first-out PATH] [--json PATH] [--verify-all]
 *
 *   --max X        decode every prime <= X and collect gap/residue stats
 *   --pi LIST      compute pi(t) for each target (whole blocks use the header
 *                  count; only the straddling block is decoded)
 *   --first-n N    also write the first N primes in decimal to --first-out
 *   --json PATH    write JSON there (default: stdout)
 *   --verify-all   decode and validate every payload in the file
 */
#define _DARWIN_C_SOURCE 1
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define GLOBAL_HEADER_SIZE 32
#define BLOCK_HEADER_SIZE 48
#define FORMAT_VERSION 1

static const char GLOBAL_MAGIC[8] = {'P', 'R', 'M', 'G', 'A', 'P', '0', '1'};
static const char BLOCK_MAGIC[4] = {'P', 'R', 'M', 'B'};

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "fast_stats: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t lo = rd32(p);
    uint64_t hi = rd32(p + 4);
    return lo | (hi << 32);
}

/* ------------------------------------------------------------------ */
/* Scanning context                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE *first_out;
    uint64_t first_n;
    uint64_t first_written;

    int stats_enabled;
    int stats_done;
    uint64_t max_stats;
    uint64_t blocks_scanned;
    uint64_t primes_counted;
    uint64_t first_prime_counted;
    uint64_t last_prime_counted;
    int have_last_counted;
    uint64_t gaps_counted;
    uint64_t gap_sum;
    int has_gap;
    uint64_t max_gap;
    uint64_t max_gap_start;
    uint64_t max_gap_end;
    uint64_t twins;
    uint64_t res3[3];
    uint64_t res4[4];

    uint64_t *pi_targets;
    uint64_t *pi_values;
    int *pi_exact;
    int n_pi;
    int pi_idx;
    uint64_t total_processed;

    uint64_t nth_targets[8];
    uint64_t nth_values[8];
    int n_nth;
    int nth_remaining;

    uint64_t decoded_payload_bytes;
} Ctx;

/* Process one prime of the dataset (in increasing order). */
static inline void handle_prime(Ctx *c, uint64_t p, uint64_t *prev, int *have_prev) {
    /* Resolve pi targets strictly smaller than p: every prime <= t has been
     * processed already and p is the first prime > t. */
    while (c->pi_idx < c->n_pi && c->pi_targets[c->pi_idx] < p) {
        c->pi_values[c->pi_idx] = c->total_processed;
        c->pi_exact[c->pi_idx] = 1;
        c->pi_idx++;
    }
    c->total_processed++;
    if (c->nth_remaining > 0) {
        for (int i = 0; i < c->n_nth; i++) {
            if (c->nth_values[i] == 0 && c->nth_targets[i] == c->total_processed) {
                c->nth_values[i] = p;
                c->nth_remaining--;
            }
        }
    }
    if (c->first_written < c->first_n) {
        fprintf(c->first_out, "%" PRIu64 "\n", p);
        c->first_written++;
    }
    if (c->stats_enabled && p <= c->max_stats) {
        if (*have_prev) {
            uint64_t g = p - *prev;
            c->gaps_counted++;
            c->gap_sum += g;
            if (g == 2) c->twins++;
            if (!c->has_gap || g > c->max_gap) {
                c->has_gap = 1;
                c->max_gap = g;
                c->max_gap_start = *prev;
                c->max_gap_end = p;
            }
        }
        if (!c->have_last_counted) {
            c->first_prime_counted = p;
            c->have_last_counted = 1;
        }
        c->last_prime_counted = p;
        c->primes_counted++;
        c->res3[p % 3]++;
        c->res4[p % 4]++;
        *prev = p;
        *have_prev = 1;
    }
}

/* Include one cross-block gap (endpoints are known from block headers). */
static inline void handle_cross_gap(Ctx *c, uint64_t a, uint64_t b) {
    if (!c->stats_enabled || c->stats_done || b > c->max_stats) return;
    uint64_t g = b - a;
    c->gaps_counted++;
    c->gap_sum += g;
    if (g == 2) c->twins++;
    if (!c->has_gap || g > c->max_gap) {
        c->has_gap = 1;
        c->max_gap = g;
        c->max_gap_start = a;
        c->max_gap_end = b;
    }
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                     */
/* ------------------------------------------------------------------ */

static uint64_t parse_u64(const char *text, const char *what) {
    /* Accepts plain decimal, plus scientific shorthand such as 1e8 / 1e11. */
    const char *e = strchr(text, 'e');
    if (e == NULL) e = strchr(text, 'E');
    if (e != NULL) {
        char basebuf[32];
        size_t blen = (size_t)(e - text);
        if (blen == 0 || blen >= sizeof(basebuf)) die("bad %s value %s", what, text);
        memcpy(basebuf, text, blen);
        basebuf[blen] = '\0';
        char *end1 = NULL;
        char *end2 = NULL;
        uint64_t base = strtoull(basebuf, &end1, 10);
        long exponent = strtol(e + 1, &end2, 10);
        if (*end1 != '\0' || *end2 != '\0' || exponent < 0 || exponent > 18)
            die("bad %s value %s", what, text);
        uint64_t result = base;
        for (long i = 0; i < exponent; i++) {
            if (result > UINT64_MAX / 10) die("bad %s value %s (overflow)", what, text);
            result *= 10;
        }
        return result;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0') die("bad %s value %s", what, text);
    return (uint64_t)v;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p < 0x20) {
            fprintf(out, "\\u%04x", *p);
        } else {
            fputc(*p, out);
        }
    }
    fputc('"', out);
}

int main(int argc, char **argv) {
    const char *dataset = NULL;
    const char *json_path = NULL;
    const char *first_out_path = NULL;
    const char *pi_arg = NULL;
    uint64_t max_stats = 0;
    int stats_enabled = 0;
    uint64_t first_n = 0;
    int verify_all = 0;
    uint64_t nth_targets[8];
    int n_nth = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            max_stats = parse_u64(argv[++i], "--max");
            stats_enabled = 1;
        } else if (strcmp(argv[i], "--pi") == 0 && i + 1 < argc) {
            pi_arg = argv[++i];
        } else if (strcmp(argv[i], "--first-n") == 0 && i + 1 < argc) {
            first_n = parse_u64(argv[++i], "--first-n");
        } else if (strcmp(argv[i], "--first-out") == 0 && i + 1 < argc) {
            first_out_path = argv[++i];
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            json_path = argv[++i];
        } else if (strcmp(argv[i], "--nth") == 0 && i + 1 < argc) {
            if (n_nth >= 8) die("--nth given more than 8 times");
            nth_targets[n_nth++] = parse_u64(argv[++i], "--nth");
        } else if (strcmp(argv[i], "--verify-all") == 0) {
            verify_all = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: fast_stats DATASET [--max X] [--pi t1,t2,...] "
                   "[--first-n N --first-out PATH] [--json PATH] [--verify-all]\n");
            return 0;
        } else if (argv[i][0] == '-') {
            die("unknown option %s (try --help)", argv[i]);
        } else if (dataset == NULL) {
            dataset = argv[i];
        } else {
            die("unexpected argument %s", argv[i]);
        }
    }
    if (dataset == NULL) die("missing DATASET argument (try --help)");
    if (first_n > 0 && first_out_path == NULL) die("--first-n requires --first-out PATH");

    struct stat st;
    if (stat(dataset, &st) != 0) die("cannot stat %s: %s", dataset, strerror(errno));
    uint64_t file_size = (uint64_t)st.st_size;

    FILE *fp = fopen(dataset, "rb");
    if (fp == NULL) die("cannot open %s: %s", dataset, strerror(errno));

    uint8_t gh[GLOBAL_HEADER_SIZE];
    if (fread(gh, 1, sizeof(gh), fp) != sizeof(gh))
        die("file shorter than the 32-byte global header");
    if (memcmp(gh, GLOBAL_MAGIC, 8) != 0) die("bad global header magic");
    uint32_t g_version = rd32(gh + 8);
    uint32_t g_flags = rd32(gh + 12);
    uint64_t chunk_odd = rd64(gh + 16);
    if (g_version != FORMAT_VERSION) die("unsupported global version %" PRIu32, g_version);
    if (g_flags != 0) die("unexpected global flags %#x", g_flags);

    Ctx c;
    memset(&c, 0, sizeof(c));
    c.max_stats = max_stats;
    c.stats_enabled = stats_enabled;
    c.first_n = first_n;
    for (int i = 0; i < n_nth; i++) c.nth_targets[i] = nth_targets[i];
    c.n_nth = n_nth;
    c.nth_remaining = n_nth;

    if (first_n > 0) {
        c.first_out = fopen(first_out_path, "w");
        if (c.first_out == NULL) die("cannot open %s for writing: %s", first_out_path,
                                     strerror(errno));
        static char outbuf[1 << 20];
        setvbuf(c.first_out, outbuf, _IOFBF, sizeof(outbuf));
    }

    /* pi target list */
    if (pi_arg != NULL && pi_arg[0] != '\0') {
        int count = 1;
        for (const char *p = pi_arg; *p; p++)
            if (*p == ',') count++;
        c.pi_targets = calloc((size_t)count, sizeof(uint64_t));
        c.pi_values = calloc((size_t)count, sizeof(uint64_t));
        c.pi_exact = calloc((size_t)count, sizeof(int));
        if (!c.pi_targets || !c.pi_values || !c.pi_exact) die("out of memory");
        int n = 0;
        char *copy = strdup(pi_arg);
        if (copy == NULL) die("out of memory");
        char *save = NULL;
        for (char *tok = strtok_r(copy, ",", &save); tok != NULL;
             tok = strtok_r(NULL, ",", &save)) {
            if (n >= count) die("internal error parsing --pi");
            c.pi_targets[n++] = parse_u64(tok, "--pi target");
        }
        free(copy);
        c.n_pi = n;
        qsort(c.pi_targets, (size_t)c.n_pi, sizeof(uint64_t), cmp_u64);
    }

    uint8_t *buf = NULL;
    size_t buf_cap = 0;

    uint64_t pos = GLOBAL_HEADER_SIZE;
    uint64_t prev_end = 0;
    uint64_t block_index = 0;
    uint64_t total_primes = 0;
    uint64_t total_payload = 0;
    uint64_t first_prime = 0;
    uint64_t last_prime = 0;
    int have_first = 0;
    int have_tail = 0;
    uint64_t tail_offset = 0;
    uint64_t tail_bytes = 0;
    const char *tail_reason = NULL;

    while (pos < file_size) {
        if (file_size - pos < BLOCK_HEADER_SIZE) {
            have_tail = 1;
            tail_offset = pos;
            tail_bytes = file_size - pos;
            tail_reason = "trailing bytes shorter than a block header";
            break;
        }
        if (fseeko(fp, (off_t)pos, SEEK_SET) != 0) die("seek failed at offset %" PRIu64, pos);
        uint8_t bh[BLOCK_HEADER_SIZE];
        if (fread(bh, 1, sizeof(bh), fp) != sizeof(bh))
            die("short read of block header at offset %" PRIu64, pos);
        if (memcmp(bh, BLOCK_MAGIC, 4) != 0)
            die("block %" PRIu64 " at offset %" PRIu64 ": bad magic", block_index, pos);
        uint16_t b_version = rd16(bh + 4);
        uint16_t b_flags = rd16(bh + 6);
        uint64_t prev_prime = rd64(bh + 8);
        uint64_t firstp = rd64(bh + 16);
        uint64_t endp = rd64(bh + 24);
        uint64_t count = rd64(bh + 32);
        uint64_t nbytes = rd64(bh + 40);

        if (b_version != FORMAT_VERSION)
            die("block %" PRIu64 ": version %" PRIu16, block_index, b_version);
        if (b_flags != 0) die("block %" PRIu64 ": flags %#x", block_index, b_flags);
        if (count < 1) die("block %" PRIu64 ": count < 1", block_index);
        if (firstp < 2) die("block %" PRIu64 ": first_prime < 2", block_index);
        if (count == 1) {
            if (nbytes != 0) die("block %" PRIu64 ": count==1 with nbytes!=0", block_index);
            if (endp != firstp) die("block %" PRIu64 ": count==1 with first!=end", block_index);
        } else {
            if (count - 1 > nbytes)
                die("block %" PRIu64 ": nbytes too small for count", block_index);
            if ((__uint128_t)nbytes > (__uint128_t)10 * (count - 1))
                die("block %" PRIu64 ": nbytes too large for count", block_index);
            if (endp <= firstp) die("block %" PRIu64 ": end_prime <= first_prime", block_index);
        }
        if (block_index == 0) {
            if (prev_prime != 0) die("block 0: prev_prime != 0");
            if (firstp != 2) die("block 0: first_prime != 2");
            if (!(count == 1 && endp == 2 && nbytes == 0))
                die("block 0 does not match the v1 spec (first=end=2, count=1, nbytes=0)");
        } else {
            if (prev_prime != prev_end)
                die("block %" PRIu64 ": prev_prime %" PRIu64 " != previous end_prime %" PRIu64,
                    block_index, prev_prime, prev_end);
            if (firstp <= prev_end)
                die("block %" PRIu64 ": first_prime <= prev_prime", block_index);
            uint64_t cross = firstp - prev_end;
            if (prev_end == 2) {
                if (cross != 1) die("block %" PRIu64 ": prime after 2 must be 3", block_index);
            } else if (cross < 2 || (cross & 1)) {
                die("block %" PRIu64 ": cross-block gap %" PRIu64 " not positive/even",
                    block_index, cross);
            }
        }
        uint64_t payload_off = pos + BLOCK_HEADER_SIZE;
        if (payload_off > file_size || nbytes > file_size - payload_off) {
            have_tail = 1;
            tail_offset = pos;
            tail_bytes = file_size - pos;
            tail_reason = "block payload exceeds file size";
            break;
        }

        /* The gap straddling the previous block boundary is not stored in any
         * payload, but both endpoints are in headers: recover it exactly. */
        if (block_index > 0 && c.stats_enabled && !c.stats_done && firstp <= c.max_stats)
            handle_cross_gap(&c, prev_end, firstp);

        /* pi targets below this block need no payload decode at all */
        while (c.pi_idx < c.n_pi && c.pi_targets[c.pi_idx] < firstp) {
            c.pi_values[c.pi_idx] = c.total_processed;
            c.pi_exact[c.pi_idx] = 1;
            c.pi_idx++;
        }
        int pi_boundary = (c.pi_idx < c.n_pi && c.pi_targets[c.pi_idx] <= endp);
        int need_decode =
            verify_all || (c.first_written < c.first_n) || pi_boundary ||
            (c.nth_remaining > 0) ||
            (c.stats_enabled && !c.stats_done && firstp <= c.max_stats);

        if (need_decode) {
            if (nbytes > buf_cap) {
                size_t want = (size_t)nbytes;
                uint8_t *nb = realloc(buf, want);
                if (nb == NULL) die("out of memory allocating %zu-byte payload buffer", want);
                buf = nb;
                buf_cap = want;
            }
            if (nbytes > 0 && fread(buf, 1, (size_t)nbytes, fp) != (size_t)nbytes)
                die("short read of payload for block %" PRIu64, block_index);
            uint64_t p = firstp;
            uint64_t block_prev = 0;
            int block_have_prev = 0;
            handle_prime(&c, p, &block_prev, &block_have_prev);
            uint64_t sum = 0;
            uint64_t seen = 1;
            uint64_t dpos = 0;
            while (dpos < nbytes) {
                uint64_t gap = 0;
                unsigned shift = 0;
                for (;;) {
                    if (dpos >= nbytes)
                        die("block %" PRIu64 ": truncated LEB128 varint", block_index);
                    uint8_t byte = buf[dpos++];
                    gap |= (uint64_t)(byte & 0x7F) << shift;
                    if ((byte & 0x80) == 0) break;
                    shift += 7;
                    if (shift >= 64)
                        die("block %" PRIu64 ": LEB128 varint overflows 64 bits", block_index);
                }
                if (gap == 0) die("block %" PRIu64 ": zero gap", block_index);
                if (p > 2 && (gap & 1))
                    die("block %" PRIu64 ": odd gap after odd prime", block_index);
                if (gap > UINT64_MAX - p)
                    die("block %" PRIu64 ": prime overflow", block_index);
                p += gap;
                if (p > endp)
                    die("block %" PRIu64 ": decoded prime beyond end_prime", block_index);
                sum += gap;
                seen++;
                if (seen > count)
                    die("block %" PRIu64 ": more gaps than count-1", block_index);
                handle_prime(&c, p, &block_prev, &block_have_prev);
            }
            if (seen != count)
                die("block %" PRIu64 ": decoded %" PRIu64 " primes but header count %" PRIu64,
                    block_index, seen, count);
            if (firstp + sum != endp)
                die("block %" PRIu64 ": first_prime + sum(gaps) != end_prime", block_index);
            c.decoded_payload_bytes += nbytes;
            if (c.stats_enabled && !c.stats_done && firstp <= c.max_stats) {
                c.blocks_scanned++;
                if (endp > c.max_stats) c.stats_done = 1;
            }
        } else {
            /* All primes of this block are accounted for: count them for the
             * pi bookkeeping of later blocks, then seek past the payload. */
            c.total_processed += count;
            if (fseeko(fp, (off_t)(payload_off + nbytes), SEEK_SET) != 0)
                die("seek failed at offset %" PRIu64, payload_off);
        }
        total_primes += count;
        total_payload += nbytes;
        if (!have_first) {
            first_prime = firstp;
            have_first = 1;
        }
        last_prime = endp;
        prev_end = endp;
        pos = payload_off + nbytes;
        block_index++;
        if (c.stats_enabled && !c.stats_done && firstp > c.max_stats) c.stats_done = 1;
    }

    if (first_n > 0 && c.first_written != first_n)
        die("dataset holds only %" PRIu64 " primes but --first-n asked for %" PRIu64,
            c.first_written, first_n);
    if (c.total_processed != total_primes)
        die("internal error: processed %" PRIu64 " primes but headers total %" PRIu64,
            c.total_processed, total_primes);

    /* finalize pi targets at/after the last saved prime */
    while (c.pi_idx < c.n_pi) {
        uint64_t t = c.pi_targets[c.pi_idx];
        if (have_first && t == last_prime) {
            c.pi_values[c.pi_idx] = total_primes;
            c.pi_exact[c.pi_idx] = 1;
        } else {
            c.pi_values[c.pi_idx] = 0;
            c.pi_exact[c.pi_idx] = 0;
        }
        c.pi_idx++;
    }
    if (c.stats_enabled && last_prime >= c.max_stats) c.stats_done = 1;

    uint64_t expected_size = GLOBAL_HEADER_SIZE + block_index * BLOCK_HEADER_SIZE + total_payload;
    int complete = (!have_tail && expected_size == file_size);

    if (c.first_out != NULL) {
        if (fclose(c.first_out) != 0) die("error closing first-out file");
        c.first_out = NULL;
    }

    /* ---------------- JSON output ---------------- */
    FILE *out = stdout;
    if (json_path != NULL) {
        out = fopen(json_path, "w");
        if (out == NULL) die("cannot open %s for writing: %s", json_path, strerror(errno));
    }
    fprintf(out, "{\n");
    fprintf(out, "  \"tool\": \"fast_stats\",\n");
    fprintf(out, "  \"dataset\": ");
    json_string(out, dataset);
    fprintf(out, ",\n");
    fprintf(out, "  \"file_size\": %" PRIu64 ",\n", file_size);
    fprintf(out, "  \"format_version\": %" PRIu32 ",\n", g_version);
    fprintf(out, "  \"chunk_odd\": %" PRIu64 ",\n", chunk_odd);
    fprintf(out, "  \"block_count\": %" PRIu64 ",\n", block_index);
    fprintf(out, "  \"total_primes\": %" PRIu64 ",\n", total_primes);
    fprintf(out, "  \"first_prime\": %" PRIu64 ",\n", first_prime);
    fprintf(out, "  \"last_prime\": %" PRIu64 ",\n", last_prime);
    fprintf(out, "  \"payload_bytes\": %" PRIu64 ",\n", total_payload);
    fprintf(out, "  \"expected_file_size\": %" PRIu64 ",\n", expected_size);
    fprintf(out, "  \"complete\": %s,\n", complete ? "true" : "false");
    if (have_tail) {
        fprintf(out, "  \"incomplete_tail\": {\"offset\": %" PRIu64
                     ", \"tail_bytes\": %" PRIu64 ", \"reason\": ",
                tail_offset, tail_bytes);
        json_string(out, tail_reason != NULL ? tail_reason : "truncated");
        fprintf(out, "},\n");
    } else {
        fprintf(out, "  \"incomplete_tail\": null,\n");
    }
    fprintf(out, "  \"verify_all\": %s,\n", verify_all ? "true" : "false");
    fprintf(out, "  \"decoded_payload_bytes\": %" PRIu64 ",\n", c.decoded_payload_bytes);
    fprintf(out, "  \"first_n_requested\": %" PRIu64 ",\n", first_n);
    fprintf(out, "  \"first_n_written\": %" PRIu64 ",\n", c.first_written);
    fprintf(out, "  \"nth_prime\": {");
    for (int i = 0; i < c.n_nth; i++) {
        fprintf(out, "%s\n    \"%" PRIu64 "\": ", i == 0 ? "" : ",", c.nth_targets[i]);
        if (c.nth_values[i] != 0)
            fprintf(out, "%" PRIu64, c.nth_values[i]);
        else
            fprintf(out, "null");
    }
    fprintf(out, "%s},\n", c.n_nth ? "\n  " : "");

    fprintf(out, "  \"pi\": {");
    for (int i = 0; i < c.n_pi; i++) {
        fprintf(out, "%s\n    \"%" PRIu64 "\": ", i == 0 ? "" : ",", c.pi_targets[i]);
        if (c.pi_exact[i])
            fprintf(out, "%" PRIu64, c.pi_values[i]);
        else
            fprintf(out, "null");
    }
    fprintf(out, "%s},\n", c.n_pi ? "\n  " : "");

    fprintf(out, "  \"stats\": {\n");
    fprintf(out, "    \"enabled\": %s,\n", c.stats_enabled ? "true" : "false");
    fprintf(out, "    \"target_max\": %" PRIu64 ",\n", c.max_stats);
    fprintf(out, "    \"target_covered\": %s,\n", c.stats_done ? "true" : "false");
    fprintf(out, "    \"blocks_scanned\": %" PRIu64 ",\n", c.blocks_scanned);
    fprintf(out, "    \"cross_block_gaps_included\": %" PRIu64 ",\n",
            c.blocks_scanned > 0 ? c.blocks_scanned - 1 : 0);
    fprintf(out, "    \"cross_block_gaps_omitted\": 0,\n");
    fprintf(out, "    \"primes_counted\": %" PRIu64 ",\n", c.primes_counted);
    fprintf(out, "    \"first_prime_counted\": %" PRIu64 ",\n", c.first_prime_counted);
    fprintf(out, "    \"last_prime_counted\": %" PRIu64 ",\n", c.last_prime_counted);
    fprintf(out, "    \"gaps_counted\": %" PRIu64 ",\n", c.gaps_counted);
    fprintf(out, "    \"gap_sum\": %" PRIu64 ",\n", c.gap_sum);
    if (c.gaps_counted > 0)
        fprintf(out, "    \"mean_gap\": %.9f,\n",
                (double)c.gap_sum / (double)c.gaps_counted);
    else
        fprintf(out, "    \"mean_gap\": null,\n");
    if (c.has_gap)
        fprintf(out, "    \"max_gap\": %" PRIu64 ",\n", c.max_gap);
    else
        fprintf(out, "    \"max_gap\": null,\n");
    if (c.has_gap) {
        fprintf(out, "    \"max_gap_start\": %" PRIu64 ",\n", c.max_gap_start);
        fprintf(out, "    \"max_gap_end\": %" PRIu64 ",\n", c.max_gap_end);
    } else {
        fprintf(out, "    \"max_gap_start\": null,\n");
        fprintf(out, "    \"max_gap_end\": null,\n");
    }
    fprintf(out, "    \"twin_pairs\": %" PRIu64 ",\n", c.twins);
    fprintf(out, "    \"residue_mod3\": [%" PRIu64 ", %" PRIu64 ", %" PRIu64 "],\n",
            c.res3[0], c.res3[1], c.res3[2]);
    fprintf(out, "    \"residue_mod4\": [%" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "]\n",
            c.res4[0], c.res4[1], c.res4[2], c.res4[3]);
    fprintf(out, "  }\n");
    fprintf(out, "}\n");

    if (json_path != NULL && fclose(out) != 0) die("error closing %s", json_path);

    free(buf);
    free(c.pi_targets);
    free(c.pi_values);
    free(c.pi_exact);
    fclose(fp);
    return 0;
}
