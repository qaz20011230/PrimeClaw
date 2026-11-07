#!/usr/bin/env python3
"""pmgap.py -- independent reader/decoder for the PRMGAP01 v1 prime dataset.

Written from the published format specification only; it shares no code with the
producer (src) or the independent verifier (verify).

Format v1, little endian
------------------------
Global header, 32 bytes:
    char magic[8] = "PRMGAP01"
    u32  version  = 1
    u32  flags    = 0
    u64  chunk_odd   (informational: odd numbers per writer chunk)
    u64  reserved = 0

Then a sequence of blocks.  Each block is a 48-byte header followed by nbytes
payload bytes:
    char magic[4] = "PRMB"
    u16  version = 1
    u16  flags = 0
    u64  prev_prime   last prime strictly before this block (0 for the first block)
    u64  first_prime  first prime stored in this block
    u64  end_prime    last prime stored in this block
    u64  count        number of primes in this block (>= 1)
    u64  nbytes       payload length in bytes
Payload = exactly (count - 1) unsigned LEB128 varints: the gaps between
consecutive primes inside the block (g2-g1, g3-g2, ...).  The gap from
prev_prime to first_prime is NOT stored because it straddles the block
boundary.  The very first block is prev_prime=0, first_prime=end_prime=2,
count=1, nbytes=0.

Consequence worth remembering for statistics: gaps that cross block boundaries
are absent from the file, so gap statistics computed here are "within-block"
statistics.  With the producer's large blocks this omits only (#blocks - 1)
gaps out of billions.

Validation performed here
--------------------------
* global header magic/version/flags;
* every block header: magic/version/flags, prev_prime chained to the previous
  block's end_prime, first_prime > prev_prime, parity/size sanity of the
  (unstored) cross-block gap, count/nbytes consistency bounds;
* when payloads are decoded: exact number of varints, exact payload length,
  first_prime + sum(gaps) == end_prime, positive gaps, even gaps after odd
  primes.
"""

from __future__ import annotations

import argparse
import hashlib
import heapq
import json
import os
import struct
import sys
from dataclasses import dataclass

GLOBAL_MAGIC = b"PRMGAP01"
BLOCK_MAGIC = b"PRMB"
GLOBAL_HEADER_SIZE = 32
BLOCK_HEADER_SIZE = 48
FORMAT_VERSION = 1

GLOBAL_STRUCT = struct.Struct("<8sIIQQ")
BLOCK_STRUCT = struct.Struct("<4sHHQQQQQ")
assert GLOBAL_STRUCT.size == GLOBAL_HEADER_SIZE, GLOBAL_STRUCT.size
assert BLOCK_STRUCT.size == BLOCK_HEADER_SIZE, BLOCK_STRUCT.size

_U64_MAX = (1 << 64) - 1


class FormatError(ValueError):
    """The byte stream does not satisfy the PRMGAP01 v1 format contract."""


@dataclass(frozen=True)
class GlobalHeader:
    version: int
    flags: int
    chunk_odd: int
    reserved: int


@dataclass
class Block:
    index: int
    offset: int
    version: int
    flags: int
    prev_prime: int
    first_prime: int
    end_prime: int
    count: int
    nbytes: int
    payload: bytes | None = None

    @property
    def payload_offset(self) -> int:
        return self.offset + BLOCK_HEADER_SIZE


# ---------------------------------------------------------------------------
# Low-level reading
# ---------------------------------------------------------------------------

def read_global_header(fp) -> GlobalHeader:
    """Read and validate the 32-byte global header; leaves fp at offset 32."""
    raw = fp.read(GLOBAL_HEADER_SIZE)
    if len(raw) != GLOBAL_HEADER_SIZE:
        raise FormatError(
            "file is shorter than the %d-byte global header (%d bytes available)"
            % (GLOBAL_HEADER_SIZE, len(raw))
        )
    magic, version, flags, chunk_odd, reserved = GLOBAL_STRUCT.unpack(raw)
    if magic != GLOBAL_MAGIC:
        raise FormatError("bad global header magic %r (expected %r)" % (magic, GLOBAL_MAGIC))
    if version != FORMAT_VERSION:
        raise FormatError("unsupported global format version %d (expected %d)" % (version, FORMAT_VERSION))
    if flags != 0:
        raise FormatError("unsupported global flags %#x (v1 requires 0)" % (flags,))
    return GlobalHeader(version=version, flags=flags, chunk_odd=chunk_odd, reserved=reserved)


