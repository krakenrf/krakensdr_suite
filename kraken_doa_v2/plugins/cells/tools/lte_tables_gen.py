#!/usr/bin/env python3
"""Write plugins/cells/lte_tables.hpp from the 3GPP specifications themselves.

  lte_tables_gen.py 36212-xyz.docx 36213-xyz_s06-s07.docx [OUT.hpp]

The .docx files come from the 3GPP archive (zips of the Word files):
  https://www.3gpp.org/ftp/Specs/archive/36_series/36.212/   (36212-<ver>.zip)
  https://www.3gpp.org/ftp/Specs/archive/36_series/36.213/   (36213-<ver>.zip -
      the part holding section 7, e.g. 36213-j50_s06-s07.docx)
(3gpp.org refuses some scripted clients - a browser, or curl with a browser
User-Agent, gets them). The tables taken:

  QPP       TS 36.212 Table 5.1.3-3   turbo code internal interleaver parameters
  TBS       TS 36.213 Table 7.1.7.2.1-1, rows I_TBS 0..26 x N_PRB 1..110 (the
            later releases add rows such as 26A / 33A for 256 / 1024-QAM; rows
            0..26 have not changed since Release 8)
  TBS_1C    TS 36.213 Table 7.1.7.2.3-1   DCI format 1C transport block sizes
  MCS_ITBS  TS 36.213 Table 7.1.7.1-1     I_MCS 0..28 -> I_TBS (PDSCH)

Every table is checked for completeness (each entry exactly once). Python 3,
standard library only. Without OUT the file is written to stdout.
"""
import re
import sys
import zipfile
import xml.etree.ElementTree as ET

W = '{http://schemas.openxmlformats.org/wordprocessingml/2006/main}'


def text(el):
    return ''.join(t.text or '' for t in el.iter(W + 't')).replace(' ', ' ').strip()


def body_items(path):
    """('p' | 'tbl', element) in document order"""
    with zipfile.ZipFile(path) as z:
        root = ET.fromstring(z.read('word/document.xml'))
    for el in root.find(W + 'body').iter():
        if el.tag == W + 'p' and el.find('.//' + W + 'tbl') is None:
            yield 'p', el
        elif el.tag == W + 'tbl':
            yield 'tbl', el


def first_table_after(path, caption):
    """the rows (lists of cell texts) of the first table after the paragraph
    that starts with `caption` (e.g. 'Table 5.1.3-3') - '...: <title>'"""
    found = False
    for kind, el in body_items(path):
        if kind == 'p' and not found:
            t = text(el)
            if t.startswith(caption) and t[len(caption):len(caption) + 1] in (':', ' ', ''):
                found = True
        elif kind == 'tbl' and found:
            return table_rows(el)
    sys.exit('%s: no table after "%s"' % (path, caption))


def table_rows(tbl):
    """cell texts per row; a cell that continues a vertical merge (w:vMerge
    without val="restart" - e.g. two MCS indexes sharing one TBS index) gets
    the text of the merged cell above it (columns counted with w:gridSpan)"""
    out = []
    above = {}
    for tr in tbl.findall(W + 'tr'):
        row = []
        col = 0
        for tc in tr.findall(W + 'tc'):
            pr = tc.find(W + 'tcPr')
            span = 1
            vm = None
            if pr is not None:
                gs = pr.find(W + 'gridSpan')
                if gs is not None:
                    span = int(gs.get(W + 'val', '1'))
                vm = pr.find(W + 'vMerge')
            t = text(tc)
            if vm is not None and vm.get(W + 'val') != 'restart':
                t = above.get(col, t)
            above[col] = t
            row.append(t)
            col += span
        out.append(row)
    return out


def ints(cells):
    return [int(c) for c in cells] if cells and all(re.fullmatch(r'\d+', c) for c in cells) else None


def spec_version(path, number):
    """'36212-j30' -> 'V19.3.0' (3GPP: digits 0-9, then a = 10, b = 11 ...)"""
    m = re.search(number.replace('.', '') + r'-([0-9a-z])([0-9a-z])([0-9a-z])', path)
    if not m:
        return '(version unknown)'
    return 'V' + '.'.join(str(int(c, 36)) for c in m.groups())


def qpp(path):
    rows = first_table_after(path, 'Table 5.1.3-3')
    out = {}
    for r in rows[1:]:
        for g in range(0, len(r) - 3, 4):
            v = ints(r[g:g + 4])
            if v:
                i, k, f1, f2 = v
                if i in out:
                    sys.exit('QPP: i = %d twice' % i)
                out[i] = (k, f1, f2)
    if sorted(out) != list(range(1, 189)):
        sys.exit('QPP: expected i = 1..188, got %d entries' % len(out))
    return [out[i] for i in range(1, 189)]


