#!/usr/bin/env python3
"""Independent pure-Python reference decoder/validator for PRMGAP01 v1.

Second implementation (different language/author pathway from verify_primes.c),
used for cross-checks on small synthetic datasets.
exit: 0 PASS, 1 FAIL, 3 incomplete trailing block
"""
import argparse
import hashlib
import struct
import sys

GLOBAL = struct.Struct('<8sIIQQ')
BLKHDR = struct.Struct('<4sHHQQQQQ')
GLOBAL_SIZE, BLOCK_HDR_SIZE = 32, 48
BASES = (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37)


def is_prime(n):
    if n < 2:
        return False
    for b in BASES:
        if n == b:
            return True
        if n % b == 0:
            return False
    d, s = n - 1, 0
    while d % 2 == 0:
        d //= 2
        s += 1
    for a in BASES:
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def leb128(data, pos):
    v, shift = 0, 0
    while True:
        if pos >= len(data):
            raise ValueError('payload ended inside a varint')
        b = data[pos]
        pos += 1
        if shift >= 64:
            raise ValueError('varint overflow')
        v |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            return v, pos


def decode(path, strict=True):
    """Returns (primes, info). Raises ValueError on corruption."""
    data = open(path, 'rb').read()
    info = {'bytes': len(data), 'blocks': 0}
    if len(data) < GLOBAL_SIZE:
        raise ValueError('file smaller than global header')
    magic, ver, flags, chunk_odd, reserved = GLOBAL.unpack_from(data, 0)
    if magic != b'PRMGAP01' or ver != 1 or flags != 0 or reserved != 0:
        raise ValueError('bad global header')
    info['chunk_odd'] = chunk_odd
    info['sha256'] = hashlib.sha256(data).hexdigest()
    primes = []
    off = GLOBAL_SIZE
    blocks = 0
    last_end = 0
    while off < len(data):
        if len(data) - off < BLOCK_HDR_SIZE:
            raise EOFError('incomplete trailing block header')
        (bmagic, bver, bflags, prev, first, end, count, nbytes) = BLKHDR.unpack_from(data, off)
        if bmagic != b'PRMB' or bver != 1 or bflags != 0:
            raise ValueError('bad block header at %d' % off)
        if blocks == 0:
            if (prev, first, end, count, nbytes) != (0, 2, 2, 1, 0):
                raise ValueError('first block is not the prime-2 singleton')
        else:
            if prev != last_end:
                raise ValueError('prev_prime linkage broken at block %d' % blocks)
            if count == 0 or first <= prev:
                raise ValueError('bad block %d first/count' % blocks)
            bg = first - prev
            if prev == 2:
                if bg != 1:
                    raise ValueError('block after 2 must start at 3')
            elif bg < 2 or bg % 2:
                raise ValueError('bad cross-block gap %d' % bg)
        if end < first or (count == 1 and end != first) or (count > 1 and end <= first):
            raise ValueError('bad first/end/count in block %d' % blocks)
        if nbytes > len(data) - off - BLOCK_HDR_SIZE:
            raise EOFError('block %d payload extends past EOF' % blocks)
        if nbytes < count - 1 or nbytes > 10 * (count - 1):
            raise ValueError('implausible nbytes in block %d' % blocks)
        payload = data[off + BLOCK_HDR_SIZE: off + BLOCK_HDR_SIZE + nbytes]
        p = first
        if len(primes) and first <= primes[-1]:
            raise ValueError('block first not increasing')
        primes.append(p)
        pos = 0
        for _ in range(count - 1):
            g, pos = leb128(payload, pos)
            if g < 2 or g % 2:
                raise ValueError('gap %d is not an even number >= 2' % g)
            p += g
            primes.append(p)
        if pos != nbytes:
            raise ValueError('payload not exactly consumed in block %d' % blocks)
        if p != end:
            raise ValueError('block %d end mismatch: %d != %d' % (blocks, p, end))
        last_end = end
        blocks += 1
        off += BLOCK_HDR_SIZE + nbytes
    if off != len(data):
        raise ValueError('trailing bytes')
    info['blocks'] = blocks
    info['total'] = len(primes)
    info['last_prime'] = primes[-1] if primes else 0
    info['first_prime'] = primes[0] if primes else 0
    return primes, info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dataset')
    ap.add_argument('--dump-primes')
    ap.add_argument('--expect-json')
    ap.add_argument('--samples', type=int, default=0)
    ap.add_argument('--gaps', type=int, default=0)
    ap.add_argument('--mr-selftest', type=int, default=0)
    args = ap.parse_args()

    if args.mr_selftest:
        lim = args.mr_selftest
        for n in range(2, lim + 1):
            t = n >= 2 and all(n % d for d in range(2, int(n ** 0.5) + 1))
            if t != is_prime(n):
                print('REF_MR_SELFTEST_FAIL n=%d' % n)
                return 1
        print('REF_MR_SELFTEST    : n=2..%d all match trial division: PASS' % lim)
        return 0

    if not args.dataset:
        print('usage: ref_verify.py --dataset FILE [--expect-json J] [--dump-primes O]')
        return 2
    try:
        primes, info = decode(args.dataset)
    except EOFError as e:
        print('RESULT             : INCOMPLETE —', e)
        return 3
    except ValueError as e:
        print('RESULT             : FAIL —', e)
        return 1
    print('REF_DECODER        : blocks=%d primes=%d last=%d sha256=%s'
          % (info['blocks'], info['total'], info['last_prime'], info['sha256']))
    fail = 0
    if args.expect_json:
        import json
        ex = json.load(open(args.expect_json))
        checks = [
            ('prime_count', info['total']),
            ('last_prime', info['last_prime']),
            ('first_prime', info['first_prime']),
            ('nblocks', info['blocks']),
            ('file_size', info['bytes']),
            ('sha256', info['sha256']),
        ]
        for k, got in checks:
            if ex.get(k) != got:
                print('REF_CHECK_FAIL     : %s got %r want %r' % (k, got, ex.get(k)))
                fail = 1
        for x, want in ex.get('pi', {}).items():
            got = sum(1 for p in primes if p <= int(x))
            if got != want:
                print('REF_PI_FAIL        : pi(%s) got %d want %d' % (x, got, want))
                fail = 1
        mg = 0
        for a, b in zip(primes, primes[1:]):
            mg = max(mg, b - a)
        if ex.get('max_gap') != mg:
            print('REF_MAXGAP_FAIL    : got %d want %r' % (mg, ex.get('max_gap')))
            fail = 1
        if 'fault' not in ex:
            print('REF_EXPECT_JSON    : all dataset expectations match')
    if args.samples and primes:
        idxs = [int(i * len(primes) / args.samples) for i in range(args.samples)]
        idxs = sorted(set(idxs))
        bad = sum(0 if is_prime(primes[i]) else 1 for i in idxs)
        print('REF_MR_SAMPLES     : %d sampled decoded primes, %d failures' % (len(idxs), bad))
        if bad:
            fail = 1
    if args.gaps and primes:
        pairs = sorted(zip([b - a for a, b in zip(primes, primes[1:])],
                           zip(primes, primes[1:])), reverse=True)[:args.gaps]
        bad = 0
        for g, (p, q) in pairs:
            if not (is_prime(p) and is_prime(q)):
                bad += 1
                continue
            for mm in range(p + 2, q, 2):
                if is_prime(mm):
                    bad += 1
                    break
        print('REF_GAP_INTERIORS  : %d largest gaps checked, %d failures' % (len(pairs), bad))
        if bad:
            fail = 1
    if args.dump_primes:
        with open(args.dump_primes, 'w') as f:
            for p in primes:
                f.write('%d\n' % p)
        print('REF_DUMP           : wrote %d primes to %s' % (len(primes), args.dump_primes))
    print('RESULT             : %s' % ('FAIL' if fail else 'PASS'))
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