def _check_block_header(block: Block, prev_end: int, strict: bool) -> None:
    """Validate one block header against the format contract."""
    idx = block.index
    if block.version != FORMAT_VERSION:
        raise FormatError("block %d: version %d != %d" % (idx, block.version, FORMAT_VERSION))
    if block.flags != 0:
        raise FormatError("block %d: flags %#x != 0" % (idx, block.flags))
    if block.count < 1:
        raise FormatError("block %d: count must be >= 1" % idx)
    if block.count == 1:
        if block.nbytes != 0:
            raise FormatError("block %d: count==1 requires nbytes==0 (got %d)" % (idx, block.nbytes))
        if block.end_prime != block.first_prime:
            raise FormatError("block %d: count==1 requires first_prime==end_prime" % idx)
        if block.first_prime < 2:
            raise FormatError("block %d: first_prime %d < 2" % (idx, block.first_prime))
    else:
        # A varint is 1..10 bytes, so count-1 <= nbytes <= 10*(count-1).
        if block.nbytes < block.count - 1:
            raise FormatError(
                "block %d: nbytes %d too small for %d gap varints" % (idx, block.nbytes, block.count - 1)
            )
        if block.nbytes > 10 * (block.count - 1):
            raise FormatError(
                "block %d: nbytes %d too large for %d gap varints" % (idx, block.nbytes, block.count - 1)
            )
        if block.end_prime <= block.first_prime:
            raise FormatError("block %d: end_prime <= first_prime" % idx)
        if block.first_prime < 2:
            raise FormatError("block %d: first_prime %d < 2" % (idx, block.first_prime))

    if idx == 0:
        if block.prev_prime != 0:
            raise FormatError("block 0: prev_prime must be 0 (got %d)" % block.prev_prime)
        if block.first_prime != 2:
            raise FormatError("block 0: first_prime must be 2 (got %d)" % block.first_prime)
        if strict and (block.count != 1 or block.end_prime != 2 or block.nbytes != 0):
            raise FormatError(
                "block 0: v1 spec requires prev=0, first=end=2, count=1, nbytes=0 "
                "(got first=%d end=%d count=%d nbytes=%d)"
                % (block.first_prime, block.end_prime, block.count, block.nbytes)
            )
    else:
        if block.prev_prime != prev_end:
            raise FormatError(
                "block %d: prev_prime %d != previous block end_prime %d"
                % (idx, block.prev_prime, prev_end)
            )
        if block.first_prime <= prev_end:
            raise FormatError(
                "block %d: first_prime %d <= prev_prime %d" % (idx, block.first_prime, prev_end)
            )
        cross_gap = block.first_prime - prev_end
        if prev_end == 2:
            if strict and cross_gap != 1:
                raise FormatError(
                    "block %d: first prime after 2 must be 3 (cross-block gap %d)" % (idx, cross_gap)
                )
        elif prev_end > 2:
            if cross_gap < 2 or (cross_gap & 1):
                raise FormatError(
                    "block %d: cross-block gap %d must be positive and even" % (idx, cross_gap)
                )


def _note_incomplete(offset, file_size, reason, allow_incomplete_tail, tail_sink):
    message = "incomplete block at offset %d (%s; file size %d)" % (offset, reason, file_size)
    if not allow_incomplete_tail:
        raise FormatError(message + "; pass allow_incomplete_tail=True to read a prefix snapshot")
    if tail_sink is not None:
        tail_sink.clear()
        tail_sink.update({"offset": offset, "tail_bytes": file_size - offset, "reason": reason})
    return


