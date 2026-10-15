#!/usr/bin/env python3
"""406 MHz distress beacon test transmissions (Cospas-Sarsat C/S T.001
first-generation beacons): one beacon's bursts as on the air - 160 ms
unmodulated carrier, then the 112-bit (short) or 144-bit (long) message at
400 bit/s, biphase-L, +-1.1 rad phase deviation (150 us transitions), bit
sync (15 ones) + frame sync (000101111 normal / 011010000 self-test), the
protected data fields with their real BCH(82,61) / BCH(38,26) parity.

  beacon_gen.py OUT.cf32 [--rate 100000] [--seconds S] [--offset HZ]
                [--at T ...] [--test-at T ...] [--bit-err PCT] [--seed N]
                (--hex HEX | --protocol NAME [fields])
                [--sgb-at T ...] [--snr DB] [--phase-noise RAD]

--offset: the carrier in Hz from the input's 0 Hz (the beacon406 VFO sits on
406.050 MHz: channel 406.040 MHz = -10000). --at: normal bursts (s),
--test-at: self-test bursts. --hex: bits 25..112 / 25..144 (22 / 30 hex
digits) or the whole message incl. sync (28 / 36). --sgb-at: a burst shaped
like a second-generation beacon (1 s DSSS OQPSK, 38.4 kchip/s, random chips -
only its spectrum is real; for the detector) at --sgb-offset (default 0).

Protocols (--protocol): std-mmsi (EPIRB MMSI) | std-elt24 (ELT 24-bit
address) | std-elt-serial | std-epirb-serial | std-plb-serial | std-ssas |
std-test | nat-elt | nat-epirb | nat-plb | nat-test | rls (--type elt|epirb|
plb|test) | eltdt | user-maritime |
user-callsign | user-aviation | user-serial (--type elt|epirb-ff|epirb|plb|
elt24|elt-op) | user-test | orbitography. Fields: --country MID --mmsi N
--beacon-no N --address HEX24 --cert N --serial N --national N --callsign S
--reg S --op ABC --lat DEG --lon DEG (location protocols / user-location)
--short --homing --internal.

--snr: add noise (per-sample SNR at --rate; 100 kHz: C/N0 = SNR + 50 dBHz) -
leave it out for uca_scene.py, which adds its own. --phase-noise: the
carrier's phase as a random walk, RAD rms per bit (2.5 ms) - a real EPIRB
recording (sigidwiki, 2015) wanders ~0.3 rad per bit.

Output: OUT.cf32 (complex float32) + OUT.json (rate, bursts, messages). Put
several beacons at their own bearings with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

BAUD = 400.0
DEV = 1.1
G1 = 0b1001101101100111100011   # BCH-1, x^21 + ...
G2 = 0b1010100111001            # BCH-2, x^12 + ...
SYNC_NORMAL = '000101111'
SYNC_TEST = '011010000'

# modified Baudot (C/S T.001 Annex A), 6-bit; letters' 5-bit form = without the leading 1
BAUDOT = {'A': '111000', 'B': '110011', 'C': '101110', 'D': '110010', 'E': '110000', 'F': '110110',
          'G': '101011', 'H': '100101', 'I': '101100', 'J': '111010', 'K': '111110', 'L': '101001',
          'M': '100111', 'N': '100110', 'O': '100011', 'P': '101101', 'Q': '111101', 'R': '101010',
          'S': '110100', 'T': '100001', 'U': '111100', 'V': '101111', 'W': '111001', 'X': '110111',
          'Y': '110101', 'Z': '110001', ' ': '100100', '-': '011000', '/': '010111', '0': '001101',
          '1': '011101', '2': '011001', '3': '010000', '4': '001010', '5': '000001', '6': '010101',
          '7': '011100', '8': '001100', '9': '000011'}


def ubits(v, n):
    return format(int(v) & ((1 << n) - 1), '0%db' % n)


def bch(data, gen, ngen):
    """parity of the data bits (string, first bit = highest power)"""
    r = int(data, 2) << ngen
    top = len(data) + ngen
    for i in range(top - 1, ngen - 1, -1):
        if r >> i & 1:
            r ^= gen << (i - ngen)
    return ubits(r, ngen)


def baudot(s, n):
    s = (s.upper() + ' ' * n)[:n]
    return ''.join(BAUDOT.get(c, BAUDOT[' ']) for c in s)


def split(v, step_deg, nb, min_bits):
    """magnitude v (deg) -> coarse count in step_deg steps (nb bits) + offset
    (sign 1 = plus, minutes in min_bits, seconds / 4 in 4 bits) - C/S T.001 A3.3.1"""
    a = abs(v)
    c = int(np.floor(a / step_deg + 0.5))
    rem = (a - c * step_deg) * 3600   # seconds, signed
    sgn = '1' if rem >= 0 else '0'
    r = int(round(abs(rem) / 4)) * 4
    m, sec = divmod(r, 60)
    return ubits(c, nb), sgn + ubits(m, min_bits) + ubits(sec // 4, 4)


def coarse_std(lat, lon):
    """standard location: 1/4 deg coarse (bits 65-85) + offsets (bits 113-132)"""
    if lat is None:
        return '0' + '1' * 9 + '0' + '1' * 10, '1' + '00000' + '1111' + '1' + '00000' + '1111'
    la, lao = split(lat, 0.25, 9, 5)
    lo, loo = split(lon, 0.25, 10, 5)
    return ('1' if lat < 0 else '0') + la + ('1' if lon < 0 else '0') + lo, lao + loo


def coarse_half(lat, lon):
    """RLS / ELT(DT): 1/2 deg coarse (bits 67-85) + offsets (bits 115-132)"""
    if lat is None:
        return '0' + '1' * 8 + '0' + '1' * 9, '1' + '0000' + '1111' + '1' + '0000' + '1111'
    la, lao = split(lat, 0.5, 8, 4)
    lo, loo = split(lon, 0.5, 9, 4)
    return ('1' if lat < 0 else '0') + la + ('1' if lon < 0 else '0') + lo, lao + loo


def coarse_nat(lat, lon):
    """national location: 2-minute coarse (bits 59-85) + offsets (bits 113-126)"""
    if lat is None:
        return '0' + '1111111' + '00000' + '0' + '11111111' + '00000', '1' + '00' + '1111' + '1' + '00' + '1111'
    def one(v, nd):
        a = abs(v)
        c = int(np.floor(a * 30 + 0.5))   # 2' steps
        rem = (a - c / 30) * 3600
        sgn = '1' if rem >= 0 else '0'
        m, sec = divmod(int(round(abs(rem) / 4)) * 4, 60)
        d, mi = divmod(c * 2, 60)
        return ubits(d, nd) + ubits(mi // 2, 5), sgn + ubits(m, 2) + ubits(sec // 4, 4)
    la, lao = one(lat, 7)
    lo, loo = one(lon, 8)
    return ('1' if lat < 0 else '0') + la + ('1' if lon < 0 else '0') + lo, lao + loo


def build(o):
    """bits 25..144 (or ..112) as a string - C/S T.001 Annex A"""
    p = o['protocol']
    cc = ubits(o.get('country', 512), 10)
    lat, lon = o.get('lat'), o.get('lon')
    short = o.get('short', False)
    hom = '1' if o.get('homing', True) else '0'
    src = '1' if o.get('internal', True) else '0'
    if p.startswith('std-') or p.startswith('nat-') or p in ('rls', 'eltdt'):
        short = False
        if p == 'rls':
            bt = {'elt': '00', 'epirb': '01', 'plb': '10', 'test': '11'}[o.get('type', 'plb')]
            ident = bt + ubits(o.get('cert', 123), 10) + ubits(o.get('serial', 1234), 14)
            c1, c2 = coarse_half(lat, lon)
            pdf1 = '1' + '0' + cc + '1101' + ident + c1
            pdf2 = src + hom + '10' + '00' + '01' + c2   # Type-1 RLM accepted, none acknowledged, Galileo
        elif p == 'eltdt':
            ident = '00' + ubits(int(str(o.get('address', 'C81234')), 16), 24)
            c1, c2 = coarse_half(lat, lon)
            fix = '11' if lat is not None else '01'
            pdf1 = '1' + '0' + cc + '1001' + ident + c1
            pdf2 = '01' + ubits(o.get('alt_code', 5), 4) + fix + c2
        else:
            code = {'std-mmsi': '0010', 'std-elt24': '0011', 'std-elt-serial': '0100', 'std-elt-op': '0101',
                    'std-epirb-serial': '0110', 'std-plb-serial': '0111', 'std-ssas': '1100', 'std-test': '1110',
                    'nat-elt': '1000', 'nat-epirb': '1010', 'nat-plb': '1011', 'nat-test': '1111'}[p]
            if p.startswith('std-'):
                if p in ('std-mmsi', 'std-ssas'):
                    ident = ubits(o.get('mmsi', 512000001) % 1000000, 20) + ubits(o.get('beacon_no', 0) if p == 'std-mmsi' else 0, 4)
                elif p == 'std-elt24':
                    ident = ubits(int(str(o.get('address', 'C81234')), 16), 24)
                elif p == 'std-elt-op':
                    ident = ''.join(BAUDOT[c][1:] for c in (o.get('op', 'ANZ') + '   ')[:3]) + ubits(o.get('serial', 1), 9)
                elif p == 'std-test':
                    ident = ubits(o.get('serial', 0x123456), 24)
                else:
                    ident = ubits(o.get('cert', 123), 10) + ubits(o.get('serial', 1234), 14)
                c1, c2 = coarse_std(lat, lon)
                pdf1 = '1' + '0' + cc + code + ident + c1
                pdf2 = '1101' + src + (hom if p != 'std-ssas' else '0') + c2
            else:
                ident = ubits(o.get('national', 12345), 18)
                c1, c2 = coarse_nat(lat, lon)
                pdf1 = '1' + '0' + cc + code + ident + c1
                pdf2 = '110' + '1' + src + hom + c2 + '000000'
        assert len(pdf1) == 61 and len(pdf2) == 26, (len(pdf1), len(pdf2))
    else:
        # user protocols (A2): bits 37-39 code; 84-85 auxiliary radio-locating device (01 = 121.5 MHz)
        aux = '01' if o.get('homing', True) else '00'
        if p == 'user-maritime':
            data = '010' + baudot(('      ' + str(o.get('mmsi', 512000001))[-6:])[-6:], 6) + BAUDOT[str(o.get('beacon_no', 0))[0]] + '00' + aux
        elif p == 'user-callsign':
            cs = (o.get('callsign', 'ZMA1234') + '       ')[:7]
            bcd = ''.join(ubits(int(c), 4) if c.isdigit() else '1010' for c in cs[4:7])
            data = '110' + baudot(cs[:4], 4) + bcd + BAUDOT[str(o.get('beacon_no', 0))[0]] + '00' + aux
        elif p == 'user-aviation':
            reg = ('       ' + o.get('reg', 'ZK-ABC'))[-7:]
            data = '001' + baudot(reg, 7) + ubits(o.get('beacon_no', 0), 2) + aux
        elif p == 'user-serial':
            t = o.get('type', 'plb')
            bt = {'elt': '000', 'epirb-ff': '010', 'epirb': '100', 'plb': '110', 'elt24': '011', 'elt-op': '001'}[t]
            if t == 'elt24':
                rest = ubits(int(str(o.get('address', 'C81234')), 16), 24) + ubits(0, 6)
            elif t == 'elt-op':
                rest = baudot((o.get('op', 'ANZ') + '   ')[:3], 3) + ubits(o.get('serial', 1), 12)
            else:
                rest = ubits(o.get('serial', 1234), 20) + '0000000000'
            data = '011' + bt + '1' + rest + ubits(o.get('cert', 123), 10) + aux
        elif p == 'user-test':
            data = '111' + ubits(o.get('serial', 0x12345), 46)
        elif p == 'orbitography':
            data = '000' + ubits(o.get('serial', 0x5A5A5), 46)
        else:
            raise SystemExit('unknown protocol ' + p)
        assert len(data) == 49, (p, len(data))
        pdf1 = ('0' if short else '1') + '1' + cc + data
        assert len(pdf1) == 61, len(pdf1)
        if short:
            return pdf1 + bch(pdf1, G1, 21) + ubits(o.get('emergency', 0b010000), 6)
        # user-location: position to 4 minutes (A3.3.4.3)
        if lat is None:
            pdf2 = src + '0' + '1111111' + '0000' + '0' + '11111111' + '0000'
        else:
            def ll(v, nd):
                a = abs(v) * 60
                c = int(np.floor(a / 4 + 0.5)) * 4
                d, mi = divmod(c, 60)
                return ubits(d, nd) + ubits(mi // 4, 4)
            pdf2 = src + ('1' if lat < 0 else '0') + ll(lat, 7) + ('1' if lon < 0 else '0') + ll(lon, 8)
    m = pdf1 + bch(pdf1, G1, 21)
    return m + pdf2 + bch(pdf2, G2, 12)


def waveform(bits, rate, bit_err, rng, carrier_s=0.160, phase_noise=0.0):
    """complex baseband of one burst (bits = '0'/'1' string incl. sync);
    phase_noise = random-walk carrier phase, rad rms over one bit (2.5 ms)"""
    T = 1.0 / (BAUD * (1 + bit_err / 100))
    n_c = int(round(carrier_s * rate))
    n = n_c + int(np.ceil(len(bits) * T * rate)) + int(0.002 * rate)
    t = (np.arange(n) - n_c) / rate
    # half-bit levels: '1' = +DEV then -DEV
    m = np.zeros(n)
    on = t >= 0
    k = np.floor(t[on] / T).astype(int)
    k = np.clip(k, 0, len(bits) - 1)
    first = (t[on] - k * T) < T / 2
    b = np.array([1 if c == '1' else -1 for c in bits])[k]
    m[on] = np.where(first, b, -b)
    m[t >= len(bits) * T] = 0
    # 150 us transitions
    w = max(1, int(round(150e-6 * rate)))
    h = np.hanning(w + 2)[1:-1]
    m = np.convolve(m, h / h.sum(), mode='same')
    ph = DEV * m + rng.uniform(0, 2 * np.pi)
    if phase_noise > 0:   # a real recording (sigidwiki EPIRB) wanders ~0.3 rad per bit
        ph += np.cumsum(rng.normal(0, phase_noise / np.sqrt(rate / BAUD), n))
    x = np.exp(1j * ph)
    # 1 ms rise / fall of the carrier
    r = int(0.001 * rate)
    env = np.ones(n)
    env[:r] = np.linspace(0, 1, r)
    end = n_c + int(len(bits) * T * rate)
    env[end:] = 0
    env[max(0, end - r):end] = np.linspace(1, 0, end - max(0, end - r))
    return x * env


def sgb_waveform(rate, rng, dur=1.0, chip=38400.0):
    n = int(dur * rate)
    t = np.arange(n) / rate
    nc = int(dur * chip) + 4
    ci = rng.choice([-1.0, 1.0], nc)
    cq = rng.choice([-1.0, 1.0], nc)
    tc = 1 / chip   # T.018 2.3.3: 38400 chip/s on each of I and Q, Q half a chip later
    i = ci[np.floor(t / tc).astype(int)]
    q = cq[np.floor((t + tc / 2) / tc).astype(int)]   # offset by half a symbol
    # rectangular chips: main lobe +-38.4 kHz (T.018 allows half-sine / RRC shaping, narrower)
    return (i + 1j * q) / np.sqrt(2)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    o = {'protocol': 'std-mmsi'}
    at, test_at, sgb_at = [], [], []
    rate, seconds, offset, bit_err, seed, hexmsg, sgb_off, snr = 100000.0, None, 0.0, 0.0, 1, None, 0.0, None
    phase_noise = 0.0
    i = 1
    while i < len(a):
        k = a[i]
        v = a[i + 1] if i + 1 < len(a) else None
        if k == '--rate': rate = float(v)
        elif k == '--seconds': seconds = float(v)
        elif k == '--offset': offset = float(v)
        elif k == '--at': at.append(float(v))
        elif k == '--test-at': test_at.append(float(v))
        elif k == '--sgb-at': sgb_at.append(float(v))
        elif k == '--sgb-offset': sgb_off = float(v)
        elif k == '--bit-err': bit_err = float(v)
        elif k == '--seed': seed = int(v)
        elif k == '--hex': hexmsg = v
        elif k == '--snr': snr = float(v)
        elif k == '--phase-noise': phase_noise = float(v)
        elif k in ('--short', '--internal-off', '--no-homing'):
            o[{'--short': 'short', '--internal-off': 'internal', '--no-homing': 'homing'}[k]] = k == '--short'
            i += 1
            continue
        elif k in ('--country', '--mmsi', '--beacon-no', '--cert', '--serial', '--national', '--emergency', '--alt-code'):
            o[k[2:].replace('-', '_')] = int(v)
        elif k in ('--lat', '--lon'):
            o[k[2:]] = float(v)
        elif k in ('--protocol', '--address', '--callsign', '--reg', '--op', '--type'):
            o[k[2:]] = v
        else:
            raise SystemExit('unknown option ' + k)
        i += 2
    rng = np.random.default_rng(seed)
    if hexmsg:
        hb = ''.join(format(int(c, 16), '04b') for c in hexmsg.strip())
        if len(hb) in (112, 144):
            hb = hb[24:]
        msg = hb
    else:
        msg = build(o)
    assert len(msg) in (88, 120), len(msg)
    ends = [t + 0.6 for t in at + test_at] + [t + 1.1 for t in sgb_at]
    if seconds is None:
        seconds = max(ends + [1.0]) + 0.5
    N = int(seconds * rate)
    x = np.zeros(N, np.complex128)
    bursts = []
    for t0, test in sorted([(t, False) for t in at] + [(t, True) for t in test_at]):
        bits = '1' * 15 + (SYNC_TEST if test else SYNC_NORMAL) + msg
        w = waveform(bits, rate, bit_err, rng, phase_noise=phase_noise)
        n0 = int(round(t0 * rate))
        w = w[:max(0, N - n0)]
        x[n0:n0 + len(w)] += w * np.exp(2j * np.pi * offset * (np.arange(len(w)) + n0) / rate)
        bursts.append({'at': t0, 'self_test': test, 'mod_at': t0 + 0.160, 'bits': len(bits)})
    for t0 in sgb_at:
        w = sgb_waveform(rate, rng)
        n0 = int(round(t0 * rate))
        w = w[:max(0, N - n0)]
        x[n0:n0 + len(w)] += w * np.exp(2j * np.pi * sgb_off * (np.arange(len(w)) + n0) / rate)
        bursts.append({'at': t0, 'sgb': True})
    if snr is not None:   # noise per sample at --rate, the bursts at power 1
        sig = 10 ** (-snr / 20)
        x = x + (rng.standard_normal(N) + 1j * rng.standard_normal(N)) * sig / np.sqrt(2)
    x.astype(np.complex64).tofile(out)
    hx = format(int(msg, 2), '0%dX' % (len(msg) // 4))
    side = {'rate': rate, 'offset_hz': offset, 'bursts': bursts, 'message_hex': hx,
            'message_bits': len(msg) + 24, 'fields': {k: v for k, v in o.items()} if not hexmsg else {}}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %.1f s, %d bursts, message %s' % (out, seconds, len(bursts), hx))


if __name__ == '__main__':
    main()
