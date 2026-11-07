#!/usr/bin/env python3
"""Generate synthetic PRMGAP01 v1 datasets with an independent Python encoder.

Used only for self-tests of the verifier; writes files under verify/synth/.
Also can inject a single fault to produce a file the verifier MUST reject.
"""
import argparse
import hashlib
import json
import struct
import sys

GLOBAL = struct.Struct('<8sIIQQ')
BLKHDR = struct.Struct('<4sHHQQQQQ')   # magic, version, flags, prev, first, end, count, nbytes
GLOBAL_SIZE = 32
BLOCK_HDR_SIZE = 48

# fault name -> (expected verifier exit code, description)
FAULTS = {
    'global_magic':      (1, 'global magic corrupted'),
    'global_version':    (1, 'global version=2'),
    'global_flags':      (1, 'global flags=1'),
    'global_reserved':   (1, 'global reserved!=0'),
    'first_not2':        (1, 'first block does not store prime 2'),
    'block_magic':       (1, 'block 1 magic corrupted'),
    'block_version':     (1, 'block 1 version=2'),
    'block_flags':       (1, 'block 1 flags=1'),
    'prev_link':         (1, 'block 1 prev_prime breaks the chain'),
    'end_mismatch':      (1, 'block 1 end_prime != last decoded prime'),
    'count_mismatch':    (1, 'block 1 count != decoded count'),
    'nbytes_bad':        (1, 'block 1 nbytes has one extra payload byte'),
    'gap_odd':           (1, 'a decoded gap is odd (1)'),
    'gap_zero':          (1, 'a decoded gap is 0'),
    'trailing_garbage':  (1, '60 bytes of garbage after the last block'),
    'truncate_payload':  (3, 'file truncated by 1 byte (incomplete payload)'),
    'truncate_header':   (3, 'file truncated mid block-header'),
    'nbytes_overrun':    (3, 'last block nbytes extends past EOF'),
}


def sieve(limit):
    if limit < 2:
        return []
    bs = bytearray(b'\x01') * (limit + 1)
    bs[0:2] = b'\x00\x00'
    for i in range(2, int(limit ** 0.5) + 1):
        if bs[i]:
            bs[i * i::i] = bytearray(len(range(i * i, limit + 1, i)))
    return [i for i in range(2, limit + 1) if bs[i]]


