#!/usr/bin/env python3
"""Extract the AAC Huffman code book 11 (spectrum, escape), the scale factor
code book and the 44.1 / 48 kHz long-window scale factor band offsets from
VisualOn's vo-aacenc aac_rom.c (Apache-2.0) into a small C++ header.
Usage: make_aac_tables.py aac_rom.c OUT.hpp"""
import re
import sys

src = re.sub(r'/\*.*?\*/', ' ', open(sys.argv[1], encoding='latin-1').read(), flags=re.S)


def block(name):
    m = re.search(re.escape(name) + r'[^=]*=\s*\{(.*?)\};', src, re.S)
    if not m:
        raise SystemExit('no ' + name)
    return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', m.group(1))]


c11 = block('const UWord16 huff_ctab11[17][17]')
l11 = block('const UWord16 huff_ltab11[17][17]')
cs = block('const UWord32 huff_ctabscf[121]')
ls = block('const UWord16 huff_ltabscf[121]')
sfb = block('const short sfBandTabLong[325]')
assert len(c11) == 289 and len(l11) == 289 and len(cs) == 121 and len(ls) == 121 and len(sfb) == 325
# the 44.1 / 48 kHz long block offsets start at sfBandTabLongOffset[3] = 90, 50 values
off48 = sfb[90:140]
assert off48[0] == 0 and off48[-1] == 1024 and off48[-2] == 928 and len(off48) == 50
# prefix-free checks: Kraft sum = 1 for both complete codes, codes fit their lengths
for codes, lens in ((c11, l11), (cs, ls)):
    assert all(c < (1 << n) for c, n in zip(codes, lens))
    assert abs(sum(2.0 ** -n for n in lens) - 1.0) < 1e-12
    seen = set()
    for c, n in zip(codes, lens):
        s = format(c, '0%db' % n)
        for k in range(1, n):
            assert s[:k] not in seen
        seen.add(s)
assert cs[60] == 0 and ls[60] == 1          # scale factor delta 0 = "0"

o = ['// AAC tables taken from VisualOn vo-aacenc (aacenc/src/aac_rom.c),',
     '// Copyright 2003-2010, VisualOn, Inc., licensed under the Apache License,',
     '// Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0). Extracted by',
     '// make_aac_tables.py; values unchanged, layout reformatted. Distributed on an',
     '// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.',
     '#pragma once', '#include <cstdint>', '',
     '// spectrum code book 11 (unsigned pairs y, z = 0..16, 16 = escape): code and length',
     'static const uint16_t kAacCb11Code[17][17] = {']
for y in range(17):
    o.append('    {%s},' % ', '.join('0x%04x' % c11[y * 17 + z] for z in range(17)))
o.append('};')
o.append('static const uint8_t kAacCb11Len[17][17] = {')
for y in range(17):
    o.append('    {%s},' % ', '.join(str(l11[y * 17 + z]) for z in range(17)))
o.append('};')
o.append('// scale factor code book: index = delta + 60')
o.append('static const uint32_t kAacScfCode[121] = {%s};' % ', '.join('0x%x' % x for x in cs))
o.append('static const uint8_t kAacScfLen[121] = {%s};' % ', '.join(map(str, ls)))
o.append('// long-window scale factor band offsets, 44.1 / 48 kHz (1024 lines; the 960-line')
o.append('// frame uses the same bands, the last one ending at 960)')
o.append('static const int kAacSwbOffset48[50] = {%s};' % ', '.join(map(str, off48)))
open(sys.argv[2], 'w').write('\n'.join(o) + '\n')
print('ok')
