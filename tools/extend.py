#!/usr/bin/env python3
"""Extend the prime dataset to a new bound and record a verified snapshot.

Usage:
  extend.py --bound 1000000000000 [--threads 6] [--label "round-2"] [--no-snapshot]

Steps:
  1. disk-space guard (aborts if free space is not comfortably above estimate);
  2. bin/sieve_gap --resume --limit BOUND  (append-only extension);
  3. bin/primes_tool info;
  4. tools/snapshot.py --bound BOUND (verify + pi checkpoints + sha256 -> MANIFEST.jsonl).
Logs go to logs/extend-<bound>.log .
"""
import argparse
import math
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATASET = os.path.join(ROOT, "data", "primes_gap.bin")
SIEVE = os.path.join(ROOT, "bin", "sieve_gap")
TOOL = os.path.join(ROOT, "bin", "primes_tool")

MIN_FREE_AFTER = 12 * 1024**3  # keep at least ~12 GiB free


def last_prime():
    out = subprocess.check_output([TOOL, "info", DATASET, "--json"], text=True)
    import json
    return json.loads([ln for ln in out.splitlines() if ln.strip()][-1])["last_prime"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bound", type=int, required=True)
    ap.add_argument("--threads", type=int, default=6)
    ap.add_argument("--label", default="")
    ap.add_argument("--no-snapshot", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()

    if not os.path.exists(DATASET):
        print("dataset missing:", DATASET, file=sys.stderr)
        return 2
    cur = last_prime()
    if a.bound <= cur:
        print("bound %d <= last stored prime %d: nothing to do" % (a.bound, cur), file=sys.stderr)
        return 0
    # estimate new primes: integral dx/ln x over (cur, bound], * ~1.01 bytes/prime
    est_primes = (a.bound - cur) / max(math.log(a.bound), 2.0) * 1.05
    est_bytes = est_primes * 1.02
    free = shutil.disk_usage(ROOT).free
    print("current last prime: %d" % cur)
    print("target bound:       %d" % a.bound)
    print("estimated new data: %.2f GiB (est. %.3f billion new primes)" % (est_bytes / 1024**3, est_primes / 1e9))
    print("free disk:          %.2f GiB" % (free / 1024**3))
    if free < est_bytes + MIN_FREE_AFTER and not a.force:
        print("ABORT: not enough disk headroom (would leave < %.1f GiB free); use --force to override"
              % (MIN_FREE_AFTER / 1024**3), file=sys.stderr)
        return 2

    log_path = os.path.join(ROOT, "logs", "extend-%d.log" % a.bound)
    t0 = time.time()
    cmd = [SIEVE, "--out", DATASET, "--limit", str(a.bound), "--threads", str(a.threads), "--resume"]
    print("RUN:", " ".join(cmd), flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    with open(log_path, "a") as f:
        f.write("=== %s extend to %d (threads %d) ===\n" % (time.strftime("%Y-%m-%dT%H:%M:%S"), a.bound, a.threads))
        f.write("$ %s\n" % " ".join(cmd))
        f.write(r.stdout)
        f.write(r.stderr)
        f.write("exit=%d elapsed=%.2fs\n\n" % (r.returncode, time.time() - t0))
    if r.returncode != 0:
        print(r.stdout, file=sys.stderr)
        print(r.stderr, file=sys.stderr)
        return r.returncode
    print(r.stdout.strip())
    print("sieve finished in %.1f s; log: %s" % (time.time() - t0, log_path))

    print(subprocess.check_output([TOOL, "info", DATASET]).decode(), flush=True)
    if a.no_snapshot:
        return 0
    label = a.label or ("extend to %d" % a.bound)
    snap = [sys.executable, os.path.join(ROOT, "tools", "snapshot.py"),
            "--bound", str(a.bound), "--label", label]
    r2 = subprocess.run(snap, capture_output=True, text=True)
    with open(log_path, "a") as f:
        f.write("$ %s\n" % " ".join(snap))
        f.write(r2.stdout + r2.stderr + "\n")
    print(r2.stderr.strip())
    if r2.returncode != 0:
        return r2.returncode
    print("snapshot recorded: %s" % os.path.join(ROOT, "data", "MANIFEST.jsonl"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
