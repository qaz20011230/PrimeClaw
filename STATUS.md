# PrimeClaw — status

Updated: 2026-11-07

## State: PAUSED — corpus frozen and verified

Extension work is paused; no computation is running. The binary corpus is
frozen at all primes <= 1.2 x 10^12 and has passed both the Lead's full
verification and an independent verifier implementation.

## Corpus (round 3 snapshot)

| item | value |
|---|---|
| file | data/primes_gap.bin (not committed to git) |
| primes | 44,821,651,132 |
| last prime | 1,199,999,999,993 |
| bytes | 45,065,036,384 (41.97 GiB) |
| blocks | 286,104 |
| sha256 | 36c93c3b9a7750bf481d1f973568f1238b346401bf1aede401a8b1e340207b93 |
| Lead verify | PASS (0 bad; 858,311 MR; 374,347,950 gap-composite checks) |
| independent verifier | PASS (own implementation; 44,821,651,132 primes decoded) |
| published anchors | pi(10^12)=37,607,912,018; gap 540 at 738,832,927,927; twin counts = OEIS A007508 through 10^12 |

Snapshot records: data/MANIFEST.jsonl (rounds 1-3, with sha256 and pi(10^k)).

## Human-readable files

In git: data/text/ first_1000000_primes.txt, primes_upto_1000000.txt,
primes_upto_10000000.txt; data/samples/first_1000000_primes.txt.

Generated locally but not committed (size): primes_upto_100000000.txt (49 MB),
primes_upto_1000000000.txt (493 MB), primes_upto_10000000000.txt (4.6 GB).
Regenerate any of them with:

    bin/primes_tool extract data/primes_gap.bin --to 1000000000 --format text > primes_upto_1e9.txt

## Reproduce / extend

    make -C src
    # full corpus from scratch (about 10 min on 6 cores, 45 GB disk):
    bin/sieve_gap --out data/primes_gap.bin --limit 1200000000000 --threads 6
    bin/primes_tool verify data/primes_gap.bin --threads 6 --residues --json
    python3 tools/snapshot.py --bound 1200000000000 --label "reproduce"

    # append larger bounds later:
    python3 tools/extend.py --bound 1350000000000 --threads 6 --label "next"

## Team / provenance

The project was built with an Agent Team (Lead + independent verifier +
independent analyst). The verifier implementation in verify/ was written by a
separate agent from the format specification, never reading the producer code.
