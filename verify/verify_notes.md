# Verifier notes — independent PRMGAP01 v1 tooling

Everything here was implemented from the written format specification only;
no code from `src` or `tools` was read or reused.
Main results: see **VERIFICATION_REPORT.md** (round-1 snapshot verdict: PASS,
bound to sha256 `33528f7b93df936e518037e811a16467e7df002cf9ff444bdf1b9f6f91f0f9ff`).

## Build

    cd verify
    make                    # -> ./verify_primes   (C, -O2 -pthread)
    cc -O2 -o twin_count twin_count.c

## Full verification of a frozen snapshot

    ./verify_primes PrimeClaw/data/primes_gap.bin --full \
      --threads 4 --samples 5000 --gaps 2000 --interior-min 256 \
      --checkpoints 1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11 \
      --expect-pi '1e1:4,1e2:25,1e3:168,1e4:1229,1e5:9592,1e6:78498,1e7:664579,1e8:5761455,1e9:50847534,1e10:455052511,1e11:4118054813' \
      --expect-last 99999999977
    # exit 0 = PASS, 1 = FAIL/corrupt, 2 = usage/IO, 3 = incomplete trailing block

Safe on a dataset still being appended (headers only, fast, does not decode):

    ./verify_primes <dataset> --meta        # exit 3 = benign partial tail

Twin-prime cross-check (second independent decoder):

    ./twin_count <dataset> 1000000000:3424506 10000000000:27412679 100000000000:224376048

SHA-256 (no dependence on this tool):

    shasum -a 256 <dataset>

Self-test (synthetic + corrupted datasets; C + Python cross-checks):

    python3 run_selftest.py                 # 52/52 PASS, log in results/selftest.txt

Independent Python reference decoder (small datasets):

    python3 ref_verify.py --dataset synth/t1e5.bin --expect-json synth/t1e5.expect.json \
        --samples 300 --gaps 100

Synthetic encoder / fault injector:

    python3 mk_synth.py --limit 100000 --block 1000 --out synth/x.bin \
        --expect-json synth/x.expect.json --write-primes synth/x.primes.txt
    python3 mk_synth.py ... --fault gap_odd      # corrupt label; must be rejected

## What is checked

- global header: magic/version/flags/reserved
- every block header: magic/version/flags, `prev_prime` chain, first block =
  prime-2 singleton, `first_prime > prev_prime`, parity/even-gap rules,
  first/end/count/nbytes consistency, payload bounds and file coverage
- full payload decode: exact LEB128 consumption, strictly increasing primes,
  every gap even >= 2, decoded count == header count, last decoded == end_prime
- pi(10^k) checkpoint counts, total count, last prime
- deterministic Miller-Rabin: 12 bases (deterministic for n < 3.3e24), block
  endpoints + N uniformly sampled decoded primes
- top-K largest gaps and (optionally) every gap >= G: endpoints prime and all
  odd interiors composite => genuine consecutive-prime runs
- optional prime dump for exhaustive small-dataset cross-checks

## Round-1 result (snapshot sha256 33528f7b...f0f9ff)

- 23,843 blocks / 4,130,475,289 bytes / 4,118,054,813 primes / last 99,999,999,977
- all header + payload invariants PASS; pi(10^k) k=1..11 exact; sha256 exact
- 47,685 block-endpoint + 5,000 random Miller-Rabin checks, 0 failures
- every gap >= 256 (22,649 gaps, 3,095,936 odd interiors) fully composite inside, 0 failures
- twin pairs: 3,424,506 / 27,412,679 / 224,376,048 at 10^9 / 10^10 / 10^11 (exact)
- max gap 464 between 42,652,618,343 and 42,652,618,807
- runtime: 12.5 s full decode (4 threads) + 10.3 s extended run + ~21 s sha256
