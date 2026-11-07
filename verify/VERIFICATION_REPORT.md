# Independent verification report — `data/primes_gap.bin` (PRMGAP01 v1)

**Verdict: PASS** — the frozen snapshot is a valid, complete, self-consistent
list of every prime <= 10^11.

- Snapshot: `PrimeClaw/data/primes_gap.bin`
- Size: **4,130,475,289 bytes** (3,939.13 MiB), mtime 2026-11-07 03:35
- SHA-256 (computed by me with `shasum -a 256`):
  `33528f7b93df936e518037e811a16467e7df002cf9ff444bdf1b9f6f91f0f9ff`
  — **matches** the hash recorded by the Lead in `data/MANIFEST.jsonl`.
- Bound: all primes <= 100,000,000,000 (10^11); first prime 2, last 99,999,999,977.

## Tooling (written independently from the format specification)

| file | purpose |
|---|---|
| `verify/verify_primes.c` | main verifier: format parser + full decode + invariants + pi checkpoints + deterministic Miller-Rabin (bases 2,3,5,7,11,13,17,19,23,29,31,37) + largest-gap interior compositeness; multithreaded decode |
| `verify/twin_count.c` | second, separately written sequential decoder used for twin-prime pair counts (cross-check) |
| `verify/Makefile` | builds `verify_primes` |

No code from `src` or `tools` was read or reused; this verifier
was implemented only from the written v1 format spec (32-byte global header
`PRMGAP01`, 48-byte block header `PRMB`, (count-1) unsigned LEB128 gaps).

Miller-Rabin self-test (run before touching the dataset):

    $ ./verify_primes --mr-selftest 200000
    MR_SELFTEST_RANGE  : n=2..200000 compared with trial division: all match
    MR_SELFTEST_VECTORS: 23 known vectors tested: all match
    MR_SELFTEST_RESULT : PASS

(The vectors include Carmichael numbers, strong pseudoprimes, 2^61-1,
2^64-59 and several composites/edge cases; the 12-base set is deterministic
for n < 3.3e24, hence for all 64-bit inputs.)

## Exact commands and outputs

### 1) Header scan (all 23,843 block headers)

    $ ./verify_primes data/primes_gap.bin --meta
    DATASET            : PrimeClaw/data/primes_gap.bin
    MODE               : meta (headers only)
    FILE_SIZE          : 4130475289 bytes (3939.13 MiB)
    GLOBAL             : magic=PRMGAP01 version=1 flags=0 reserved=0 chunk_odd=2097152
    BLOCKS             : 23843
    TOTAL_PRIMES       : 4118054813 (sum of block headers)
    LAST_PRIME         : 99999999977
    FIRST_PRIME        : 2
    HEADER_SCAN        : PASS (0.262 s)
    LARGEST_BLOCK_GAP  : 268 (at prime 30278680429)
    RESULT             : PASS