def iter_blocks(fp, *, decode_payload=True, strict=True, allow_incomplete_tail=False, tail_sink=None):
    """Yield Block objects in order, validating headers as we stream.

    decode_payload=False skips reading payload bytes (headers are still
    validated); this is the fast mode used for pi(x) tables over whole files.
    allow_incomplete_tail=True tolerates a file that ends in the middle of a
    block (append-only writer still running / crash); the partial tail is
    reported through tail_sink.
    """
    file_size = os.fstat(fp.fileno()).st_size
    fp.seek(0)
    read_global_header(fp)
    prev_end = 0
    index = 0
    pos = GLOBAL_HEADER_SIZE
    while pos < file_size:
        if file_size - pos < BLOCK_HEADER_SIZE:
            _note_incomplete(pos, file_size, "trailing bytes shorter than a block header",
                             allow_incomplete_tail, tail_sink)
            return
        fp.seek(pos)
        raw = fp.read(BLOCK_HEADER_SIZE)
        if len(raw) != BLOCK_HEADER_SIZE:
            _note_incomplete(pos, file_size, "short read of block header",
                             allow_incomplete_tail, tail_sink)
            return
        (magic, version, flags, prev_prime, first_prime,
         end_prime, count, nbytes) = BLOCK_STRUCT.unpack(raw)
        if magic != BLOCK_MAGIC:
            raise FormatError("block %d at offset %d: bad magic %r" % (index, pos, magic))
        if prev_prime > _U64_MAX or first_prime > _U64_MAX:  # pragma: no cover - struct can't exceed
            raise FormatError("block %d: prime field overflow" % index)
        block = Block(index=index, offset=pos, version=version, flags=flags,
                      prev_prime=prev_prime, first_prime=first_prime,
                      end_prime=end_prime, count=count, nbytes=nbytes)
        _check_block_header(block, prev_end, strict)
        payload_end = pos + BLOCK_HEADER_SIZE + nbytes
        if payload_end > file_size:
            _note_incomplete(pos, file_size, "block payload of %d bytes exceeds file" % nbytes,
                             allow_incomplete_tail, tail_sink)
            return
        if decode_payload:
            fp.seek(pos + BLOCK_HEADER_SIZE)
            payload = fp.read(nbytes)
            if len(payload) != nbytes:
                raise FormatError("block %d: short read of payload" % index)
            block.payload = payload
        yield block
        prev_end = end_prime
        pos = payload_end
        index += 1


def load_block_payload(fp, block: Block) -> bytes:
    """Read a block payload on demand (block was yielded with decode_payload=False)."""
    if block.payload is None:
        fp.seek(block.payload_offset)
        payload = fp.read(block.nbytes)
        if len(payload) != block.nbytes:
            raise FormatError("block %d: short read of payload" % block.index)
        block.payload = payload
    return block.payload


# ---------------------------------------------------------------------------
# Payload decoding
# ---------------------------------------------------------------------------

def decode_uleb128(buf, pos: int):
    """Decode one unsigned LEB128 value; returns (value, new_position)."""
    value = 0
    shift = 0
    size = len(buf)
    while pos < size:
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            return value, pos
        shift += 7
        if shift >= 64:
            raise FormatError("LEB128 varint overflows 64 bits")
    raise FormatError("truncated LEB128 varint at end of payload")


