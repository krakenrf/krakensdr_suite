#!/usr/bin/env python3
"""Transcribe the DAB tables the generator needs straight from the pdftotext
-layout output of the ETSI PDFs into a C++ header, with sanity checks.
Usage: make_tables.py EN300401.txt TS103466.txt OUT.hpp"""
import re
import sys

en = open(sys.argv[1], encoding='utf-8').read().split('\n')
ts = open(sys.argv[2], encoding='utf-8').read().split('\n')
out = []


def find(lines, pat, start=0):
    for i in range(start, len(lines)):
        if re.search(pat, lines[i]):
            return i
    raise SystemExit('not found: ' + pat)


def ints(s):
    return [int(x) for x in re.findall(r'-?\d+', s.replace('−', '-'))]


# ---- EN 300 401 table 13: puncturing vectors ------------------------------
i = find(en, r'Table 13: Puncturing vectors')
pv = {}
for ln in en[i:i + 60]:
    m = re.match(r'\s*PI=(\d+):\s+([01 ]+)$', ln)
    if m:
        bits = m.group(2).replace(' ', '')
        assert len(bits) == 32, ln
        pv[int(m.group(1))] = bits
assert sorted(pv) == list(range(1, 25)), sorted(pv)
for p, b in pv.items():
    assert b.count('1') == 8 + p, (p, b)          # code rate 8/(8+PI)
j = find(en, r'VT = \(', i)
vt = re.search(r'\(([01 ]+)\)', en[j]).group(1).replace(' ', '')
assert len(vt) == 24 and vt.count('1') == 12
out.append('// EN 300 401 V2.2.1 table 13 (puncturing vectors PI = 1..24, v_PI,0 first) and VT (clause 11.1.2)')
out.append('static const char* const kPunct[25] = {nullptr,')
out += ['    "%s",' % pv[p] for p in range(1, 25)]
out.append('};')
out.append('static const char kPunctTail[] = "%s";' % vt)

# ---- table 14 (UEP: I, L per bit rate) and table 15 (UEP profiles) ----------
i = find(en, r'Table 15: UEP coded service component protection profiles')
uep = []
k = i
while len(uep) < 64 and k < i + 120:
    v = ints(en[k])
    # rate P L1 L2 L3 L4 PI1 PI2 PI3 [PI4] pad
    if len(v) in (10, 11) and v[0] in (32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384) and 1 <= v[1] <= 5:
        if len(v) == 10:       # PI4 printed as '-' (L4 = 0)
            v = v[:9] + [0] + v[9:]
        uep.append(v)
    k += 1
assert len(uep) == 64, len(uep)          # 14 rates x 5 levels, 6 not provided (table 16 "X")
# table 8: short-form index -> size, P, bit rate
i8 = find(en, r'Table 8: Sub-channel size for service components')
t8 = {}
for ln in en[i8:i8 + 45]:
    v = ints(ln)
    if len(v) == 8:
        t8[v[0]] = v[1:4]
        t8[v[4]] = v[5:8]
    elif len(v) == 4 and v[0] < 64:
        t8[v[0]] = v[1:4]
assert sorted(t8) == list(range(64)), sorted(t8)
for idx, (size, p, rate) in t8.items():
    rows = [r for r in uep if r[0] == rate and r[1] == p]
    assert len(rows) == 1, (idx, rate, p)
    r = rows[0]
    L = r[2] + r[3] + r[4] + r[5]
    assert L == rate * 24 // 32, (rate, L)           # table 14: L = I / 32, I = 24 x rate
    bits = sum(4 * r[2 + q] * (8 + r[6 + q]) for q in range(4) if r[2 + q]) + 12 + r[10]
    assert bits == size * 64, (idx, bits, size * 64)
out.append('')
out.append('// EN 300 401 table 8 (short form: index -> CUs, protection level, bit rate) joined with table 15')
out.append('// (L1..L4, PI1..PI4, padding bits); checked: L = 24 x rate / 32, 4 sum Li (8 + PIi) + 12 + pad = 64 x CUs')
out.append('struct UepProfile { int index, cu, level, kbps, L[4], PI[4], pad; };')
out.append('static const UepProfile kUep[64] = {')
for idx in range(64):
    size, p, rate = t8[idx]
    r = [x for x in uep if x[0] == rate and x[1] == p][0]
    out.append('    {%d, %d, %d, %d, {%d, %d, %d, %d}, {%d, %d, %d, %d}, %d},' % (
        idx, size, p, rate, r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10]))
out.append('};')

# ---- table 23 / 24: phase reference symbol --------------------------------
i = find(en, r'Table 23: Relation between the indices i')
t23 = []
for ln in en[i:i + 40]:
    v = ints(ln)
    if len(v) == 10:
        t23.append(v[0:5])
        t23.append(v[5:10])
assert len(t23) == 48
t23.sort()
lo = -768
for r in t23:
    assert r[0] == lo and r[1] == lo + 31 and r[2] == lo, r
    lo = r[1] + 1
    if lo == 0:
        lo = 1
    assert 0 <= r[3] <= 3 and 0 <= r[4] <= 3
i = find(en, r'Table 24: Time-Frequency-Phase parameter h values')
h = [[], [], [], []]
for ln in en[i:i + 14]:
    m = re.match(r'\s*h(\d), ,j\s+(.*)$', ln)
    if m:
        h[int(m.group(1))] += ints(m.group(2))
