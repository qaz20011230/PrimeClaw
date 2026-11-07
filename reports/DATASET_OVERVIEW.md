# Dataset Overview

## What this dataset is

`data/primes_gap.bin` is a **compact gap-encoded list of consecutive prime
numbers** from 2 up to the largest prime not exceeding 1,200,000,000,000,
stored as unsigned LEB128 gaps between consecutive primes, grouped into
independently checkable blocks (PRMGAP01 v1, little endian).

* File size: **45,065,036,384 bytes** (41.97 GiB).
* Blocks: 286,104 (informational chunk size: 2,097,152 odd numbers per writer chunk).
* SHA-256: `36c93c3b9a7750bf481d1f973568f1238b346401bf1aede401a8b1e340207b93`
* All 286,104 block headers plus the global header account exactly for the file size;
  the snapshot is complete and was treated as read-only.

## How many primes it holds

* **44,821,651,132 primes in total**; the first is 2, the last is **1,199,999,999,993**.
* The total is pi(1.2e12) = 44,821,651,132 and the largest saved prime is the largest prime
  below 1.2e12, matching the known pi(10^12) = 37,607,912,018 checkpoint.
* pi checkpoints from the dataset (header scan): pi(10^8) = 5,761,455, pi(10^9) = 50,847,534,
  pi(10^10) = 455,052,511, pi(10^11) = 4,118,054,813, pi(10^12) = 37,607,912,018.

## Spot checks (positions decoded from the file)

| Position | Prime | How checked |
|---:|---:|---|
| #1 | 2 | first prime in the file |
| #1,000,000 | **15,485,863** | decoded; line 1,000,000 of the sample file |
| #10,000,000 | **179,424,673** | decoded by the streaming scan (round 1; unchanged prefix) |
| #37,607,912,018 | **999,999,999,989** | largest prime below 10^12 = last prime of the 10^12 snapshot (round-2 full decode) |
| #44,821,651,132 (last) | **1,199,999,999,993** | `end_prime` of the final block; largest saved prime |

## Related artifacts

* `data/samples/first_1000000_primes.txt` -- first 1,000,000 primes,
  decimal, one per line (re-verified: 1,000,000 lines, line 1,000,000 = 15,485,863).
* `reports/STATS.md` -- pi tables, gaps, twin primes, average gaps.
* `reports/RESIDUE_BIAS.md` -- counts mod 4 and mod 3.
* `reports/pmgap.py`, `reports/fast_stats.c`, `reports/selftest.py`
  -- independent reader, fast scanner and 604-check self-test suite.
