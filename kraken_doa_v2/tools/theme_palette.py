#!/usr/bin/env python3
"""The page's colour palette (docs/dev/theme.md), defined in OKLCH.

  tools/theme_palette.py              the tokens: name, hex, L C h, what for
  tools/theme_palette.py --css        the :root block as it stands in kraken_doa.html
  tools/theme_palette.py --contrast   WCAG contrast of the text and state tokens on the surfaces
  tools/theme_palette.py --check [kraken_doa.html]
        the page's :root block equals this palette, every var(--name) it uses
        is defined, and how many colour literals in its CSS are not tokens

The palette is designed in OKLCH because its three axes are what the eye
judges: lightness (contrast, elevation), chroma (how loud) and hue. Change a
token here, paste --css into the page's :root block and replace the token's
old hex in the page's script (inline styles, canvas code) - see theme.md.
No dependencies beyond the standard library.
"""
import math
import os
import re
import sys

HB = 255.0   # the tint of the neutrals: the accent's hue, almost no chroma
HA = 250.0   # the accent: azure

# name, L, C, h, what it is for
NEUTRAL = [
    ('s0', 0.130, 0.012, HB, 'wells: plot and list backgrounds'),
    ('s1', 0.165, 0.013, HB, 'the page behind the panels; text boxes'),
    ('s2', 0.195, 0.014, HB, 'the sidebar, the bars; boxes inside boxes'),
    ('s3', 0.225, 0.015, HB, 'panels and boxes'),
    ('s4', 0.262, 0.017, HB, 'headers, raised parts'),
    ('s5', 0.300, 0.019, HB, 'hover'),
    ('l1', 0.350, 0.022, HB, 'lines between parts'),
    ('l2', 0.385, 0.023, HB, 'borders of text boxes; plain buttons'),
    ('l3', 0.425, 0.025, HB, 'borders of controls'),
    ('l4', 0.465, 0.027, HB, 'borders of controls, hover'),
    ('l5', 0.520, 0.025, HB, 'strong borders'),
    ('t6', 0.570, 0.020, HB, 'faint text: units, placeholders'),
    ('t5', 0.625, 0.022, HB, 'muted text'),
    ('t4', 0.675, 0.024, HB, 'labels'),
    ('t3', 0.720, 0.025, HB, 'labels, secondary text'),
    ('t2', 0.800, 0.020, HB, 'secondary text'),
    ('t1', 0.870, 0.016, HB, 'text'),
    ('t0', 0.915, 0.014, HB, 'text, values'),
]
ACCENT = [
    ('ac-d2', 0.300, 0.070, HA, 'accent: dark container'),
    ('ac-d', 0.420, 0.120, HA, 'accent: container'),
    ('ac', 0.550, 0.165, HA, 'accent: primary buttons, selected'),
    ('ac-h', 0.620, 0.160, HA, 'accent: hover, borders of selected'),
    ('ac-l', 0.690, 0.150, HA, 'accent: slider thumbs, focus'),
    ('ac-t', 0.765, 0.112, HA, 'accent text: links, readouts, digits'),
    ('ac-t2', 0.840, 0.080, HA, 'accent text, light'),
    ('ac-t3', 0.910, 0.045, HA, 'accent text, lightest'),
]
STATES = [
    [('ok-d', 0.330, 0.075, 152, 'good: container'),
     ('ok-m', 0.500, 0.125, 152, 'good: fill (a switch that is on)'),
     ('ok', 0.670, 0.145, 152, 'good'),
     ('ok-t', 0.780, 0.115, 152, 'good: text'),
     ('ok-t2', 0.880, 0.080, 152, 'good: light text')],
    [('warn-d', 0.330, 0.062, 78, 'attention: container'),
     ('warn-m', 0.520, 0.105, 78, 'attention: border'),
     ('warn', 0.770, 0.135, 78, 'attention'),
     ('warn-t', 0.840, 0.115, 78, 'attention: text'),
     ('warn-t2', 0.900, 0.085, 78, 'attention: light text')],
    [('bad-d', 0.300, 0.065, 25, 'problem / destructive: container'),
     ('bad-m', 0.480, 0.125, 25, 'problem: fill, border'),
     ('bad', 0.640, 0.150, 25, 'problem'),
     ('bad-t', 0.760, 0.135, 25, 'problem: text'),
     ('bad-t2', 0.870, 0.070, 25, 'problem: light text')],
    [('spec-d', 0.300, 0.085, 300, 'playback / replay: container'),
     ('spec-m', 0.480, 0.130, 300, 'playback: fill'),
     ('spec', 0.640, 0.140, 300, 'playback'),
     ('spec-t', 0.800, 0.100, 300, 'playback: text')],
]
ALL = NEUTRAL + ACCENT + [t for fam in STATES for t in fam]


