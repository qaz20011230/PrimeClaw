# Analyst Tooling Runbook

Self-contained, independent tooling for reading the PRMGAP01 v1 prime dataset and
producing the human-readable artifacts. Nothing here is imported by, or imports, the
producer or verifier code.

## Files

| File | Purpose |
|---|---|
| `pmgap.py` | From-scratch Python decoder library + CLI (info / pi / stats / verify / extract). Strict by default; `--lenient` tolerates a mid-write incomplete tail. |
| `make_synthetic.py` | Builds genuine v1 datasets from a plain Python sieve (for self-tests). |
| `selftest.py` | Self-test suite: 604 checks (known pi(10^4)=1229, pi(10^5)=9592, six chunk layouts, corruption/truncation rejection, C-vs-Python cross-check). |
| `fast_stats.c` / `Makefile` / `fast_stats` | Fast single-threaded C scanner used for production runs (validates every decoded block; emits JSON). |
| `make_reports.py` | Renders `STATS.md`, `RESIDUE_BIAS.md`, `DATASET_OVERVIEW.md` from the JSON outputs in `_work/`. |
| `_work/` | Raw JSON scan outputs (provenance for the reports). |
| `_selftest/` | Synthetic datasets + corruption fixtures from the last self-test run. |

## Quick start

```bash
cd reports
make fast_stats            # build the C scanner
python3 selftest.py        # 604-check self-test suite (uses only synthetic data)
```

## Decoder CLI (Python)

```bash
python3 pmgap.py info   ../data/primes_gap.bin                    # header-only summary
python3 pmgap.py pi     ../data/primes_gap.bin 1e3,1e4,1e5,1e8    # pi(x) table
python3 pmgap.py stats  ../data/primes_gap.bin 1e8                # gap/residue stats
python3 pmgap.py verify ../data/primes_gap.bin                    # full payload validation (slow)
python3 pmgap.py extract ../data/primes_gap.bin 100               # first N primes
# add --lenient to tolerate a snapshot captured mid-append
```

## Fast scanner (C) -- production path

```bash
# summary + pi table only (~3 s for the 4.1 GB file; headers + boundary blocks):
./fast_stats ../data/primes_gap.bin \
  --pi 10,100,1000,10000,100000,1000000,10000000,100000000,1000000000,10000000000,100000000000 \
  --json _work/pi_table.json

# prefix statistics + first-N artifact + ordinals (decodes only the needed prefix):
./fast_stats ../data/primes_gap.bin --max 100000000 \
  --first-n 1000000 --first-out ../data/samples/first_1000000_primes.txt \
  --nth 1000000 --nth 10000000 --json _work/prefix_1e8.json

# further prefixes / full dataset:
./fast_stats ../data/primes_gap.bin --max 1000000000  --json _work/prefix_1e9.json
./fast_stats ../data/primes_gap.bin --max 10000000000 --json _work/prefix_1e10.json
./fast_stats ../data/primes_gap.bin --max 100000000000 --json _work/full_1e11.json

# full-payload validation of every block (no --max):
./fast_stats ../data/primes_gap.bin --verify-all --json _work/verify_all.json
```

## Notes

* Gap statistics include the cross-block gap recovered from header endpoints
  (`first_prime - prev_prime`), so every consecutive-prime gap in the scanned range is
  counted exactly once.
* pi(x) uses block header counts for whole blocks and decodes only the straddling block;
  all headers are validated while streaming.
* The 4.1 GB dataset decodes and fully validates in a single ~26 s single-threaded pass
  on this machine (Apple silicon, -O3).
* Regenerate the markdown reports with `python3 make_reports.py` (it re-verifies the
  1,000,000-line sample file and recomputes the dataset SHA-256).