def iter_block_primes(block: Block, *, validate=True):
    """Yield the primes stored in one block, validating the payload when it is
    consumed to exhaustion (count, payload length and end_prime checks)."""
    p = block.first_prime
    yield p
    if block.count == 1:
        if validate:
            if block.nbytes != 0 or (block.payload is not None and len(block.payload) != 0):
                raise FormatError("block %d: count==1 block carries payload bytes" % block.index)
            if block.end_prime != block.first_prime:
                raise FormatError("block %d: count==1 requires first==end" % block.index)
        return
    if block.payload is None:
        raise ValueError("block %d: payload not loaded" % block.index)
    buf = block.payload
    size = len(buf)
    if size != block.nbytes:
        raise FormatError("block %d: payload length %d != nbytes %d" % (block.index, size, block.nbytes))
    pos = 0
    seen = 1
    total_gap = 0
    while pos < size:
        gap, pos = decode_uleb128(buf, pos)
        if gap <= 0:
            raise FormatError("block %d: non-positive gap %d" % (block.index, gap))
        if p > 2 and (gap & 1):
            raise FormatError("block %d: odd gap %d after odd prime %d" % (block.index, gap, p))
        p += gap
        total_gap += gap
        seen += 1
        if seen > block.count:
            raise FormatError(
                "block %d: more gaps than count-1=%d" % (block.index, block.count - 1)
            )
        yield p
    if validate:
        if seen != block.count:
            raise FormatError(
                "block %d: decoded %d primes but header count=%d" % (block.index, seen, block.count)
            )
        expected_end = block.first_prime + total_gap
        if expected_end != block.end_prime:
            raise FormatError(
                "block %d: first_prime + sum(gaps) = %d != end_prime %d"
                % (block.index, expected_end, block.end_prime)
            )


def iter_primes(fp, *, strict=True, allow_incomplete_tail=False, tail_sink=None):
    """Yield every saved prime in increasing order."""
    for block in iter_blocks(fp, decode_payload=True, strict=strict,
                              allow_incomplete_tail=allow_incomplete_tail,
                              tail_sink=tail_sink):
        yield from iter_block_primes(block)


# ---------------------------------------------------------------------------
# Dataset-wide helpers
# ---------------------------------------------------------------------------

def dataset_summary(path, *, strict=True, allow_incomplete_tail=True):
    """Header-only pass: block count, total prime count, saved bound, sizes."""
    file_size = os.path.getsize(path)
    tail = {}
    block_count = 0
    total = 0
    first_prime = None
    last_prime = None
    payload_bytes = 0
    count_min = None
    count_max = 0
    with open(path, "rb") as fp:
        global_header = read_global_header(fp)
        for block in iter_blocks(fp, decode_payload=False, strict=strict,
                                 allow_incomplete_tail=allow_incomplete_tail, tail_sink=tail):
            block_count += 1
            total += block.count
            payload_bytes += block.nbytes
            if first_prime is None:
                first_prime = block.first_prime
            last_prime = block.end_prime
            count_min = block.count if count_min is None else min(count_min, block.count)
            count_max = max(count_max, block.count)
    expected_size = GLOBAL_HEADER_SIZE + block_count * BLOCK_HEADER_SIZE + payload_bytes
    return {
        "path": path,
        "format": "PRMGAP01/v1",
        "version": global_header.version,
        "chunk_odd": global_header.chunk_odd,
        "file_size": file_size,
        "block_count": block_count,
        "total_primes": total,
        "first_prime": first_prime,
        "last_prime": last_prime,
        "payload_bytes": payload_bytes,
        "header_bytes": GLOBAL_HEADER_SIZE + block_count * BLOCK_HEADER_SIZE,
        "expected_file_size": expected_size,
        "complete": expected_size == file_size,
        "incomplete_tail": dict(tail) if tail else None,
        "block_count_min": count_min,
        "block_count_max": count_max,
    }


