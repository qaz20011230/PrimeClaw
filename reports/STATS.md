# Prime Dataset Statistics

Human-readable statistics for the gap-encoded prime dataset
`data/primes_gap.bin` (round-3 snapshot, all primes <= 1,200,000,000,000).
Generated: 2026-11-07 08:17:17 CST

## Snapshot facts

| Quantity | Value |
|---|---|
| File size | **45,065,036,384 bytes** (41.97 GiB) |
| SHA-256 | `36c93c3b9a7750bf481d1f973568f1238b346401bf1aede401a8b1e340207b93` |
| Format | PRMGAP01 v1 (gap-varint blocks, little endian) |
| Blocks | 286,104 |
| Total primes saved | **44,821,651,132** |
| Saved bound (largest prime) | **1,199,999,999,993** |
| Smallest saved prime | 2 |
| Declared computation limit | 1,200,000,000,000 |
| File complete (all blocks + global header accounted for) | yes |
| Block headers validated during this refresh | 286,104 |

The total is pi(1.2e12) = 44,821,651,132 (the dataset contains every prime up to the bound;
the largest saved prime 1,199,999,999,993 is the largest prime below 1.2e12).

## pi(10^k) computed from the dataset

Header-scan method: whole blocks are accounted for from their 48-byte header
`count` field and only the block straddling each target is decoded; every block
header is validated while streaming (magic, version, flags, chained `prev_prime`,
monotonicity and parity of the cross-block gap). This refresh decoded no full
payload: the scan of all 286,104 blocks over 45 GB took 53 s single-threaded.

| k | 10^k | pi(10^k) from dataset | Known reference | Match |
|---:|---:|---:|---:|:---:|
| 1 | 10 | 4 | 4 | yes |
| 2 | 100 | 25 | 25 | yes |
| 3 | 1,000 | 168 | 168 | yes |
| 4 | 10,000 | 1,229 | 1,229 | yes |
| 5 | 100,000 | 9,592 | 9,592 | yes |
| 6 | 1,000,000 | 78,498 | 78,498 | yes |
| 7 | 10,000,000 | 664,579 | 664,579 | yes |
| 8 | 100,000,000 | 5,761,455 | 5,761,455 | yes |
| 9 | 1,000,000,000 | 50,847,534 | 50,847,534 | yes |
| 10 | 10,000,000,000 | 455,052,511 | 455,052,511 | yes |
| 11 | 100,000,000,000 | 4,118,054,813 | 4,118,054,813 | yes |
| 12 | 1,000,000,000,000 | 37,607,912,018 | 37,607,912,018 | yes |

All twelve values match the published tables exactly. pi(10^12) = 37,607,912,018 is exact
because this snapshot extends past 10^12; pi(1.2e12) = 44,821,651,132 equals the saved total.

## Largest prime gaps

Gap statistics include every consecutive-prime pair in the range (cross-block
gaps are recovered exactly from header endpoints). Prefix rows up to 10^11 come
from analyst scans of the append-only prefix; the 10^12 row comes from the
analyst full decode of the 37,804,230,737-byte round-2 file.

| Range scanned | Largest gap | First prime of gap | Second prime of gap | Gaps counted |
|---|---:|---:|---:|---:|
| 2 .. 10^8 | **220** | 47,326,693 | 47,326,913 | 5,761,454 |
| 2 .. 10^9 | **282** | 436,273,009 | 436,273,291 | 50,847,533 |
| 2 .. 10^10 | **354** | 4,302,407,359 | 4,302,407,713 | 455,052,510 |
| 2 .. 10^11 | **464** | 42,652,618,343 | 42,652,618,807 | 4,118,054,812 |
| 2 .. 999,999,999,989 (10^12 snapshot) | **540** | 738,832,927,927 | 738,832,928,467 | 37,607,912,017 |
| 2 .. 1,199,999,999,993 (full 1.2e12 snapshot, verifier record) | **540** | 738,832,927,927 | 738,832,928,467 | 44,821,651,131 |

The record gap is **540** at 738,832,927,927 -> 738,832,928,467. It was found independently by the analyst
full decode of the 10^12 snapshot and is confirmed by the verifier record for the
1.2e12 snapshot (no larger gap occurs in the appended range 10^12 .. 1.2e12).
This is gap 540, first occurrence below 10^12, matching published prime-gap tables.

