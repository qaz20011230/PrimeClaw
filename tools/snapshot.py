#!/usr/bin/env python3
"""Record a verified snapshot of the prime dataset into data/MANIFEST.jsonl.

Runs the C helper: info -> pi(10^k) checkpoints (k=1..12) -> full verify
(with --residues) -> optional sha256, then appends one JSON record.
"""
import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Published values of pi(10^k) (k = 1..13).
KNOWN_PI = {
    1: 4,
    2: 25,
    3: 168,
    4: 1229,
    5: 9592,
    6: 78498,
    7: 664579,
    8: 5761455,
    9: 50847534,
    10: 455052511,
    11: 4118054813,
    12: 37607912018,
    13: 346065536839,
}
BIN = os.path.join(ROOT, "bin", "primes_tool")
DEFAULT_DATASET = os.path.join(ROOT, "data", "primes_gap.bin")
DEFAULT_MANIFEST = os.path.join(ROOT, "data", "MANIFEST.jsonl")


def run_json(args):
    out = subprocess.check_output(args, text=True)
    lines = [ln for ln in out.splitlines() if ln.strip()]
    return json.loads(lines[-1])


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", default=DEFAULT_DATASET)
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--label", default="")
    ap.add_argument("--bound", type=int, default=0,
                    help="sieve bound (defaults to last stored prime)")
    ap.add_argument("--threads", type=int, default=6)
    ap.add_argument("--no-verify", action="store_true")
    ap.add_argument("--no-sha", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    t0 = time.time()

    def stamp(msg):
        print("[snapshot] %6.1fs %s" % (time.time() - t0, msg), file=sys.stderr, flush=True)

    # One header scan yields file info AND every pi(10^k) checkpoint.
    # (Fall back to last_prime if the dataset is too short for the scan.)
    probe = run_json([BIN, "info", a.dataset, "--json"])
    bound = a.bound if a.bound else probe["last_prime"]
    targets = [10 ** k for k in range(1, 13) if 10 ** k <= bound]
    summary = run_json([BIN, "summary", a.dataset,
                        "--upto-list", ",".join(str(t) for t in targets), "--json"])
    stamp("summary (1 scan): %d blocks, %d primes" % (summary["blocks"], summary["primes"]))
    want = set(targets)
    checkpoints = {
        "10^%d" % k: c["count"]
        for c, k in zip(summary["checkpoints"], range(1, 13))
        if 10 ** k in want
    }
    info = {
        "tool": "info",
        "file": a.dataset,
        "bytes": summary["bytes"],
        "blocks": summary["blocks"],
        "primes": summary["primes"],
        "first_prime": summary["first_prime"],
        "last_prime": summary["last_prime"],
        "chunk_odd": summary["chunk_odd"],
        "ok": summary["ok"],
    }
    rec = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "label": a.label,
        "dataset": os.path.relpath(a.dataset, ROOT),
        "bound": bound,
        "info": info,
        "summary_elapsed_sec": summary["elapsed_sec"],
    }
    rec["checkpoints_pi"] = checkpoints
    stamp("checkpoints: " + ", ".join("%s=%d" % (k, v) for k, v in sorted(checkpoints.items(), key=lambda kv: int(kv[0].split("^")[1]))))
    matches = {}
    for key, computed in checkpoints.items():
        k = int(key.split("^")[1])
        known = KNOWN_PI.get(k)
        matches[key] = {
            "computed": computed,
            "known": known,
            "match": (known == computed) if known is not None else None,
        }
    rec["pi_matches"] = matches
    if not a.no_verify:
        rec["verify"] = run_json(
            [BIN, "verify", a.dataset, "--threads", str(a.threads), "--residues", "--json"]
        )
        stamp("verify: ok=%s bad=%s mr=%s comp=%s (%.1fs)"
              % (rec["verify"]["ok"], rec["verify"]["bad"], rec["verify"]["mr_tests"],
                 rec["verify"]["comp_tests"], rec["verify"]["elapsed_sec"]))
    if not a.no_sha:
        rec["sha256"] = sha256_file(a.dataset)
        stamp("sha256: " + rec["sha256"])
    rec["snapshot_elapsed_sec"] = round(time.time() - t0, 3)
    with open(a.manifest, "a") as f:
        f.write(json.dumps(rec, sort_keys=True) + "\n")
    if not a.quiet:
        print(json.dumps(rec, indent=2, sort_keys=True))
    bad = [k for k, v in matches.items() if v["match"] is False]
    if bad:
        print("PIPELINE FAILED known pi checks:", bad, file=sys.stderr)
        return 3
    print("known-pi checks: PASS (%d checkpoints)" % len(matches), file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