def compute_pi_table(path, targets, *, strict=True, allow_incomplete_tail=True):
    """Compute pi(t) = number of saved primes <= t for each target.

    Whole blocks are accounted for from their 48-byte header 'count' field
    (headers are still fully validated as we stream); only the one block that
    straddles a target needs its payload decoded.

    Returns {"targets": {t: {"pi": int|None, "exact": bool}}, ...}.
    exact=False means t is beyond the last prime saved in this snapshot, so
    pi(t) cannot be read from the file (a later, longer snapshot may know it).
    """
    wanted = sorted({int(t) for t in targets})
    results = {}
    for t in wanted:
        results[t] = {"pi": None, "exact": False}
    for t in wanted:
        if t < 2:
            results[t] = {"pi": 0, "exact": True}
    pending = [t for t in wanted if t >= 2]
    if not pending:
        return {"targets": results, "last_prime": None, "total_primes": 0, "incomplete_tail": None}
    tail = {}
    total = 0
    last_prime = None
    with open(path, "rb") as fp:
        for block in iter_blocks(fp, decode_payload=False, strict=strict,
                                 allow_incomplete_tail=allow_incomplete_tail, tail_sink=tail):
            last_prime = block.end_prime
            # Targets below this block's first prime are fully covered already.
            while pending and pending[0] < block.first_prime:
                t = pending.pop(0)
                results[t] = {"pi": total, "exact": True}
            if pending and pending[0] <= block.end_prime:
                load_block_payload(fp, block)
                boundary = []
                while pending and pending[0] <= block.end_prime:
                    boundary.append(pending.pop(0))
                count_here = 0
                gen = iter_block_primes(block)
                current = next(gen)
                for t in boundary:
                    while current is not None and current <= t:
                        count_here += 1
                        try:
                            current = next(gen)
                        except StopIteration:
                            current = None
                    results[t] = {"pi": total + count_here, "exact": True}
                    if current is None:
                        break
            total += block.count
    for t in pending:
        results[t] = {"pi": None, "exact": False}
    return {
        "targets": results,
        "last_prime": last_prime,
        "total_primes": total,
        "incomplete_tail": dict(tail) if tail else None,
    }


def pi_value(path, x, *, strict=True, allow_incomplete_tail=True):
    """Convenience single-target wrapper around compute_pi_table."""
    return compute_pi_table(path, [x], strict=strict,
                            allow_incomplete_tail=allow_incomplete_tail)["targets"][int(x)]


def scan_prefix_stats(path, max_prime, *, strict=True, allow_incomplete_tail=True,
                      top_gaps=10):
    """Decode all primes <= max_prime and compute gap/residue statistics.

    Gap statistics are within-block only (cross-block gaps are not stored in
    the format); the number of omitted gaps is reported.
    """
    max_prime = int(max_prime)
    stats = {
        "target_max": max_prime,
        "primes_counted": 0,
        "first_prime_counted": None,
        "last_prime_counted": None,
        "blocks_scanned": 0,
        "gaps_counted": 0,
        "gap_sum": 0,
        "mean_gap": None,
        "max_gap": None,
        "max_gap_start": None,
        "max_gap_end": None,
        "twin_pairs": 0,
        "residue_mod3": [0, 0, 0],
        "residue_mod4": [0, 0, 0, 0],
        "top_gaps": [],
        "cross_block_gaps_included": 0,
        "cross_block_gaps_omitted": 0,
        "target_covered": False,
        "file_last_prime": None,
        "incomplete_tail": None,
    }
    if max_prime < 2:
        return stats

    tail = {}
    heap = []
    blocks_scanned = 0
    primes_counted = 0
    gaps = 0
    gap_sum = 0
    max_gap = 0
    max_gap_start = None
    max_gap_end = None
    twins = 0
    residue_mod3 = [0, 0, 0]
    residue_mod4 = [0, 0, 0, 0]
    first_counted = None
    last_counted = None
    covered = False
    file_last_prime = None

    previous_block_end = None
    cross_gaps_included = 0
    with open(path, "rb") as fp:
        for block in iter_blocks(fp, decode_payload=True, strict=strict,
                                 allow_incomplete_tail=allow_incomplete_tail, tail_sink=tail):
            file_last_prime = block.end_prime
            if block.first_prime > max_prime:
                covered = True
                break
            blocks_scanned += 1
            # The gap straddling the boundary with the previous block is not
            # stored in any payload, but both of its endpoints are in headers,
            # so it can be recovered exactly (and is part of the prime sequence).
            if previous_block_end is not None:
                cross_gap = block.first_prime - previous_block_end
                gaps += 1
                cross_gaps_included += 1
                gap_sum += cross_gap
                if cross_gap == 2:
                    twins += 1
                if cross_gap > max_gap:
                    max_gap = cross_gap
                    max_gap_start = previous_block_end
                    max_gap_end = block.first_prime
                if top_gaps:
                    item = (cross_gap, previous_block_end, block.first_prime)
                    if len(heap) < top_gaps:
                        heapq.heappush(heap, item)
                    elif item > heap[0]:
                        heapq.heapreplace(heap, item)
            prev_in_block = None
            for p in iter_block_primes(block):
                if p <= max_prime:
                    if first_counted is None:
                        first_counted = p
                    primes_counted += 1
                    last_counted = p
                    residue_mod3[p % 3] += 1
                    residue_mod4[p % 4] += 1
                    if prev_in_block is not None:
                        gap = p - prev_in_block
                        gaps += 1
                        gap_sum += gap
                        if gap == 2:
                            twins += 1
                        if gap > max_gap:
                            max_gap = gap
                            max_gap_start = prev_in_block
                            max_gap_end = p
                        if top_gaps:
                            item = (gap, prev_in_block, p)
                            if len(heap) < top_gaps:
                                heapq.heappush(heap, item)
                            elif item > heap[0]:
                                heapq.heapreplace(heap, item)
                    prev_in_block = p
                # primes beyond max_prime: keep draining for payload validation
            previous_block_end = block.end_prime
            if block.end_prime > max_prime:
                covered = True
                break

    if file_last_prime is not None and file_last_prime >= max_prime:
        covered = True
    stats.update({
        "primes_counted": primes_counted,
        "first_prime_counted": first_counted,
        "last_prime_counted": last_counted,
        "blocks_scanned": blocks_scanned,
        "gaps_counted": gaps,
        "gap_sum": gap_sum,
        "mean_gap": (gap_sum / gaps) if gaps else None,
        "max_gap": max_gap if gaps else None,
        "max_gap_start": max_gap_start,
        "max_gap_end": max_gap_end,
        "twin_pairs": twins,
        "residue_mod3": residue_mod3,
        "residue_mod4": residue_mod4,
        "top_gaps": [{"gap": g, "start": s, "end": e} for g, s, e in sorted(heap, reverse=True)],
        "cross_block_gaps_included": cross_gaps_included,
        "cross_block_gaps_omitted": 0,
        "target_covered": covered,
        "file_last_prime": file_last_prime,
        "incomplete_tail": dict(tail) if tail else None,
    })
    return stats