def leb128(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def encode(primes, block_sizes, chunk_odd=0):
    assert primes and primes[0] == 2
    buf = bytearray(GLOBAL.pack(b'PRMGAP01', 1, 0, chunk_odd, 0))
    blocks = []
    prev = 0
    # block 0 must be the prime-2 singleton
    groups = [[2]]
    rest = primes[1:]
    i = 0
    si = 0
    while i < len(rest):
        n = block_sizes[si % len(block_sizes)]
        groups.append(rest[i:i + n])
        i += n
        si += 1
    for ps in groups:
        off = len(buf)
        first, end = ps[0], ps[-1]
        payload = bytearray()
        for a, b in zip(ps, ps[1:]):
            payload += leb128(b - a)
        buf += BLKHDR.pack(b'PRMB', 1, 0, prev, first, end, len(ps), len(payload))
        blocks.append({'off': off, 'payload_off': off + BLOCK_HDR_SIZE,
                       'first_gap_off': off + BLOCK_HDR_SIZE,
                       'prev': prev, 'first': first, 'end': end,
                       'count': len(ps), 'nbytes': len(payload)})
        buf += payload
        prev = end
    return bytes(buf), blocks


def apply_fault(buf, blocks, fault):
    b = bytearray(buf)
    if fault == 'global_magic':
        b[0] ^= 0xFF
    elif fault == 'global_version':
        struct.pack_into('<I', b, 8, 2)
    elif fault == 'global_flags':
        struct.pack_into('<I', b, 12, 1)
    elif fault == 'global_reserved':
        struct.pack_into('<Q', b, 24, 1)
    elif fault == 'first_not2':
        struct.pack_into('<Q', b, blocks[0]['off'] + 16, 3)
        struct.pack_into('<Q', b, blocks[0]['off'] + 24, 3)
    elif fault == 'block_magic':
        b[blocks[1]['off']] ^= 0xFF
    elif fault == 'block_version':
        struct.pack_into('<H', b, blocks[1]['off'] + 4, 2)
    elif fault == 'block_flags':
        struct.pack_into('<H', b, blocks[1]['off'] + 6, 1)
    elif fault == 'prev_link':
        struct.pack_into('<Q', b, blocks[1]['off'] + 8, blocks[1]['prev'] + 2)
    elif fault == 'end_mismatch':
        struct.pack_into('<Q', b, blocks[1]['off'] + 24, blocks[1]['end'] + 2)
    elif fault == 'count_mismatch':
        struct.pack_into('<Q', b, blocks[1]['off'] + 32, blocks[1]['count'] + 1)
    elif fault == 'nbytes_bad':
        struct.pack_into('<Q', b, blocks[1]['off'] + 40, blocks[1]['nbytes'] + 1)
    elif fault == 'gap_odd':
        b[blocks[1]['first_gap_off']] = 0x01
    elif fault == 'gap_zero':
        b[blocks[1]['first_gap_off']] = 0x00
    elif fault == 'trailing_garbage':
        b += b'\xAA' * 60
    elif fault == 'truncate_payload':
        b = b[:-1]
    elif fault == 'truncate_header':
        b = b[:blocks[-1]['off'] + 24]
    elif fault == 'nbytes_overrun':
        # plausible varint payload size for the block, but past end of file
        struct.pack_into('<Q', b, blocks[-1]['off'] + 40, blocks[-1]['nbytes'] + 5000)
    else:
        raise SystemExit('unknown fault ' + fault)
    return bytes(b)


def expectations(primes, blocks, data, checkpoints):
    pi = {}
    for x in checkpoints:
        lo, hi = 0, len(primes)
        while lo < hi:
            mid = (lo + hi) // 2
            if primes[mid] <= x:
                lo = mid + 1
            else:
                hi = mid
        pi[str(x)] = lo
    max_gap = 0
    max_at = None
    for a, b in zip(primes, primes[1:]):
        if b - a > max_gap:
            max_gap = b - a
            max_at = [a, b]
    twin_total = sum(1 for a, b in zip(primes, primes[1:]) if b - a == 2)
    return {
        'limit': primes[-1],
        'prime_count': len(primes),
        'last_prime': primes[-1],
        'first_prime': primes[0],
        'nblocks': len(blocks),
        'file_size': len(data),
        'sha256': hashlib.sha256(data).hexdigest(),
        'pi': pi,
        'max_gap': max_gap,
        'max_gap_between': max_at,
        'twin_total': twin_total,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--limit', type=int, required=True)
    ap.add_argument('--block', type=int, default=1000)
    ap.add_argument('--vary', action='store_true')
    ap.add_argument('--out', required=True)
    ap.add_argument('--expect-json')
    ap.add_argument('--write-primes')
    ap.add_argument('--checkpoints', default='10,100,1000,10000,100000,1000000')
    ap.add_argument('--fault')
    ap.add_argument('--chunk-odd', type=int, default=0)
    args = ap.parse_args()

    primes = sieve(args.limit)
    if not primes:
        raise SystemExit('limit too small')
    sizes = [args.block]
    if args.vary:
        sizes = [1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987]
    data, blocks = encode(primes, sizes, args.chunk_odd)
    cps = [int(x) for x in args.checkpoints.split(',') if x]
    ex = expectations(primes, blocks, data, cps)

    if args.fault:
        data = apply_fault(data, blocks, args.fault)
        ex['fault'] = args.fault
        ex['expected_exit'] = FAULTS[args.fault][0]

    with open(args.out, 'wb') as f:
        f.write(data)
    if args.write_primes:
        with open(args.write_primes, 'w') as f:
            for p in primes:
                f.write('%d\n' % p)
    if args.expect_json:
        with open(args.expect_json, 'w') as f:
            json.dump(ex, f, indent=1, sort_keys=True)
    print(json.dumps({'out': args.out, 'bytes': len(data), 'primes': len(primes),
                      'blocks': len(blocks), 'sha256': ex['sha256']}))


if __name__ == '__main__':
    main()
