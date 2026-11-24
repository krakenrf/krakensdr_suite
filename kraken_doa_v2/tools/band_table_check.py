#!/usr/bin/env python3
"""Check the page's worldwide band table (the Signal ID box's "Bands" bar).

Reads the rows of BAND_TABLE in kraken_doa.html - the lines between
"// BAND TABLE BEGIN" and "// BAND TABLE END", one JSON array per row:
[lo Hz, hi Hz, short label, full name, category, ITU RR provision] plus,
for a regional row only, its Region - and checks them:

- regional rows: only the two ISM bands the RR designate for one Region
  (REGIONAL: 433.05-434.79 MHz Region 1, 902-928 MHz Region 2), category
  ism, "(R<n>)" in the label and "Region <n> only" in the full name;
- every row: integer Hz, lo < hi, inside the receiver's range (24 MHz to
  the Wideband variant's 6668 MHz, rounded out to 6.7 GHz), a short label of
  at most LABEL_MAX characters, a full name, a known category, a provision;
- sorted by lo, and no two rows overlap: the table has no nesting (a
  sub-band - 121.5 MHz in the airband - is named in its band's full name);
- coverage: rows from the bottom of the range (< 30 MHz) to its top (>= 6 GHz);
- consistency with the classifier's table, tools/sigclass/classes.json:
  each of its exclusive bands ("bands": a service that has the band to
  itself worldwide) must be covered by rows of the expected category and
  none of the table's rows may cross its edges; each class's rf_hz ranges
  are listed against the rows they meet (information, not an error - the
  two tables serve different purposes: rf_hz is where a class can exist
  anywhere, this table what is common to the whole world).

Usage: python3 tools/band_table_check.py [path/to/kraken_doa.html] [-v]
Exit status 0 when every check passes.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, '..', 'kraken_doa.html')
CLASSES = os.path.join(HERE, 'sigclass', 'classes.json')

LABEL_MAX = 16
RANGE_LO, RANGE_HI = 24_000_000, 6_700_000_000
# the only regional rows allowed (lo, hi, Region): ISM designated for one Region by the RR itself
REGIONAL = {(433_050_000, 434_790_000, 1),   # No. 5.138 (and 5.280)
            (902_000_000, 928_000_000, 2)}   # No. 5.150
CATS = {'broadcast', 'aero', 'marine', 'amateur', 'satellite', 'nav', 'ism', 'science', 'mobile'}
# classes.json's exclusive bands: the categories a row inside one may have
EXCLUSIVE_CATS = {
    'wfm': {'broadcast'},
    'vor': {'aero'}, 'ils_loc': {'aero'},
    'acars': {'aero'}, 'vdl2': {'aero'}, 'am_voice': {'aero'},
    'beacon406': {'satellite'},
    'dme': {'aero', 'nav'}, 'adsb': {'aero', 'nav'}, 'ssr1030': {'aero', 'nav'}, 'uat': {'aero', 'nav'},
}


def read_rows(path):
    rows, inside = [], False
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            s = line.strip()
            if s.startswith('// BAND TABLE BEGIN'):
                inside = True
                continue
            if s.startswith('// BAND TABLE END'):
                return rows
            if not inside or not s.startswith('['):
                continue
            try:
                rows.append((n, json.loads(s.rstrip(','))))
            except ValueError as e:
                rows.append((n, e))
    raise SystemExit('no "// BAND TABLE BEGIN" ... "// BAND TABLE END" block in ' + path)


def mhz(hz):
    return '%g' % (hz / 1e6)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    verbose = '-v' in sys.argv[1:]
    page = args[0] if args else PAGE
    rows = read_rows(page)
    errs = []

    good, regional = [], set()
    for n, r in rows:
        if isinstance(r, Exception):
            errs.append('line %d: not one JSON array: %s' % (n, r))
            continue
        if not (isinstance(r, list) and len(r) in (6, 7)):
            errs.append('line %d: a row has 6 fields (7 for a regional row)' % n)
            continue
        lo, hi, short, full, cat, ref = r[:6]
        if len(r) == 7:
            # the ONE exception to "worldwide": the ISM bands the RR designate for a single Region
            if (lo, hi, r[6]) not in REGIONAL:
                errs.append('line %d: %s-%s MHz marked as Region %r - only %s may be regional'
                            % (n, mhz(lo), mhz(hi), r[6], ', '.join('%s-%s MHz (R%d)' % (mhz(a), mhz(b), g) for a, b, g in sorted(REGIONAL))))
            elif cat != 'ism' or '(R%d)' % r[6] not in short or 'Region %d only' % r[6] not in full:
                errs.append('line %d: a regional row is ISM and says its Region in the label "(R%d)" and the full name "Region %d only"'
                            % (n, r[6], r[6]))
        if not (isinstance(lo, int) and isinstance(hi, int)):
            errs.append('line %d: lo / hi are integer Hz' % n)
            continue
        if not lo < hi:
            errs.append('line %d: lo %d >= hi %d' % (n, lo, hi))
        if lo < RANGE_LO or hi > RANGE_HI:
            errs.append('line %d: %s-%s MHz outside %s-%s MHz' % (n, mhz(lo), mhz(hi), mhz(RANGE_LO), mhz(RANGE_HI)))
        if not (isinstance(short, str) and 0 < len(short) <= LABEL_MAX):
            errs.append('line %d: short label %r: 1..%d characters' % (n, short, LABEL_MAX))
        if not (isinstance(full, str) and len(full) >= len(short or '')):
            errs.append('line %d: no full name' % n)
        if cat not in CATS:
            errs.append('line %d: category %r not in %s' % (n, cat, sorted(CATS)))
        if not (isinstance(ref, str) and ref):
            errs.append('line %d: no RR provision' % n)
        for s in (short, full, ref):
            if isinstance(s, str) and re.search(r'[\U0001F000-\U0001FFFF☀-➿]', s):
                errs.append('line %d: an emoji / pictogram in %r' % (n, s))
        good.append((n, lo, hi, short, cat))
        if len(r) == 7:
            regional.add((lo, hi, r[6]))
    if regional != REGIONAL:
        errs.append('regional rows: %s, expected exactly %s' % (sorted(regional), sorted(REGIONAL)))

    for (n0, lo0, hi0, s0, _), (n1, lo1, hi1, s1, _) in zip(good, good[1:]):
        if lo1 < lo0:
            errs.append('line %d: not sorted (%s MHz after %s MHz)' % (n1, mhz(lo1), mhz(lo0)))
        if lo1 < hi0:
            errs.append('lines %d / %d: %s (%s-%s) and %s (%s-%s) overlap - the table has no nesting'
                        % (n0, n1, s0, mhz(lo0), mhz(hi0), s1, mhz(lo1), mhz(hi1)))
    if good:
        if good[0][1] > 30_000_000:
            errs.append('coverage: the first band starts at %s MHz (> 30 MHz)' % mhz(good[0][1]))
        if max(g[2] for g in good) < 6_000_000_000:
            errs.append('coverage: the last band ends at %s MHz (< 6 GHz)' % mhz(max(g[2] for g in good)))

    # the classifier's table (tools/sigclass/classes.json)
    info = []
    if os.path.exists(CLASSES):
        with open(CLASSES, encoding='utf-8') as f:
            cj = json.load(f)
        for b in cj.get('bands', []):
            lo, hi, only = b['lo'], b['hi'], b.get('only', [])
            want = set()
            for c in only:
                want |= EXCLUSIVE_CATS.get(c, set())
            inside = [g for g in good if g[1] < hi and g[2] > lo]
            for g in inside:
                if g[1] < lo or g[2] > hi:
                    errs.append('classes.json exclusive band %s-%s MHz (%s): row %s (%s-%s MHz) crosses its edge'
                                % (mhz(lo), mhz(hi), '/'.join(only), g[3], mhz(g[1]), mhz(g[2])))
                if want and g[4] not in want:
                    errs.append('classes.json exclusive band %s-%s MHz (%s): row %s has category %s, expected %s'
                                % (mhz(lo), mhz(hi), '/'.join(only), g[3], g[4], '/'.join(sorted(want))))
            covered, x = True, lo
            for g in sorted(inside, key=lambda g: g[1]):
                if g[1] > x:
                    covered = False
                x = max(x, g[2])
            if x < hi or not covered:
                errs.append('classes.json exclusive band %s-%s MHz (%s): not covered by the table'
                            % (mhz(lo), mhz(hi), '/'.join(only)))
            info.append('exclusive %s-%s MHz (%s): %s' % (mhz(lo), mhz(hi), '/'.join(only),
                        ', '.join('%s %s-%s' % (g[3], mhz(g[1]), mhz(g[2])) for g in inside)))
        for c in cj.get('classes', []):
            for lo, hi in c.get('rf_hz') or []:
                meet = [g for g in good if g[1] < hi and g[2] > lo]
                info.append('rf_hz %s %s-%s MHz: %s' % (c['id'], mhz(lo), mhz(min(hi, RANGE_HI)),
                            ', '.join('%s %s-%s' % (g[3], mhz(g[1]), mhz(g[2])) for g in meet) or '(no worldwide band)'))
    else:
        errs.append('no ' + CLASSES)

    if verbose:
        for line in info:
            print('  ' + line)
    for e in errs:
        print('FAIL ' + e)
    print('%d rows, %s - %s MHz: %s' % (len(good), mhz(good[0][1]) if good else '-',
                                         mhz(max(g[2] for g in good)) if good else '-',
                                         'OK' if not errs else '%d problem(s)' % len(errs)))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