def tbs(path):
    rows = first_table_after(path, 'Table 7.1.7.2.1-1')
    t = [[None] * 110 for _ in range(27)]
    cols = None
    for r in rows:
        if r and r[0] == '' and ints(r[1:]):
            cols = ints(r[1:])                   # a block's N_PRB header row
        elif r and re.fullmatch(r'\d+', r[0]) and int(r[0]) <= 26 and cols:
            v = ints(r[1:])
            if not v or len(v) != len(cols):
                sys.exit('TBS: row %s does not match its N_PRB header' % r[0])
            for n, x in zip(cols, v):
                if t[int(r[0])][n - 1] is not None:
                    sys.exit('TBS: I_TBS %s N_PRB %d twice' % (r[0], n))
                t[int(r[0])][n - 1] = x
    if any(x is None for row in t for x in row):
        sys.exit('TBS: table incomplete')
    return t


def tbs_1c(path):
    rows = first_table_after(path, 'Table 7.1.7.2.3-1')
    out = {}
    for a, b in zip(rows, rows[1:]):
        idx = ints(a[1:]) if a and a[0] == '' else None
        val = ints(b[1:]) if b and b[0] == 'TBS' else None
        if idx and val and len(idx) == len(val):
            for i, v in zip(idx, val):
                out[i] = v
    if sorted(out) != list(range(32)):
        sys.exit('TBS_1C: expected I_TBS 0..31, got %d' % len(out))
    return [out[i] for i in range(32)]


def mcs_itbs(path):
    rows = first_table_after(path, 'Table 7.1.7.1-1')
    head = [c.lower() for c in rows[0]]
    if not head or 'mcs' not in head[0] or 'tbs' not in head[-1]:
        sys.exit('MCS table: unexpected header %r' % rows[0])
    out = {}
    for r in rows[1:]:
        # I_MCS 28 reads "26/26A" in later releases (26A: certain configurations
        # only - TS 36.213 7.1.7.2.1); the Release 8 value is the 26
        m = re.fullmatch(r'(\d+)(/\d+A)?', r[-1])
        if re.fullmatch(r'\d+', r[0]) and m:
            out[int(r[0])] = int(m.group(1))
    if any(i not in out for i in range(29)):
        sys.exit('MCS table: I_MCS 0..28 incomplete')
    return [out[i] for i in range(29)]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    s212, s213 = sys.argv[1], sys.argv[2]
    v212, v213 = spec_version(s212, '36.212'), spec_version(s213, '36.213')
    q, t, t1c, m = qpp(s212), tbs(s213), tbs_1c(s213), mcs_itbs(s213)
    L = ['#pragma once', '',
         '// LTE tables taken from the 3GPP specifications (written by',
         '// tools/lte_tables_gen.py from the specifications\' Word files: 3GPP TS',
         '// 36.212 %s, TS 36.213 %s; the tables are unchanged since Release 8):' % (v212, v213),
         '//   QPP turbo interleaver parameters (36.212 Table 5.1.3-3)',
         '//   transport block sizes (36.213 Table 7.1.7.2.1-1, I_TBS 0..26 x N_PRB 1..110)',
         '//   format 1C transport block sizes (36.213 Table 7.1.7.2.3-1)',
         '', '#include <cstdint>', '', 'namespace cells::lte {', '',
         'constexpr int QPP_N = 188;', '// code block size K, f1, f2',
         'constexpr uint16_t QPP[QPP_N][3] = {']
    for i in range(0, 188, 6):
        L.append('    ' + ' '.join('{%d, %d, %d},' % e for e in q[i:i + 6]))
    L += ['};', '', 'constexpr uint32_t TBS[27][110] = {']
    for row in t:
        L.append('    {')
        for i in range(0, 110, 14):
            L.append('        ' + ' '.join('%d,' % x for x in row[i:i + 14]))
        L.append('    },')
    L += ['};', '',
          'constexpr uint16_t TBS_1C[32] = {%s};' % ', '.join(map(str, t1c)), '',
          '// PDSCH MCS -> I_TBS (36.213 Table 7.1.7.1-1, 64QAM table)',
          'constexpr uint8_t MCS_ITBS[29] = {%s};' % ', '.join(map(str, m)), '',
          '}  // namespace cells::lte', '']
    out = '\n'.join(L)
    if len(sys.argv) > 3:
        with open(sys.argv[3], 'w', newline='\n') as f:
            f.write(out)
    else:
        sys.stdout.write(out)


if __name__ == '__main__':
    main()