Validated at header level: global magic/version/flags/reserved; block magic/
version/flags; `prev_prime` chain linkage (each block's `prev_prime` equals the
previous block's `end_prime`); block 0 is the prime-2 singleton
(prev=0, first=end=2, count=1, nbytes=0); `first_prime > prev_prime`; parity of
odd primes and cross-block gaps (even >= 2, except 2 -> 3); `end_prime >=
first_prime`; count/nbytes size bounds; payload fits in file; sum of counts
4,118,054,813; file has no trailing/partial block (exit code 0).

### 2) Full decode of ALL payloads (4 threads)

    $ ./verify_primes data/primes_gap.bin --full --threads 4 \
        --samples 5000 --gaps 2000 \
        --checkpoints 1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11 \
        --expect-pi '1e1:4,1e2:25,1e3:168,1e4:1229,1e5:9592,1e6:78498,1e7:664579,1e8:5761455,1e9:50847534,1e10:455052511,1e11:4118054813' \
        --expect-last 99999999977
    ...
    DECODED_PRIMES     : 4118054813 (4 threads, 12.346 s)
    CHECKPOINT 1e1      : 4 (expected 4) PASS
    CHECKPOINT 1e2      : 25 (expected 25) PASS
    CHECKPOINT 1e3      : 168 (expected 168) PASS
    CHECKPOINT 1e4      : 1229 (expected 1229) PASS
    CHECKPOINT 1e5      : 9592 (expected 9592) PASS
    CHECKPOINT 1e6      : 78498 (expected 78498) PASS
    CHECKPOINT 1e7      : 664579 (expected 664579) PASS
    CHECKPOINT 1e8      : 5761455 (expected 5761455) PASS
    CHECKPOINT 1e9      : 50847534 (expected 50847534) PASS
    CHECKPOINT 1e10     : 455052511 (expected 455052511) PASS
    CHECKPOINT 1e11     : 4118054813 (expected 4118054813) PASS
    MR_BLOCK_ENDPOINTS : 47685 block first/end primes checked, 0 failures
    MR_SAMPLES         : 5000 decoded primes checked (requested 5000, seed 6840143425043995324), 0 failures; examples: 2599777 6818179 14322697 18139811 20933599 25156357 42743089 62535731
    MAX_GAP            : 464 (consecutive primes 42652618343 .. 42652618807)
    AVG_STORED_GAP     : 24.2832 (over 4118030970 stored gaps)
    LARGE_GAP          : gap=464 interval=[42652618343, 42652618807] odd-interiors-checked=231 unexpected-primes=0 OK (endpoints prime, all interiors composite)
    LARGE_GAP          : gap=456 interval=[25056082087, 25056082543] odd-interiors-checked=227 unexpected-primes=0 OK (endpoints prime, all interiors composite)
    LARGE_GAP          : gap=450 interval=[63816175447, 63816175897] odd-interiors-checked=224 unexpected-primes=0 OK (endpoints prime, all interiors composite)
    LARGE_GAP          : gap=444 interval=[36172730063, 36172730507] odd-interiors-checked=221 unexpected-primes=0 OK (endpoints prime, all interiors composite)
    LARGE_GAP          : gap=432 interval=[87241770619, 87241771051] odd-interiors-checked=215 unexpected-primes=0 OK (endpoints prime, all interiors composite)
    GAP_INTERIOR_CHECK : 2000 largest gaps, 322800 odd numbers inside them tested composite, 0 unexpected primes, 0 failing gaps
    EXPECTED_LAST      : 99999999977 (expected 99999999977) PASS
    ELAPSED            : 12.527 s total (header scan 0.038 s, decode 12.346 s)
    RESULT             : PASS

Per-decode invariants checked for every one of the 4,118,054,813 primes:
exact LEB128 consumption (payload bytes consumed == `nbytes`), strictly
increasing primes, every stored gap an even number >= 2, decoded count ==
header count, last decoded == `end_prime`, global index alignment across
blocks, sum of decoded counts == 4,118,054,813.

### 3) SHA-256

    $ /usr/bin/time -p shasum -a 256 data/primes_gap.bin
    33528f7b93df936e518037e811a16467e7df002cf9ff444bdf1b9f6f91f0f9ff  data/primes_gap.bin
    real 21.37

Lead-recorded value in MANIFEST.jsonl: `33528f7b...f0f9ff` — **identical**.

### 4) Twin-prime pairs (second independent decoder, `twin_count.c`)

    $ ./twin_count data/primes_gap.bin \
        1000000000:3424506 10000000000:27412679 100000000000:224376048
    TOTAL_PRIMES       : 4118054813
    LAST_PRIME         : 99999999977
    TWIN_PAIRS_TOTAL   : 224376048 (last upper prime 99999999763)
    TWIN_PAIRS<=1000000000 : 3424506 (expected 3424506) PASS
    TWIN_PAIRS<=10000000000 : 27412679 (expected 27412679) PASS
    TWIN_PAIRS<=100000000000 : 224376048 (expected 224376048) PASS
    RESULT             : PASS

### 5) Manifest cross-check

`data/MANIFEST.jsonl` (read only): bound 100000000000, blocks 23843,
primes 4118054813, last_prime 99999999977, sha256 match; its `gap_hist`
sums to 4,118,054,811 gaps (all even gaps incl. block boundaries),
consistent with 4,118,054,812 consecutive pairs minus the odd gap 2 -> 3.

### 6) Extended interior check — EVERY gap >= 256

After the first PASS report, coverage was extended from the 2,000 largest gaps
to **every** gap >= 256 in the dataset (stored gaps and block-boundary gaps),
computed in parallel inside the same 4 decode threads:

    $ ./verify_primes data/primes_gap.bin --full --threads 4 --samples 5000 \
        --gaps 2000 --interior-min 256 \
        --checkpoints 1e1,...,1e11 --expect-pi '...' --expect-last 99999999977
    ...
    DECODED_PRIMES     : 4118054813 (4 threads, 10.112 s)
    CHECKPOINT 1e1..1e11: all PASS
    MR_BLOCK_ENDPOINTS : 47685 block first/end primes checked, 0 failures
    MR_SAMPLES         : 5000 decoded primes checked, 0 failures
    INTERIOR_MIN_CHECK : every gap >= 256: 22649 gaps, 3095936 odd numbers strictly inside tested, 0 unexpected primes
    MAX_GAP            : 464 (consecutive primes 42652618343 .. 42652618807)
    GAP_INTERIOR_CHECK : 2000 largest gaps, 322800 odd numbers inside them tested composite, 0 unexpected primes, 0 failing gaps
    EXPECTED_LAST      : 99999999977 (expected 99999999977) PASS
    RESULT             : PASS

So for all 22,649 gaps of size >= 256 in the snapshot, both endpoints are
prime and every odd number strictly between them is composite — i.e. those are
genuine maximal runs between consecutive primes. The independently recomputed
gap count 22,649 also matches the `gap_hist` count >= 256 in
`data/MANIFEST.jsonl`. File size and mtime were identical before and after
the run, and sha256 was re-checked afterwards: unchanged
`33528f7b...f0f9ff`.

## Verification matrix

| # | Check | Result |
|---|---|---|
| 1 | global header (magic/version/flags/reserved) | PASS |
| 2 | 23,843 block headers: magic/version/flags, prev linkage, first/end/count/nbytes bounds, parity, payload fits | PASS |
| 3 | block 0 == prime-2 singleton; first prime 2 | PASS |
| 4 | full payload decode: exact varint consumption, strictly increasing, all gaps even >= 2, count == header count, end == last decoded | PASS |
| 5 | total count 4,118,054,813 == pi(10^11) | PASS |
| 6 | pi(10^k) for k = 1..11 | all PASS |
| 7 | last prime 99,999,999,977 | PASS |
| 8 | Miller-Rabin: 47,685 block first/end primes + 5,000 random decoded primes (uniform by index, whole range) | 0 failures |
| 9 | largest gaps: 2,000 gaps, 322,800 odd interior numbers, all composite; endpoints prime | 0 failures |
| 9b | **every gap >= 256** (22,649 gaps): 3,095,936 odd interior numbers all composite, endpoints prime | 0 failures |
| 10 | max gap 464 between 42,652,618,343 and 42,652,618,807 | confirmed |
| 11 | twin-prime counts at 10^9, 10^10, 10^11 | all PASS |
| 12 | sha256 via shasum | PASS (matches MANIFEST) |

Runtime: 12.5 s for the full decode + checks, 10.3 s for the extended run
(with every gap >= 256 interior-checked), plus ~21 s for sha256; using
4 decode threads on 6 CPUs, 2 heavy processes max. Read-only throughout;
all artifacts written under `verify/` only.

## Honest limitations

- It is not computationally feasible to re-prove primality of all 4.1e9
  stored numbers in minutes; instead the checks above establish that the
  file is a structurally valid, gap-consistent, count-exact list whose
  aggregate statistics match known values (pi(10^k), twin counts), whose
  block endpoints and 5,000 uniformly sampled entries are prime, and whose
  2,000 largest gaps are true maximal runs between consecutive primes.
- Any byte-level change to the file after this snapshot invalidates this
  report; the verdict binds to SHA-256 `33528f7b...f0f9ff`.

## Artifacts

- `results/meta.txt` — header-scan output
- `results/full_verify.txt` — full verification output
- `results/sha256.txt`, `results/shasum_time.txt` — hash evidence
- `results/twins.txt`, `results/twins_time.txt` — twin-count evidence
- `results/full_verify_extended.txt` — extended run (every gap >= 256)
- `results/sha256_extended.txt` — sha256 re-checked after the extended run
- `results/selftest.txt` — complete verifier self-test log (52/52 passed)

## Appendix: self-test of the verifier itself

Before/independently of the dataset verification, the tool was exercised against
synthetic datasets produced by a **separate Python encoder** (`mk_synth.py`,
own Eratosthenes sieve) and a **pure-Python reference decoder**
(`ref_verify.py`). Reproduce with:

    $ cd verify && python3 run_selftest.py        # 52/52 tests PASS

Results summary (full log: `results/selftest.txt`):

- C Miller-Rabin vs trial division for every n in 2..200,000 plus 23 known
  vectors (Carmichael numbers, strong pseudoprimes, 2^61-1, 2^64-59, ...): PASS.
- Python reference Miller-Rabin vs trial division 2..5,000: PASS.
- Positive cases: datasets for limits 2, 100, 100,000 (1,000-prime blocks) and
  1,000,000 (variable block sizes 1..987, many block boundaries); meta + full
  verification PASS, all pi(x)/last-prime/max-gap values match the independent
  Python sieve exactly; C prime dump == Python sieve == Python reference dump
  (all 9,592 primes of the 10^5 dataset); empty dataset (header only) PASS.
- Corruption tests — verifier must reject (exit 1 = corrupt, 3 = incomplete
  trailing block), all checks pass: bad global magic/version/flags/reserved;
  block magic/version/flags; broken prev_prime linkage; wrong end_prime; wrong
  count; nbytes off by one; odd gap; zero gap; garbage after last block; payload
  truncated by 1 byte (exit 3); header truncated (exit 3); payload past EOF
  (exit 3).
- Semantically bad but structurally valid data: a composite stored as a block
  end is rejected by Miller-Rabin; a missing interior prime (gap 6 where 13 was
  removed) is caught by the gap-interior compositeness check.
- Headers-only mode (`--meta`) deliberately does not decode payload content,
  so payload-only corruptions are invisible to it by design (they are caught
  by `--full`).
- `shasum -a 256` == Python `hashlib.sha256` on synthetic datasets.

Self-test files live in `verify/synth/`; nothing outside
`verify/` is touched.

## Round 2 (1e12 snapshot, sha256 efdeaea5...aa673) — SUPERSEDED

Round-2 work by the verifier was not completed before the Lead issued the
round-3 request ("this supersedes"). No round-2 PASS/FAIL is claimed here.
Indirect result only: the round-3 prefix check below shows that
`sha256(first 37,804,230,737 bytes of the round-3 file) = efdeaea5...aa673`,
which is exactly the round-2 full-file sha256 recorded by the Lead — so the
round-2 snapshot bytes are confirmed unchanged (append-only extension).

## Round 3 (1.2e12 snapshot) — **PASS**

- Snapshot: `data/primes_gap.bin`, **45,065,036,384 bytes**, 286,104 blocks,
  mtime 2026-11-07 08:04:57; dataset frozen during verification.
- Prefix continuity: `sha256(first 37,804,230,737 bytes) =
  efdeaea5a4f26d15959165d113dadb2dbe5d771cd42bace21d046542834aa673` —
  **matches** the Lead-recorded round-2 full-file hash (round-1/2 bytes
  unchanged; append-only).
- Independently decoded all primes: **44,821,651,132** (= pi(10^12)),
  first prime 2, last prime **1,199,999,999,993**.
- pi(10^k) for k = 1..12: all exact
  (4, 25, 168, 1229, 9592, 78498, 664579, 5761455, 50847534, 455052511,
  4118054813, 37607912018).
- Miller-Rabin (12 deterministic bases): 572,207 block first/end primes +
  3,000 uniformly sampled decoded primes — **0 failures**.
- Gap interiors: **every gap >= 256** — 894,259 gaps, 123,399,058 odd numbers
  strictly inside tested, **0 unexpected primes** (all composite); endpoints
  prime.
- Max gap **540** between 738,832,927,927 and 738,832,928,467 (matches the
  known record gap), interiors all composite; avg stored gap 26.7726.
- Exact command (as requested by Lead):

      cd PrimeClaw
      ./verify/verify_primes data/primes_gap.bin --full --threads 4 --samples 3000 \
        --gaps 2000 --interior-min 256 \
        --checkpoints 1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,1e12 \
        --expect-pi '1e1:4,1e2:25,1e3:168,1e4:1229,1e5:9592,1e6:78498,1e7:664579,1e8:5761455,1e9:50847534,1e10:455052511,1e11:4118054813,1e12:37607912018'
      # exit 0, total 205.852 s (header scan 70.964 s, decode 4 threads 134.753 s)

- Raw logs: `results/round3/full_verify.txt`, `results/round3/verify_exit.txt`,
  `results/round3/prefix_sha.txt`.
- Not computed in round 3: full-file sha256 of the 45,065,036,384-byte file
  (not requested in the round-3 under-10-minute scope; the prefix check above
  covers the round-2 hash). Verdict binds to the frozen round-3 snapshot.
