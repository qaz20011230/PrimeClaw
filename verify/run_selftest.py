#!/usr/bin/env python3
"""Self-test driver for the independent PRMGAP01 v1 verifier.

Builds verify_primes, generates synthetic datasets with mk_synth.py, runs
positive tests (correct data must PASS), corruption tests (bad data must be
rejected with the expected exit code) and cross-checks C vs the pure-Python
reference decoder/ref_verify.py.

Run:  python3 run_selftest.py
Writes: results/selftest.txt  (full log); exits 0 iff every test passed.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SYNTH = os.path.join(HERE, 'synth')
RESULTS = os.path.join(HERE, 'results')
BIN = os.path.join(HERE, 'verify_primes')
sys.path.insert(0, HERE)
import mk_synth  # noqa: E402
import ref_verify  # noqa: E402

LOG = []
CASES = []


def sh(cmd, cwd=HERE):
    p = subprocess.run(cmd, shell=True, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def rec(name, ok, detail=''):
    CASES.append((name, ok, detail))
    LOG.append('### %s: %s %s\n' % (name, 'PASS' if ok else 'FAIL', detail))


def kv(out):
    d = {}
    for line in out.splitlines():
        if ':' in line:
            k, v = line.split(':', 1)
            if k.strip() and k.strip().replace('_', '').replace(' ', '').isalpha():
                d.setdefault(k.strip(), v.strip())
    return d


def make(name, limit, block, vary=False, cps='10,100,1000,10000,100000,1000000',
         fault=None):
    out = os.path.join(SYNTH, name + '.bin')
    exj = os.path.join(SYNTH, name + '.expect.json')
    prj = os.path.join(SYNTH, name + '.primes.txt')
    cmd = ('python3 mk_synth.py --limit %d --block %d --out %s --expect-json %s '
           '--write-primes %s --checkpoints %s' % (limit, block, out, exj, prj, cps))
    if vary:
        cmd += ' --vary'
    if fault:
        cmd += ' --fault ' + fault
    rc, so = sh(cmd)
    assert rc == 0, so
    return out, exj, prj, json.load(open(exj))


def run_c(args):
    return sh('./verify_primes ' + args)


def pi_flags(ex):
    return '--expect-pi ' + ','.join('%s:%d' % (x, n) for x, n in ex['pi'].items())


def expect_lines(out, ex):
    missing = []
    for x, n in ex['pi'].items():
        if 'expected %d) PASS' % n not in out:
            missing.append('pi(%s)' % x)
    if ('EXPECTED_LAST' in out and ex and 'expected %d) PASS' % ex['last_prime'] not in out):
        missing.append('last_prime')
    return missing


def main():
    os.makedirs(SYNTH, exist_ok=True)
    os.makedirs(RESULTS, exist_ok=True)

    # 0) build
    rc, out = sh('cc -O2 -Wall -Wextra -pthread -o verify_primes verify_primes.c')
    LOG.append('### build cc\n' + out)
    rec('build verify_primes.c', rc == 0 and os.path.exists(BIN), '')

    # 1) C Miller-Rabin self-test
    rc, out = run_c('--mr-selftest 200000')
    LOG.append('### C mr-selftest\n' + out)
    rec('C Miller-Rabin vs trial division 2..200000 + known vectors', rc == 0)

    # 2) positive cases
    out, exj, prj, ex = make('t2', 2, 1)
    rc, so = run_c('%s --meta' % out)
    k = kv(so)
    rec('t2 (limit 2): meta PASS, 1 block, total 1, last 2',
        rc == 0 and k.get('BLOCKS') == '1' and k.get('TOTAL_PRIMES', '').startswith('1 ')
        and k.get('LAST_PRIME') == '2', so.strip().splitlines()[-1])
    rc, so = run_c('%s --full --samples 5 --gaps 5' % out)
    k = kv(so)
    rec('t2 (limit 2): full PASS, decoded 1', rc == 0 and 'RESULT             : PASS' in so)

    out, exj, prj, ex = make('t100', 100, 7, cps='10,100')
    rc, so = run_c('%s --full --samples 20 --gaps 20 %s --expect-last %d'
                   % (out, pi_flags(ex), ex['last_prime']))
    LOG.append('### t100 full\n' + so)
    rec('t100 (limit 100, 7-prime blocks): full PASS vs python sieve expectations',
        rc == 0 and not expect_lines(so, ex))

    out, exj, prj, ex = make('t1e5', 100000, 1000, cps='1000,10000,100000')
    rc, so = run_c('%s --full --samples 500 --gaps 500 %s --expect-last %d '
                   '--dump-primes %s'
                   % (out, pi_flags(ex), ex['last_prime'], prj + '.cdump'))
    LOG.append('### t1e5 full\n' + so)
    ok = rc == 0 and 'RESULT             : PASS' in so and not expect_lines(so, ex)
    rec('t1e5 (limit 100000, 1000-prime blocks): full PASS', ok)
    # C dump vs independent sieve list vs python reference decoder
    rc2, so2 = sh('python3 ref_verify.py --dataset %s --expect-json %s --samples 300 '
                  '--gaps 100 --dump-primes %s' % (out, exj, prj + '.refdump'))
    LOG.append('### t1e5 ref_verify\n' + so2)
    rec('t1e5: pure-Python reference decoder/validator PASS', rc2 == 0)
    same1 = open(prj).read() == open(prj + '.cdump').read()
    same2 = open(prj).read() == open(prj + '.refdump').read()
    rec('t1e5: C dump == sieve output == Python ref dump (all primes)',
        same1 and same2, 'C:%s ref:%s' % (same1, same2))

    out, exj, prj, ex = make('t1e6v', 1000000, 0, vary=True, cps='1000,100000,1000000')
    rc, so = run_c('%s --full --threads 4 --samples 1000 --gaps 1000 %s --expect-last %d'
                   % (out, pi_flags(ex), ex['last_prime']))
    LOG.append('### t1e6v full\n' + so)
    k = kv(so)
    mg = k.get('MAX_GAP', '')
    mg_ok = mg.startswith(str(ex['max_gap']) + ' ')
    rec('t1e6 vary-blocks: full PASS, max_gap == python value %d' % ex['max_gap'],
        rc == 0 and mg_ok, 'got=' + mg)
    rc2, so2 = sh('python3 ref_verify.py --dataset %s --expect-json %s --gaps 200'
                  % (out, exj))
    rec('t1e6 vary-blocks: Python reference PASS incl. max gap/pi', rc2 == 0)

    # 3) empty dataset (global header only)
    empty = os.path.join(SYNTH, 'empty.bin')
    open(empty, 'wb').write(mk_synth.GLOBAL.pack(b'PRMGAP01', 1, 0, 0, 0))
    rc, so = run_c('%s --meta' % empty)
    k = kv(so)
    rec('empty dataset: meta PASS, 0 blocks', rc == 0 and k.get('BLOCKS') == '0')
    rc, so = run_c('%s --full' % empty)
    k = kv(so)
    rec('empty dataset: full PASS, 0 primes', rc == 0 and 'RESULT             : PASS' in so)

    # 4) corruption cases: must be rejected
    for fault, (want_rc, why) in sorted(mk_synth.FAULTS.items()):
        out, exj, prj, ex = make('bad_' + fault, 100000, 1000, fault=fault)
        rc, so = run_c('%s --full --samples 20 --gaps 20' % out)
        LOG.append('### fault %s (want exit %d, got %d)\n%s\n' % (fault, want_rc, rc, so))
        ok = (rc == want_rc)
        rec('corrupt %-16s -> exit %d (%s)' % (fault, want_rc, why), ok,
            'got exit %d' % rc)
        # meta must also reject the same file, except for faults that only live
        # in payload CONTENT (headers-only mode cannot and must not decode it)
        rc2, so2 = run_c('%s --meta' % out)
        if fault in ('gap_odd', 'gap_zero'):
            ok2 = rc2 == 0
            rec('corrupt %-16s --meta correctly sees only headers (exit 0)' % fault, ok2)
        else:
            ok2 = rc2 != 0
            rec('corrupt %-16s --meta also rejects (exit %d)' % (fault, rc2), ok2)

    # 5) semantically bad but structurally valid data -> Miller-Rabin must catch
    primes = mk_synth.sieve(1000)
    ps = list(primes)
    ps[-1] = 999  # composite last prime, gaps stay even
    data, blocks = mk_synth.encode(ps, [1000])
    f = os.path.join(SYNTH, 'bad_composite.bin')
    open(f, 'wb').write(data)
    rc, so = run_c('%s --full --samples 10 --gaps 10' % f)
    LOG.append('### composite encoded\n' + so)
    rec('composite stored as block end -> rejected by Miller-Rabin (exit 1)', rc == 1)

    ps = [p for p in primes if p != 13]
    data, blocks = mk_synth.encode(ps, [1000])
    f = os.path.join(SYNTH, 'bad_missing.bin')
    open(f, 'wb').write(data)
    rc, so = run_c('%s --full --gaps 8192 --samples 10' % f)
    LOG.append('### missing prime encoded\n' + so)
    rec('omitted interior prime -> caught by gap-interior check (exit 1)', rc == 1)

    # 6) sha256 cross-check via shasum
    out, exj, prj, ex = make('sha', 10000, 100)
    rc, so = sh('shasum -a 256 %s' % out)
    got = so.split()[0]
    rec('shasum -a 256 == hashlib value recorded by generator', got == ex['sha256'])

    # 7) reference MR self-test
    rc, so = sh('python3 ref_verify.py --mr-selftest 5000')
    LOG.append('### ref mr selftest\n' + so)
    rec('Python reference Miller-Rabin vs trial division 2..5000', rc == 0)

    # summary
    passed = sum(1 for _, ok, _ in CASES if ok)
    lines = ['\n=== SELFTEST SUMMARY: %d/%d passed ===' % (passed, len(CASES))]
    for name, ok, detail in CASES:
        lines.append(' %-4s %s%s' % ('PASS' if ok else 'FAIL', name,
                                     ('  [' + detail + ']') if detail and not ok else ''))
    summary = '\n'.join(lines)
    LOG.append(summary)
    with open(os.path.join(RESULTS, 'selftest.txt'), 'w') as f:
        f.write(''.join(LOG))
    print(summary)
    return 0 if passed == len(CASES) else 1


if __name__ == '__main__':
    sys.exit(main())