## Twin prime pairs (gap == 2)

| Bound | Twin pairs | Known reference | Match | Source |
|---:|---:|---:|:---:|---|
| 10^8 | **440,312** | 440,312 | yes | analyst decode of append-only prefix |
| 10^9 | **3,424,506** | 3,424,506 | yes | analyst decode of append-only prefix |
| 10^10 | **27,412,679** | 27,412,679 | yes | analyst decode of append-only prefix |
| 10^11 | **224,376,048** | 224,376,048 | yes | analyst decode of append-only prefix |
| 10^12 | **1,870,585,220** | 1,870,585,220 | yes | analyst decode of append-only prefix |
| 1.2e12 (full) | **2,214,085,660** | - | - | verifier record (data/MANIFEST.jsonl) |

Twin counts agree exactly with OEIS A007508 at every checkpoint through 10^12
(440,312; 3,424,506; 27,412,679; 224,376,048; 1,870,585,220). The verifier's
full-file count for the 1.2e12 snapshot is 2,214,085,660.

## Average gap over stated ranges

| Range | Gaps counted | Mean gap | Sum of gaps |
|---|---:|---:|---:|
| 2 .. 10^8 | 5,761,454 | 17.356727 | 99,999,987 |
| 2 .. 10^9 | 50,847,533 | 19.666636 | 999,999,935 |
| 2 .. 10^10 | 455,052,510 | 21.975486 | 9,999,999,965 |
| 2 .. 10^11 | 4,118,054,812 | 24.283310 | 99,999,999,975 |
| 2 .. 999,999,999,989 (10^12 snapshot) | 37,607,912,017 | 26.590149 | 999,999,999,987 |
| 2 .. 1,199,999,999,993 (full 1.2e12 snapshot) | 44,821,651,131 | 26.772775 | 1,199,999,999,991 |

The full-range row is exact from headers: the sum of all gaps from 2 to the last
prime is last_prime - 2 = 1,199,999,999,991 over 44,821,651,131 gaps.

## File composition

| Component | Bytes |
|---|---:|
| Global header | 32 |
| Block headers (286,104 x 48) | 13,732,992 |
| Payload (gap varints) | 45,051,303,360 |
| Payload share | 99.97% |
| Total | 45,065,036,384 |

Average payload per block: 157,464 bytes; average primes per block: 156,662.

## Sample artifact re-verification

`data/samples/first_1000000_primes.txt`: 1,000,000 lines, first line 2, last line 15485863.
Expected 1,000,000 lines and line 1,000,000 = 15,485,863: **PASS**. The file is
byte-for-byte identical to the first million primes decoded from the dataset.

## Methodology and self-checks

1. Tooling is independent of producer and verifier: `reports/pmgap.py`
   (Python reference decoder) and `reports/fast_stats.c` (C scanner).
2. Every block header was validated while streaming in this refresh (magic,
   version, flags, `prev_prime` chaining, monotonicity, cross-gap parity).
3. pi(10^k) uses header counts for whole blocks plus one decoded boundary block
   per target; no full-file decode was performed for this round-3 refresh.
4. Gap/twin rows up to 10^12 are from the analyst's full payload decode of the
   round-2 10^12 snapshot (37,804,230,737 bytes, all 37,607,912,018 primes); the full 1.2e12
   row (twin count 2,214,085,660, record gap 540, residue counts) is the independent
   verifier record from data/MANIFEST.jsonl for the current snapshot.
5. Self-checks: 604-check synthetic self-test suite passes (pi(10^4)=1229,
   pi(10^5)=9592, corruption/truncation rejection, C-vs-Python cross-check);
   all twelve pi checkpoints match published values; twin counts match OEIS
   A007508 exactly through 10^12; sample file re-verified as above.
6. SHA-256 and totals match the Lead/verifier metadata for the frozen snapshot;
   the dataset was treated as strictly read-only.

## Reproduction

```bash
cd reports && make fast_stats
./fast_stats ../data/primes_gap.bin --pi 10,100,1000,10000,100000,1000000,10000000,100000000,1000000000,10000000000,100000000000,1000000000000 --json _work/r3_pi_table.json
python3 refresh_round3.py
```