assert all(len(x) == 32 for x in h)
out.append('')
out.append('// EN 300 401 table 23 (kmin, kmax, k\', i, n) and table 24 (h[i][j]): phi_k = pi/2 (h[i][k - k\'] + n)')
out.append('static const int kPrsTab[48][5] = {')
out += ['    {%d, %d, %d, %d, %d},' % tuple(r) for r in t23]
out.append('};')
out.append('static const int kPrsH[4][32] = {')
out += ['    {%s},' % ', '.join(map(str, x)) for x in h]
out.append('};')

# ---- table 26: TII patterns ------------------------------------------------
i = find(en, r'Table 26: TII pattern for transmission mode I')
pat = {}
for ln in en[i:i + 30]:
    for m in re.finditer(r'(\d+)\s+([01]{7}\s?[01]?(?:[01])?)', ln):
        b = m.group(2).replace(' ', '')
        if len(b) == 8:
            pat[int(m.group(1))] = b
assert sorted(pat) == list(range(70)), sorted(pat)
assert all(b.count('1') == 4 for b in pat.values())
assert len(set(pat.values())) == 70
out.append('')
out.append('// EN 300 401 table 26: TII pattern a_b(p), b = 0..7 left to right (70 distinct, 4 ones each)')
out.append('static const char* const kTiiPattern[70] = {')
out += ['    "%s",' % pat[p] for p in range(70)]
out.append('};')

# ---- TS 103 466 table 1: scale factors ------------------------------------
i = find(ts, r'Table 1: Scale Factors')
scf = {}
for ln in ts[i:i + 40]:
    for m in re.finditer(r'(\d+)\s+(\d),(\d{14})', ln):
        scf[int(m.group(1))] = float(m.group(2) + '.' + m.group(3))
assert sorted(scf) == list(range(63)), sorted(scf)
for k in range(63):
    assert abs(scf[k] - 2.0 * 2 ** (-k / 3.0)) < 1e-12, k
out.append('')
out.append('// TS 103 466 V1.2.1 table 1: Layer II scale factors (checked: 2 x 2^(-i/3))')
out.append('static const double kScf[63] = {')
out += ['    %.14f,' % scf[k] for k in range(63)]
out.append('};')

# ---- TS 103 466 tables 4, 5, 6: allocation (nbal, nlevels per index) ------
def alloc_table(title, rows):
    i = find(ts, title)
    res = {}
    for ln in ts[i:i + 50]:
        s = ln.replace('1 023', '1023').replace('2 047', '2047').replace('4 095', '4095').replace(
            '8 191', '8191').replace('16 383', '16383').replace('32 767', '32767').replace('65 535', '65535')
        m = re.match(r'\s*(\d+)\s+(\d)\s+-\s*(.*)$', s)
        if m:
            sb, nbal = int(m.group(1)), int(m.group(2))
            lv = ints(m.group(3))
            assert len(lv) == (2 ** nbal - 1 if nbal else 0), (title, sb, nbal, lv)
            res[sb] = (nbal, lv)
    assert sorted(res) == list(range(rows)), (title, sorted(res))
    return res


t4 = alloc_table(r'Table 4: Bit allocation and possible quantization', 32)
t5 = alloc_table(r'Table 5: Bit allocation and possible quantization', 32)
t6 = alloc_table(r'Table 6: Bit allocation and possible quantization', 32)
assert sum(t4[s][0] for s in range(32)) == 88 and sum(t5[s][0] for s in range(32)) == 26
assert sum(t6[s][0] for s in range(32)) == 75
for name, t, doc in (('kAlloc48High', t4, 'table 4 (48 kHz, 56-192 kbit/s mono, sblimit 27)'),
                     ('kAlloc48Low', t5, 'table 5 (48 kHz, 32 / 48 kbit/s mono, sblimit 8)'),
                     ('kAlloc24', t6, 'table 6 (24 kHz, sblimit 30)')):
    out.append('')
    out.append('// TS 103 466 %s: nbal, then nlevels for index 1..2^nbal-1' % doc)
    out.append('static const int %s[32][17] = {' % name)
    for s in range(32):
        nb, lv = t[s]
        out.append('    {%s},' % ', '.join(map(str, [nb] + lv + [0] * (16 - len(lv)))))
    out.append('};')

# ---- TS 103 466 table 7: quantization coefficients A, B -------------------
i = find(ts, r'Table 7: Quantization coefficients')
q = {}
for ln in ts[i:i + 25]:
    s = ln.replace('1 023', '1023').replace('2 047', '2047').replace('4 095', '4095').replace(
        '8 191', '8191').replace('16 383', '16383').replace('32 767', '32767').replace('65 535', '65535')
    m = re.match(r'\s*(\d+)\s+(\d),(\d+)\s+-(\d),(\d+)', s)
    if m:
        q[int(m.group(1))] = (float(m.group(2) + '.' + m.group(3)), -float(m.group(4) + '.' + m.group(5)))
assert sorted(q) == [3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535], sorted(q)
out.append('')
out.append('// TS 103 466 table 7: quantization coefficients (steps, A, B)')
out.append('static const double kQuantAB[17][3] = {')
out += ['    {%d, %.9f, %.9f},' % (k, q[k][0], q[k][1]) for k in sorted(q)]
out.append('};')

hdr = ['// Generated by make_tables.py from the pdftotext -layout output of ETSI EN 300 401 V2.2.1',
       '// and ETSI TS 103 466 V1.2.1 (the tables cited below). Do not edit by hand.',
       '#pragma once', '']
open(sys.argv[3], 'w').write('\n'.join(hdr + out) + '\n')
print('ok: %d lines' % len(out))