def verify_full(path, *, strict=True, allow_incomplete_tail=True):
    """Decode every block payload and validate the whole file.  Returns a summary.

    This is intentionally the slow path; it is used by self-tests and can be
    used as an extra cross-check on a real snapshot.
    """
    tail = {}
    block_count = 0
    total = 0
    first_prime = None
    last_prime = None
    gaps = 0
    gap_sum = 0
    with open(path, "rb") as fp:
        for block in iter_blocks(fp, decode_payload=True, strict=strict,
                                 allow_incomplete_tail=allow_incomplete_tail, tail_sink=tail):
            prev = None
            count_here = 0
            for p in iter_block_primes(block):
                count_here += 1
                if prev is not None:
                    gaps += 1
                    gap_sum += p - prev
                prev = p
            if count_here != block.count:  # pragma: no cover - generator validates
                raise FormatError("block %d: decoded count mismatch" % block.index)
            block_count += 1
            total += block.count
            if first_prime is None:
                first_prime = block.first_prime
            last_prime = block.end_prime
    return {
        "path": path,
        "block_count": block_count,
        "total_primes": total,
        "first_prime": first_prime,
        "last_prime": last_prime,
        "gaps_within_blocks": gaps,
        "gap_sum": gap_sum,
        "complete": not tail,
        "incomplete_tail": dict(tail) if tail else None,
    }


def write_first_primes(dataset, out_path, n, *, strict=True, allow_incomplete_tail=True):
    """Write the first n primes of the dataset as decimal text, one per line."""
    if n <= 0:
        raise ValueError("n must be positive")
    written = 0
    last_prime = None
    with open(dataset, "rb") as fp, open(out_path, "w", encoding="ascii", newline="\n") as out:
        chunk = []
        for p in iter_primes(fp, strict=strict, allow_incomplete_tail=allow_incomplete_tail):
            chunk.append(str(p))
            written += 1
            last_prime = p
            if len(chunk) >= 100_000:
                out.write("\n".join(chunk) + "\n")
                chunk.clear()
            if written >= n:
                break
        if chunk:
            out.write("\n".join(chunk) + "\n")
    if written != n:
        raise FormatError(
            "dataset contains only %d primes but %d were requested" % (written, n)
        )
    return {"written": written, "last_prime": last_prime, "path": out_path}


