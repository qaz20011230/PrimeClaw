#!/usr/bin/env python3
"""make_synthetic.py -- build synthetic PRMGAP01 v1 datasets for self-testing.

These are genuine v1 files (same byte layout) built from a plain Python sieve.
Block layout produced here (matching what the real producer does structurally):
  * block 0 holds prime 2 alone (prev=0, first=end=2, count=1, nbytes=0);
  * remaining primes are grouped into consecutive blocks of chunk_primes primes;
  * payload = little-endian unsigned LEB128 of the internal gaps only.

Usage:
  make_synthetic.py LIMIT OUT [--chunk-primes N] [--chunk-odd N]
"""

from __future__ import annotations

import argparse
import struct
import sys

GLOBAL_MAGIC = b"PRMGAP01"
BLOCK_MAGIC = b"PRMB"
FORMAT_VERSION = 1
GLOBAL_STRUCT = struct.Struct("<8sIIQQ")
BLOCK_STRUCT = struct.Struct("<4sHHQQQQQ")


def sieve_primes(limit: int):
    """Return the list of all primes <= limit (bytearray sieve, no deps)."""
    if limit < 2:
        return []
    flags = bytearray(b"\x01") * (limit + 1)
    flags[0] = flags[1] = 0
    i = 2
    while i * i <= limit:
        if flags[i]:
            start = i * i
            step = i
            flags[start:limit + 1:step] = b"\x00" * (((limit - start) // step) + 1)
        i += 1
    return [n for n in range(2, limit + 1) if flags[n]]


def encode_uleb128(value: int) -> bytes:
    if value < 0:
        raise ValueError("LEB128 value must be non-negative")
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def partition_blocks(primes, chunk_primes):
    """Split a prime list into the synthetic block partition described above."""
    if not primes:
        raise ValueError("need at least prime 2")
    if primes[0] != 2:
        raise ValueError("prime list must start with 2")
    blocks = [[primes[0]]]
    for i in range(1, len(primes), chunk_primes):
        blocks.append(primes[i:i + chunk_primes])
    return blocks


def write_dataset(path, primes, chunk_primes=10_000, chunk_odd=None):
    """Write a complete v1 dataset file; returns basic file statistics."""
    blocks = partition_blocks(primes, chunk_primes)
    if chunk_odd is None:
        chunk_odd = max(2, 2 * chunk_primes)
    prev = 0
    payload_bytes = 0
    with open(path, "wb") as fp:
        fp.write(GLOBAL_STRUCT.pack(GLOBAL_MAGIC, FORMAT_VERSION, 0, chunk_odd, 0))
        for group in blocks:
            first, end, count = group[0], group[-1], len(group)
            payload = b"".join(
                encode_uleb128(b - a) for a, b in zip(group, group[1:])
            )
            fp.write(BLOCK_STRUCT.pack(BLOCK_MAGIC, FORMAT_VERSION, 0, prev,
                                       first, end, count, len(payload)))
            fp.write(payload)
            payload_bytes += len(payload)
            prev = end
    return {
        "path": path,
        "primes": len(primes),
        "blocks": len(blocks),
        "payload_bytes": payload_bytes,
        "file_size": GLOBAL_STRUCT.size + BLOCK_STRUCT.size * len(blocks) + payload_bytes,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description="Build a synthetic PRMGAP01 v1 dataset")
    parser.add_argument("limit", type=int, help="sieve all primes <= limit")
    parser.add_argument("out", help="output dataset path")
    parser.add_argument("--chunk-primes", type=int, default=10_000)
    parser.add_argument("--chunk-odd", type=int, default=None)
    args = parser.parse_args(argv)
    primes = sieve_primes(args.limit)
    info = write_dataset(args.out, primes, chunk_primes=args.chunk_primes,
                         chunk_odd=args.chunk_odd)
    print("wrote %s: %d primes in %d blocks, %d bytes"
          % (info["path"], info["primes"], info["blocks"], info["file_size"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
