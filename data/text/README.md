# Human-readable prime lists

Plain-text decimal prime lists extracted from the compact binary corpus
(data/primes_gap.bin, format v1 gap-varint). One prime per line, ascending.
All commands below assume the repository root as working directory.

## Files in this repository

| file | lines (= primes) | last line | size |
|---|---:|---:|---:|
| first_1000000_primes.txt | 1,000,000 | 15,485,863 | 7.9 MB |
| primes_upto_1000000.txt | 78,498 | 999,983 | 528 KB |
| primes_upto_10000000.txt | 664,579 | 9,999,991 | 5.0 MB |

Larger lists are generated locally (not committed because of size):

| bound | lines (= primes) | last line | approx size |
|---|---:|---:|---:|
| 10^8 | 5,761,455 | 99,999,989 | 49 MB |
| 10^9 | 50,847,534 | 999,999,937 | 493 MB |
| 10^10 | 455,052,511 | 9,999,999,967 | 4.6 GB |

All counts equal the published pi(10^k); the tier files are exact prefixes of
the larger files. The samples were extracted from a fully verified corpus
(sha256 36c93c3b9a7750bf481d1f973568f1238b346401bf1aede401a8b1e340207b93,
44,821,651,132 primes <= 1.2 x 10^12).

## Generate any list

    # all primes <= 10^8
    bin/primes_tool extract data/primes_gap.bin --to 100000000 --format text > primes_upto_1e8.txt

    # a range
    bin/primes_tool extract data/primes_gap.bin --from 1000000000000 --to 1000001000000

    # first N primes
    bin/primes_tool extract data/primes_gap.bin --limit 100000000 --format text > first_1e8.txt

A full text dump of all 44.8 billion primes would be about 550-600 GB; the
binary corpus is the complete archive.
