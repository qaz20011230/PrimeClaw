# PrimeClaw

**Repository**: https://github.com/qaz20011230/PrimeClaw

**PrimeClaw** is a compact, verifiable archive of prime numbers. It computes
primes with a multithreaded segmented sieve, stores them in an append-only
gap-varint binary format (~1 byte per prime), and ships tooling to verify,
count, extract, and export the data.

The reference corpus currently contains **all 44,821,651,132 primes
<= 1.2 x 10^12** (last prime 1,199,999,999,993), verified end-to-end against
published values and by a second, independently written verifier.

The 45 GB binary corpus and the multi-gigabyte text exports are **not
committed to git** (size limits). Everything can be regenerated with the
commands below; verified snapshot hashes are recorded in
[data/MANIFEST.jsonl](data/MANIFEST.jsonl).

## Highlights

- **Fast**: 6-thread segmented sieve of Eratosthenes; all primes <= 10^12 in
  about 7 minutes on a 6-core laptop; <= 10^11 in 41 s.
- **Compact**: only gaps between consecutive primes are stored as LEB128
  varints, about 1.005 bytes per prime (44.8 billion primes = 42 GiB).
- **Append-only and crash-safe**: extending never rewrites existing bytes;
  a torn trailing block is detected and truncated on resume.
- **Verified**: 12-base deterministic Miller-Rabin sampling, exhaustive
  compositeness proofs inside large gaps, published pi(10^k) anchors, twin
  prime counts matching OEIS A007508, and an independently implemented
  verifier (see verify/).
- **Human-readable exports**: decimal text lists for the first million primes
  and all primes up to 10^6 / 10^7 are included in data/text/; larger lists
  are generated on demand.

## Verified data snapshot (round 3)

| property | value |
|---|---|
| primes stored | 44,821,651,132 (all primes <= 1.2 x 10^12) |
| first / last prime | 2 / 1,199,999,999,993 |
| binary corpus size | 45,065,036,384 bytes (41.97 GiB) |
| blocks | 286,104 |
| sha256 | 36c93c3b9a7750bf481d1f973568f1238b346401bf1aede401a8b1e340207b93 |
| published anchors | pi(10^12) = 37,607,912,018; record gap 540 at 738,832,927,927; twin counts match OEIS A007508 through 10^12 |
| independent verifier | PASS (44,821,651,132 primes decoded; 572,207 endpoint + 3,000 random MR checks; 894,259 gaps >= 256 with all 123,399,058 odd interiors proven composite) |

Extension to larger bounds is currently paused; the dataset is frozen and
verified. See [STATUS.md](STATUS.md).

## Quick start

    git clone git@github.com:qaz20011230/PrimeClaw.git
    cd PrimeClaw
    make -C src                 # builds bin/sieve_gap and bin/primes_tool

    # Build a (small) corpus: all primes <= 10^9
    bin/sieve_gap --out data/primes_gap.bin --limit 1000000000 --threads 6

    # Inspect / verify / query / export
    bin/primes_tool info     data/primes_gap.bin
    bin/primes_tool verify   data/primes_gap.bin --threads 6 --json
    bin/primes_tool count    data/primes_gap.bin --upto 1000000000
    bin/primes_tool extract  data/primes_gap.bin --from 100 --to 200
    bin/primes_tool extract  data/primes_gap.bin --limit 25

    # Extend the corpus (append-only), then record a verified snapshot
    python3 tools/extend.py --bound 10000000000 --threads 6 --label "my run"
    python3 tools/snapshot.py --bound 10000000000 --label "my snapshot"

Building the full 1.2 x 10^12 corpus takes about 10 minutes of CPU and 45 GB
of disk (sieve about 9.5 minutes + snapshot verify about 4.5 minutes on 6 cores).

## Format v1 (little-endian)

Global header (32 bytes):
    char magic[8] = "PRMGAP01"; u32 version = 1; u32 flags = 0;
    u64 chunk_odd; u64 reserved = 0

Each block: 48-byte header + payload.
    char magic[4] = "PRMB"; u16 version = 1; u16 flags = 0;
    u64 prev_prime;   /* last prime strictly before this block; 0 for block 0 */
    u64 first_prime;  /* first prime in this block */
    u64 end_prime;    /* last prime in this block */
    u64 count;        /* number of primes in this block */
    u64 nbytes;       /* payload length */

Payload: (count - 1) unsigned LEB128 varints, the gaps between consecutive
primes inside the block. The first block stores the singleton prime 2. All
later blocks contain odd primes, so gaps are even (except the 2 -> 3 gap
between blocks). Any reader can stream the file; the --resume option truncates
a torn tail and continues from the last valid prime.

## Repository layout

    PrimeClaw/
      src/                 C sources: sieve_gap.c (producer), primes_tool.c (consumer)
      src/Makefile
      tools/               extend.py, snapshot.py, ref_decoder.py
      data/MANIFEST.jsonl  verified snapshot records (counts, checkpoints, sha256)
      data/samples/        first_1000000_primes.txt
      data/text/           sample human-readable decimal lists + README
      verify/              independent verifier implementation and report
      reports/             statistics and analyses
      logs/                run records
      .github/workflows/   CI (build + smoke test)

## Human-readable exports

Included in the repository: the first 1,000,000 primes (7.9 MB) and all
primes <= 10^6 and <= 10^7. Larger text lists are one command away:

    bin/primes_tool extract data/primes_gap.bin --to 10000000000 --format text > primes_to_1e10.txt

A full text dump of all 44.8 billion primes would be about 550-600 GB; the
binary corpus is the complete archive.

## Verification

Every snapshot is checked by the Lead pipeline (primes_tool verify) and, when
available, by the independent verifier in verify/, which is written from the
format specification only. Both decode every block and check block linkage,
counts, strictly increasing primes, and exact payload consumption; they sample
Miller-Rabin primality checks and prove every odd number inside large gaps
composite. See [verify/VERIFICATION_REPORT.md](verify/VERIFICATION_REPORT.md)
and [reports/STATS.md](reports/STATS.md).

## License

MIT — see [LICENSE](LICENSE).