# ---- OKLCH -> sRGB (Björn Ottosson's OKLab), chroma reduced until inside sRGB ----
def _linear(L, C, h):
    a, b = C * math.cos(math.radians(h)), C * math.sin(math.radians(h))
    l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3
    m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3
    s = (L - 0.0894841775 * a - 1.2914855480 * b) ** 3
    return (4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s)


def oklch_hex(L, C, h):
    def inside(rgb):
        return all(-0.0005 <= x <= 1.0005 for x in rgb)
    rgb = _linear(L, C, h)
    if not inside(rgb):
        lo, hi = 0.0, C
        for _ in range(24):
            mid = (lo + hi) / 2
            if inside(_linear(L, mid, h)):
                lo = mid
            else:
                hi = mid
        rgb = _linear(L, lo, h)
    out = []
    for x in rgb:
        x = min(1.0, max(0.0, x))
        x = 12.92 * x if x <= 0.0031308 else 1.055 * (x ** (1 / 2.4)) - 0.055
        out.append(int(round(255 * min(1.0, max(0.0, x)))))
    return '#%02x%02x%02x' % tuple(out)


def contrast(h1, h2):
    """WCAG contrast ratio of two #rrggbb colours"""
    def lum(h):
        v = []
        for i in (1, 3, 5):
            c = int(h[i:i + 2], 16) / 255.0
            v.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
        return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2]
    a, b = lum(h1), lum(h2)
    if a < b:
        a, b = b, a
    return (a + 0.05) / (b + 0.05)


TOK = {n: oklch_hex(L, C, h) for n, L, C, h, _ in ALL}


def css_block():
    def row(tokens):
        return '  ' + ' '.join('--%s:%s;' % (n, TOK[n]) for n, *_ in tokens)
    out = [':root {',
           '  /* surfaces, darkest first */', row(NEUTRAL[:6]),
           '  /* lines and borders */', row(NEUTRAL[6:11]),
           '  /* text, faintest first */', row(NEUTRAL[11:]),
           '  /* the accent */', row(ACCENT),
           '  /* states: good, attention, problem, playback */']
    out += [row(fam) for fam in STATES]
    out.append('}')
    return '\n'.join(out)


def show_table():
    for n, L, C, h, what in ALL:
        print('--%-8s %s  L=%.3f C=%.3f h=%3.0f  %s' % (n, TOK[n], L, C, h, what))


def show_contrast():
    surf = ('s1', 's2', 's3', 's4')
    print('WCAG contrast on %s (4.5 = body text, 3 = large text and parts of controls)' % ' '.join(surf))
    for n in ('t0', 't1', 't2', 't3', 't4', 't5', 't6', 'ac-t', 'ac-t2', 'ok-t', 'warn-t', 'bad-t', 'spec-t', 'ok', 'warn', 'bad'):
        print('  %-8s %s  %s' % (n, TOK[n], '  '.join('%4.1f' % contrast(TOK[n], TOK[s]) for s in surf)))
    print('white on fills: ' + '  '.join('%s %.1f' % (n, contrast('#ffffff', TOK[n])) for n in ('ac', 'ac-h', 'ok-m', 'bad-m', 'spec-m')))


def check(path):
    text = open(path, encoding='utf-8').read()
    ok = True
    if css_block() not in text:
        print('FAIL: the :root block of %s differs from this palette (--css prints it)' % path)
        ok = False
    start = text.index('/* SDR-style layout */')
    css = text[start:text.index('</style>', start)]
    used = set(re.findall(r'var\(--([a-z0-9-]+)\)', text))
    own = set(re.findall(r'--([a-z0-9-]+)\s*:', text))            # variables the page sets itself (--sb-zoom, ...)
    missing = sorted(u for u in used if u not in TOK and u not in own)
    if missing:
        print('FAIL: var() of undefined names: ' + ', '.join(missing))
        ok = False
    body = css.replace(css_block(), '')
    lits = re.findall(r'(?<![&\w])#(?:[0-9a-fA-F]{6}|[0-9a-fA-F]{3})(?![0-9a-zA-Z_-])', body)
    known = set(TOK.values()) | {'#fff', '#ffffff', '#000', '#000000'}
    stray = [c for c in lits if c.lower() not in known]
    print('%s: %d var(--token) uses, %d colour literals in the CSS that are not tokens (%d distinct)'
          % (os.path.basename(path), len(re.findall(r'var\(--', css)), len(stray), len(set(c.lower() for c in stray))))
    print('OK' if ok else 'FAILED')
    return ok


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        show_table()
    elif a[0] == '--css':
        print(css_block())
    elif a[0] == '--contrast':
        show_contrast()
    elif a[0] == '--check':
        here = os.path.dirname(os.path.abspath(__file__))
        sys.exit(0 if check(a[1] if len(a) > 1 else os.path.join(here, '..', 'kraken_doa.html')) else 1)
    else:
        print(__doc__)
        sys.exit(2)
