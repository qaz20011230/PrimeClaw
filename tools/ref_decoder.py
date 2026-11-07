#!/usr/bin/env python3
"""Minimal pure-Python reference decoder for the gap-varint prime dataset (v1).

This is intentionally simple and slow; it exists to document the format and to
let anyone inspect the corpus without building the C tools.

Usage:
  ref_decoder.py count   DATASET [--upto X]
  ref_decoder.py first   DATASET N
  ref_decoder.py range   DATASET A B
  ref_decoder.py info    DATASET
"""
import argparse
import struct
import sys

GLOBAL_MAGIC = b"PRMGAP01"
BLOCK_MAGIC = b"PRMB"
MIN_SIZE = 32
BLOCK_SIZE = 48


def read_global(f):
    hdr = f.read(32)
    if len(hdr) != 32:
        raise ValueError("file too short for global header")
    magic, version, flags, chunk_odd, reserved = struct.unpack("<8sIIQQ", hdr)
    if magic != GLOBAL_MAGIC or version != 1:
        raise ValueError("bad global header")
    return {"chunk_odd": chunk_odd}


def iter_blocks(path, f=None):
    """Yield (prev, first, end, count, payload) for every block."""
    own = f is None
    if own:
        f = open(path, "rb")
    try:
        read_global(f)
        while True:
            hdr = f.read(48)
            if not hdr:
                return
            if len(hdr) != 48:
                raise ValueError("truncated block header")
            magic, version, flags, prev, first, end, count, nbytes = struct.unpack(
                "<4sHHQQQQQ", hdr
            )
            if magic != BLOCK_MAGIC or version != 1:
                raise ValueError("bad block header")
            payload = f.read(nbytes)
            if len(payload) != nbytes:
                raise ValueError("truncated payload")
            yield prev, first, end, count, payload
    finally:
        if own:
            f.close()


def decode_gaps(payload, count):
    """Decode exactly count-1 LEB128 gaps from payload."""
    gaps = []
    value = 0
    shift = 0
    for byte in payload:
        value |= (byte & 0x7F) << shift
        if byte & 0x80:
            shift += 7
        else:
            gaps.append(value)
            value = 0
            shift = 0
    if len(gaps) != count - 1:
        raise ValueError("payload does not encode count-1 gaps")
    return gaps


def iter_primes(path):
    for prev, first, end, count, payload in iter_blocks(path):
        p = first
        yield p
        for g in decode_gaps(payload, count):
            p += g
            yield p


def cmd_info(path):
    total = 0
    blocks = 0
    first = last = None
    for prev, f, e, count, payload in iter_blocks(path):
        if blocks == 0:
            first = f
        blocks += 1
        total += count
        last = e
    print("blocks:", blocks)
    print("primes:", total)
    print("first:", first)
    print("last:", last)


def cmd_count(path, upto):
    total = 0
    for p in iter_primes(path):
        if p > upto:
            break
        total += 1
    print(total)


def cmd_first(path, n):
    shown = 0
    for p in iter_primes(path):
        print(p)
        shown += 1
        if shown >= n:
            break


def cmd_range(path, a, b):
    for p in iter_primes(path):
        if p > b:
            break
        if p >= a:
            print(p)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_info = sub.add_parser("info")
    p_info.add_argument("dataset")
    p_count = sub.add_parser("count")
    p_count.add_argument("dataset")
    p_count.add_argument("--upto", type=int, required=True)
    p_first = sub.add_parser("first")
    p_first.add_argument("dataset")
    p_first.add_argument("n", type=int)
    p_range = sub.add_parser("range")
    p_range.add_argument("dataset")
    p_range.add_argument("a", type=int)
    p_range.add_argument("b", type=int)
    a = ap.parse_args(argv)
    if a.cmd == "info":
        cmd_info(a.dataset)
    elif a.cmd == "count":
        cmd_count(a.dataset, a.upto)
    elif a.cmd == "first":
        cmd_first(a.dataset, a.n)
    elif a.cmd == "range":
        cmd_range(a.dataset, a.a, a.b)
    return 0


if __name__ == "__main__":
    sys.exit(main())