def verify_first_primes_file(path, n, expect_last=None):
    """Re-read a generated decimal prime list and check count/order/last line."""
    lines = 0
    first = None
    last = None
    prev = None
    with open(path, "r", encoding="ascii") as fp:
        for line in fp:
            text = line.strip()
            if not text:
                raise FormatError("blank line at line %d" % (lines + 1))
            value = int(text)
            if first is None:
                first = value
            if prev is not None and value <= prev:
                raise FormatError("primes not increasing at line %d" % (lines + 1))
            prev = value
            last = value
            lines += 1
    ok = lines == n and (expect_last is None or last == expect_last)
    return {
        "path": path,
        "lines": lines,
        "expected_lines": n,
        "first": first,
        "last": last,
        "expected_last": expect_last,
        "ok": ok,
    }


def sha256_file(path, chunk_size=1 << 20):
    digest = hashlib.sha256()
    with open(path, "rb") as fp:
        while True:
            chunk = fp.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


# ---------------------------------------------------------------------------
# Minimal CLI (handy for debugging the real dataset)
# ---------------------------------------------------------------------------

def _parse_targets(text):
    targets = []
    for part in text.replace(" ", "").split(","):
        if not part:
            continue
        if part.lower().startswith("1e"):
            targets.append(10 ** int(part[2:]))
        else:
            targets.append(int(part))
    return targets


def _cmd_info(args):
    print(json.dumps(dataset_summary(args.dataset, strict=not args.lenient,
                                     allow_incomplete_tail=args.lenient), indent=2))


def _cmd_pi(args):
    result = compute_pi_table(args.dataset, _parse_targets(args.targets),
                              strict=not args.lenient, allow_incomplete_tail=args.lenient)
    print(json.dumps(result, indent=2))


def _cmd_stats(args):
    print(json.dumps(scan_prefix_stats(args.dataset, int(float(args.max)) if "." in args.max or "e" in args.max.lower() else int(args.max),
                                       strict=not args.lenient,
                                       allow_incomplete_tail=args.lenient,
                                       top_gaps=args.top_gaps), indent=2))


def _cmd_verify(args):
    print(json.dumps(verify_full(args.dataset, strict=not args.lenient,
                                 allow_incomplete_tail=args.lenient), indent=2))


def _cmd_extract(args):
    with open(args.dataset, "rb") as fp:
        for i, p in enumerate(iter_primes(fp, strict=not args.lenient,
                                          allow_incomplete_tail=args.lenient)):
            if i >= args.n:
                break
            print(p)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Independent PRMGAP01 v1 dataset decoder")
    parser.add_argument("--lenient", action="store_true",
                        help="tolerate an incomplete trailing block (snapshot being written)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("info", help="header-only dataset summary as JSON")
    p.add_argument("dataset")
    p.set_defaults(func=_cmd_info)

    p = sub.add_parser("pi", help="pi(x) at targets, e.g. --targets 1e3,1e4,1e5")
    p.add_argument("dataset")
    p.add_argument("targets")
    p.set_defaults(func=_cmd_pi)

    p = sub.add_parser("stats", help="gap/residue statistics for a prefix")
    p.add_argument("dataset")
    p.add_argument("max")
    p.add_argument("--top-gaps", type=int, default=10)
    p.set_defaults(func=_cmd_stats)

    p = sub.add_parser("verify", help="full payload decode/validation (slow)")
    p.add_argument("dataset")
    p.set_defaults(func=_cmd_verify)

    p = sub.add_parser("extract", help="print the first N primes")
    p.add_argument("dataset")
    p.add_argument("n", type=int)
    p.set_defaults(func=_cmd_extract)

    args = parser.parse_args(argv)
    try:
        args.func(args)
    except FormatError as exc:
        print("FORMAT ERROR: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
