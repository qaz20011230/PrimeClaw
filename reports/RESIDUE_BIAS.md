# Residue-Class Bias of the Primes (Chebyshev Bias)

Prime counts by residue class modulo 4 and modulo 3.
Generated: 2026-11-07 08:17:17 CST

## Method

Rows up to 10^12 come from analyst payload decodes of the append-only prefix
(round-1/round-2 files). The full 1.2e12 row is the independent verifier record
from data/MANIFEST.jsonl (a full 45 GB decode was not repeated this round).
All residue class counts are reported; for each range the class counts sum to
pi(bound). Primes > 3 are only 1 or 2 mod 3; primes > 2 are only 1 or 3 mod 4
(class 0 of mod 4 is empty; class 2 of mod 4 holds only the prime 2; class 0
of mod 3 holds only the prime 3).

## Counts modulo 4

| Range | p=0 mod 4 | p=1 mod 4 | p=2 mod 4 | p=3 mod 4 | Total | Bias (3 mod 4)-(1 mod 4) |
|---:|---:|---:|---:|---:|---:|---:|
| 2 .. 10^8 | 0 | 2,880,504 | 1 | 2,880,950 | 5,761,455 | **+446** |
| 2 .. 10^9 | 0 | 25,423,491 | 1 | 25,424,042 | 50,847,534 | **+551** |
| 2 .. 10^10 | 0 | 227,523,275 | 1 | 227,529,235 | 455,052,511 | **+5960** |
| 2 .. 10^11 | 0 | 2,059,020,280 | 1 | 2,059,034,532 | 4,118,054,813 | **+14252** |
| 2 .. 999,999,999,989 (10^12 snapshot) | 0 | 18,803,924,340 | 1 | 18,803,987,677 | 37,607,912,018 | **+63337** |
| 2 .. 1,199,999,999,993 (full 1.2e12, verifier) | 0 | 22,410,801,577 | 1 | 22,410,849,554 | 44,821,651,132 | **+47977** |

Up to 10^8: **446 more primes 3 mod 4 than 1 mod 4** (bias +446). Over the full
1.2e12 range the bias is +47977 (verifier record). The bias direction is the known
Chebyshev bias sign and persists in every computed range.

## Counts modulo 3

| Range | p=0 mod 3 | p=1 mod 3 | p=2 mod 3 | Total | Bias (2 mod 3)-(1 mod 3) |
|---:|---:|---:|---:|---:|---:|
| 2 .. 10^8 | 1 | 2,880,517 | 2,880,937 | 5,761,455 | **+420** |
| 2 .. 10^9 | 1 | 25,422,713 | 25,424,820 | 50,847,534 | **+2107** |
| 2 .. 10^10 | 1 | 227,523,123 | 227,529,387 | 455,052,511 | **+6264** |
| 2 .. 10^11 | 1 | 2,059,018,668 | 2,059,036,144 | 4,118,054,813 | **+17476** |
| 2 .. 999,999,999,989 (10^12 snapshot) | 1 | 18,803,933,520 | 18,803,978,497 | 37,607,912,018 | **+44977** |
| 2 .. 1,199,999,999,993 (full 1.2e12, verifier) | 1 | 22,410,819,179 | 22,410,831,952 | 44,821,651,132 | **+12773** |

Up to 10^8: **420 more primes 2 mod 3 than 1 mod 3** (bias +420); full 1.2e12
range bias +12773 (verifier record).

## Cross-checks

* Analyst full decode of the 10^12 snapshot: mod4 [0, 18,803,924,340, 1, 18,803,987,677], mod3 [1, 18,803,933,520, 18,803,978,497].
* Verifier full-file record for 1.2e12: mod4 [0, 22,410,801,577, 1, 22,410,849,554], mod3 [1, 22,410,819,179, 22,410,831,952].
* Class totals equal pi(bound) for every row.
