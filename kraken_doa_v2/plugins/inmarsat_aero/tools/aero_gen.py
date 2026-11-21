#!/usr/bin/env python3
"""Inmarsat Classic Aero test transmissions, built from ICAO Annex 10 Vol III
Part I chapter 4 (AMSS): signal units with their CRC, scrambler, rate 1/2
K = 7 convolutional code (2/3 punctured on the 8.4 kbit/s C channel), the
64-row interleaver, unique words, A-BPSK / A-QPSK modulation.

  aero_gen.py OUT.cf32 p [--rate 600|1200|2400|4800|10500] [--seconds S]
              [--script ITEM ...]                 P channel (GES -> aircraft)
  aero_gen.py OUT.cf32 burst --rate 600|1200|10500 --at T ITEM [--at T ITEM]...
                                                  R / T channel bursts of one AES
  aero_gen.py OUT.cf32 c [--seconds S] [--aes HEX] C channel, 8.4 kbit/s (voice
                                                  field: --voice FILE of 12-octet
                                                  frames, else random bits)
common: [--out-rate 24000] [--offset HZ] [--ebn0 DB] [--seed N] [--start S]
        [--phase-noise HZ]

ITEMs (P channel --script, at frame boundaries in order; burst: one per --at):
  acars:AES:REG:LABEL:BID:TEXT      ACARS block in user data (P: uplink;
                                    burst: downlink, its text "M01A" + flight)
  adsc:AES:REG:LAT:LON:ALTFT[:FLT]  ADS-C basic report (+ earth ref, flight
                                    ID) in an ACARS B6 downlink (bursts only)
  logon:AES                         P: log-on confirm; burst: log-on request (R)
  cass:AES:RXCODE:TXCODE            C channel assignment (P)
  fill                              a frame of fill-in only
Output: cf32 at --out-rate + OUT.json {rate}. Content is synthetic.
"""
import argparse
import json
import sys

import numpy as np
from scipy import signal

UW_P = [int(c) for c in "11100001010110101110100010010011"]
UW_C84_I = [int(c) for c in "1010101100110111011010010011100010111100101000110000"]
UW_C84_Q = [int(c) for c in "0000110001010011110100011100100101101110110011010101"]
G1 = [0, 2, 3, 5, 6]
G2 = [0, 1, 2, 3, 6]
GES = 0x44   # a synthetic GES ID


# ---------------------------------------------------------------- coding
def crc_x25(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c ^ 0xFFFF


def su(octets, n=12):
    b = bytearray(octets[:n - 2])
    b += bytes(n - 2 - len(b))
    c = crc_x25(b)
    return bytes(b) + bytes([c & 0xFF, c >> 8])


def bits_lsb(data):
    return [(o >> k) & 1 for o in data for k in range(8)]


def scramble(bits):
    st = [1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1]
    out = []
    for b in bits:
        fb = st[0] ^ st[14]
        out.append(b ^ fb)
        st = [fb] + st[:14]
    return out


class Conv:
    def __init__(self):
        self.reg = [0] * 7          # reg[k] = input delayed k

    def push(self, b):
        self.reg = [b] + self.reg[:6]
        return (sum(self.reg[k] for k in G1) & 1, sum(self.reg[k] for k in G2) & 1)

    def encode(self, bits):
        out = []
        for b in bits:
            out.extend(self.push(b))
        return out


def interleave(coded, cols):
    """coded (64 * cols) -> transmission order"""
    m = np.array(coded, dtype=np.int8).reshape(cols, 64).T     # m[row, col] = coded[col*64 + row]
    out = np.zeros_like(m)
    for i in range(64):
        out[(i * 27) % 64] = m[i]
    return list(out.reshape(-1))


# ---------------------------------------------------------------- ACARS / ADS-C
def odd(c):
    c &= 0x7F
    return c | (0 if bin(c).count("1") & 1 else 0x80)


def kermit(p):
    c = 0
    for b in p:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c


def acars_block(reg, label, bid, text, mode="2"):
    addr = reg.rjust(7, ".")[-7:]
    body = [odd(ord(mode))] + [odd(ord(ch)) for ch in addr] + [odd(0x15)] + [odd(ord(label[0])), odd(ord(label[1]))]
    body += [odd(ord(bid))]
    if text:
        body += [odd(0x02)] + [odd(ord(ch)) for ch in text]
    body += [odd(0x03)]
    bcs = kermit(body)
    return bytes([0xFF, 0xFF, odd(0x01)] + body + [bcs & 0xFF, bcs >> 8, 0x7F])


ARINC_T = []
for _i in range(256):
    _v = _i << 8
    for _ in range(8):
        _v = ((_v << 1) ^ 0x1021) & 0xFFFF if _v & 0x8000 else (_v << 1) & 0xFFFF
    ARINC_T.append(_v)


def arinc_crc(p, c=0xFFFF):
    for b in p:
        c = (c >> 8) ^ ARINC_T[(c ^ b) & 0xFF]
    return c


def arinc622(gs, imi, reg, binary):
    regf = reg.rjust(7, ".")[-7:]
    head = (imi + regf).encode()
    for x in range(65536):
        tail = bytes([x >> 8, x & 0xFF])
        if arinc_crc(head + binary + tail) == 0x1D0F:
            break
    return "/" + gs + "." + imi + regf + (binary + tail).hex().upper()


def bitpack(fields):
    v, n = 0, 0
    for val, w in fields:
        v = (v << w) | (int(val) & ((1 << w) - 1))
        n += w
    return v.to_bytes(n // 8, "big")


def coord21(deg):
    return int(round(deg * 1048575.0 / (180.0 - 90.0 / 524288.0)))


def adsc_report(lat, lon, alt_ft, flight="", track=90.0, gs_kt=450.0, t_s=123.0):
    b = bytes([7]) + bitpack([(coord21(lat), 21), (coord21(lon), 21), (int(alt_ft / 4), 16), (int(t_s / 0.125), 15), (2 << 1, 7)])
    trk = int(round((track if track <= 180 else track - 360) * 2047.0 / (180.0 - 90.0 / 1024.0)))
    b += bytes([14]) + bitpack([(0, 1), (trk, 12), (int(gs_kt * 2), 13), (0, 12)])
    if flight:
        ch = []
        for c in flight.ljust(8)[:8]:
            o = ord(c)
            ch.append((o - 0x40) if 0x41 <= o <= 0x5A else o)
        b += bytes([12]) + bitpack([(c, 6) for c in ch])
    return b


# ---------------------------------------------------------------- signal units
def aes3(a):
    return [(a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF]


def user_data_sus(aes, data, q=7, ref=3):
    """P / T channel user data: ISU (0x71) + SSUs, sequence numbers counting down"""
    first, rest = data[:2], data[2:]
    chunks = [rest[i:i + 8] for i in range(0, len(rest), 8)]
    nss = len(chunks)
    last = len(chunks[-1]) if chunks else len(first)
    out = [su([0x71] + aes3(aes) + [GES, (q << 4) | ref, nss & 0x3F, (last & 15) << 4] + list(first.ljust(2, b"\0")))]
    for i, c in enumerate(chunks):
        seq = nss - 1 - i
        out.append(su([0xC0 | seq, (q << 4) | ref] + list(c.ljust(8, b"\0"))))
    return out


def fill_su():
    return su([0x01])


def broadcast_sus(frame):
    """the system table broadcasts a GES repeats"""
    k = frame % 4
    if k == 0:
        return [su([0x0A, 5, 0, 0, 0, 0, 0, 0, 0, 0])]
    if k == 1:
        return [su([0x05, 5, (3 << 2) | 0, GES, 0x8F, 0x12, 0x0B, 0x50, 0x0B, 0x54])]
    if k == 2:
        return [su([0x0C, 5, (3 << 2) | 1, (6 << 4) | (2 << 1), 0x20, 36, 0x0F, 0x12, 0x0F, 0x16])]
    return [su([0x09, GES, 0x70, 0x20, 0x26, 0x11, 0x22, 0x12, 0x34, 0x56])]


def item_sus(item, burst=False):
    p = item.split(":")
    k = p[0]
    if k == "acars":
        aes, reg, label, bid, text = int(p[1], 16), p[2], p[3], p[4], ":".join(p[5:])
        if burst:
            text = "M01A" + "XY0123" + text
        return user_data_sus(aes, acars_block(reg, label, bid, text))
    if k == "adsc":
        aes, reg, lat, lon, alt = int(p[1], 16), p[2], float(p[3]), float(p[4]), float(p[5])
        flt = p[6] if len(p) > 6 else ""
        text = arinc622("BOMCAYA", "ADS", reg, adsc_report(lat, lon, alt, flt))
        return user_data_sus(aes, acars_block(reg, "B6", "3", text))
    if k == "logon":
        aes = int(p[1], 16)
        return [su([0x11] + aes3(aes) + [GES, (13 << 4) | 0x6, 0x0C, 0x10, (2 << 6) | 3, 1])]
    if k == "cass":
        aes = int(p[1], 16)
        rx, tx = int(p[2], 16), int(p[3], 16)
        return [su([0x33] + aes3(aes) + [GES, 0x52, rx >> 8, rx & 0xFF, tx >> 8, tx & 0xFF])]
    if k == "fill":
        return []
    raise SystemExit("unknown item " + item)


# ---------------------------------------------------------------- P channel
P_RATES = {600: (32, 0, 3, 6, 6), 1200: (32, 0, 2, 9, 6), 2400: (32, 0, 1, 18, 6),
           4800: (64, 16, 1, 36, 12), 10500: (64, 178, 1, 78, 26)}


def p_channel(rate, seconds, script):
    uw, dummy, blocks, cols, nsu = P_RATES[rate]
    fpsf = {600: 4, 1200: 8}.get(rate, 16)
    frame_bits = uw + 16 + dummy + blocks * 64 * cols
    nframes = int(np.ceil(seconds * rate / frame_bits))
    items = [item_sus(it) for it in script]
    first = int(np.ceil(5.0 * rate / frame_bits))   # from 5 s on (after acquisition), then repeated
    queue = []
    enc = Conv()
    bits = []
    for f in range(nframes):
        fn = f % fpsf
        sus = broadcast_sus(f)
        if f >= first and not queue:
            queue = list(items)
        if queue and len(queue[0]) > nsu - 1:   # longer than a frame: across frames
            head = queue.pop(0)
            queue = [head[:nsu - 1], head[nsu - 1:]] + queue if len(head) > nsu - 1 else queue
        while queue and len(sus) + len(queue[0]) <= nsu:
            sus += queue.pop(0)
        while len(sus) < nsu:
            sus.append(fill_su())
        info = scramble([b for s in sus for b in bits_lsb(s)])
        coded = enc.encode(info)
        uwb = [b for b in UW_P for _ in range(2)] if uw == 64 else UW_P
        hdr = [0, 0, 0, 1] + ([1, 1, 1, 1] if fn == 0 else [0, 0, 0, 0]) + [(fn >> (3 - i)) & 1 for i in range(4)] * 2
        dm = ([0, 0, 0, 1] * 60)[:dummy]
        fr = uwb + hdr + dm
        for b in range(blocks):
            fr += interleave(coded[b * 64 * cols:(b + 1) * 64 * cols], cols)
        bits += fr
    return bits


# ---------------------------------------------------------------- C channel 8.4 kbit/s
def c_channel(seconds, aes, voice, rng):
    """frames of 500 ms: UW (52 on I, 52 on Q) + 16 blocks of 64 x 4 (4096 coded bits)"""
    nframes = int(np.ceil(seconds * 2))
    enc = Conv()
    out = []
    vi = 0
    for f in range(nframes):
        # sub-band: 24 fields of 12 bits = 3 SUs per frame (call progress / fill-in)
        if f % 4 == 0:
            sb_sus = [su([0x30] + aes3(aes) + [GES, (13 << 4) | 1, 0x01, 0, 0, 0]), fill_su(), fill_su()]
        else:
            sb_sus = [fill_su(), fill_su(), fill_su()]
        sb = [b for s in sb_sus for b in bits_lsb(s)]
        info = []
        for k in range(25):
            info.append(1)   # reference bit
            if voice is not None:
                fr = voice[vi % len(voice)]
                vi += 1
                info += bits_lsb(fr)
            else:
                info += list(rng.integers(0, 2, 96))
            if k < 24:
                info += sb[12 * k:12 * k + 12]
        info.append(1)
        assert len(info) == 2714, len(info)
        info += [0] * 16                       # d2 dummy bits
        sc = scramble(info)
        coded = []
        for i in range(0, len(sc), 2):         # punctured 2/3: G1 G2 G1
            a = enc.push(sc[i])
            b = enc.push(sc[i + 1])
            coded += [a[0], a[1], b[0]]
        coded.append(1)                        # d3
        assert len(coded) == 4096
        fr = []
        for i in range(52):
            fr += [UW_C84_I[i], UW_C84_Q[i]]
        for b in range(16):
            fr += interleave(coded[b * 256:(b + 1) * 256], 4)
        out += fr
    return out


# ---------------------------------------------------------------- R / T bursts
PREAMBLE = {600: (150, 74), 1200: (126, 74), 2400: (78, 74), 10500: (248, 256)}


def burst_bits(rate, item):
    """(info bits before coding incl. flush, block plan, kind)"""
    p = item.split(":")
    if p[0] == "logon":   # R channel log-on request (A2-2): 19-octet SU
        aes = int(p[1], 16)
        s = su([(1 << 4) | 0xF, (13 << 4) | 0, 0x10] + aes3(aes) + [GES, 0x10, 0x0C, 0x10, 0x02, 0xFF, 0x30], 19)
        info = scramble(bits_lsb(s)) + [0] * 8
        return info, [5], "R"
    sus = item_sus(item, burst=True)
    aes = int(p[1], 16)
    bid = bytearray(aes3(aes) + [GES])
    c = crc_x25(bid)
    bid += bytes([c & 0xFF, c >> 8])
    info = scramble(bits_lsb(bid) + [b for s in sus for b in bits_lsb(s)]) + [0] * 16
    n = len(sus)
    if rate == 10500:
        plan = [2 + 3 * n]
    else:
        plan = [5] + [3] * (n - 1)
    return info, plan, "T"


def burst(rate, item):
    info, plan, kind = burst_bits(rate, item)
    enc = Conv()
    coded = enc.encode(info)
    assert len(coded) == 64 * sum(plan), (len(coded), plan)
    unmod, modn = PREAMBLE[rate]
    out = []
    pos = 0
    for cols in plan:
        out += interleave(coded[pos:pos + 64 * cols], cols)
        pos += 64 * cols
    uw = [b for b in UW_P for _ in range(2)] if rate == 10500 else UW_P
    if rate == 10500:
        pre = [0] * unmod + [(i & 1) if i % 2 == 0 else 0 for i in range(modn)]
        # A-QPSK preamble: I alternates 0 1 0 1 ..., Q stays 0 (bits I Q I Q ...)
        pre = [0] * unmod + [((i // 2) & 1) if i % 2 == 0 else 0 for i in range(modn)]
        return pre, uw + out, unmod
    pre = [i & 1 for i in range(modn)]
    return [], pre + uw + out, unmod


# ---------------------------------------------------------------- modulation
def rrc(sps, span, beta):
    t = np.arange(-span * sps, span * sps + 1) / sps
    with np.errstate(all="ignore"):
        h = (np.sin(np.pi * t * (1 - beta)) + 4 * beta * t * np.cos(np.pi * t * (1 + beta))) / (np.pi * t * (1 - (4 * beta * t) ** 2))
    h[len(h) // 2] = 1 - beta + 4 * beta / np.pi
    bad = ~np.isfinite(h)
    if bad.any():
        tb = 1 / (4 * beta)
        h[bad] = beta / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi * tb) + (1 - 2 / np.pi) * np.cos(np.pi * tb))
    return h / np.sqrt(np.sum(h * h))


def mod_abpsk(bits, sps, beta=1.0, phase0=0.0, unmod=0):
    """A-BPSK: "1" +90 deg, "0" -90 deg per bit; unmod = unmodulated bit periods first"""
    steps = np.where(np.array(bits) > 0, np.pi / 2, -np.pi / 2)
    ph = phase0 + np.concatenate([[0] * unmod, np.cumsum(steps)])
    sym = np.exp(1j * ph)
    up = np.zeros(len(sym) * sps, dtype=complex)
    up[::sps] = sym
    return np.convolve(up, rrc(sps, 6, beta), mode="full")[6 * sps:6 * sps + len(up)]


def mod_aqpsk(bits, sps, beta=0.6, unmod=0):
    """A-QPSK: bits alternate I (0 -> +1) and Q (0 -> +j), Q one bit late; sps per BIT (even)"""
    b = np.array(bits, dtype=float)
    if len(b) % 2:
        b = np.append(b, 0)
    i = 1 - 2 * b[0::2]
    q = 1 - 2 * b[1::2]
    if unmod:
        i = np.concatenate([np.ones(unmod // 2), i])
        q = np.concatenate([np.ones(unmod // 2), q])
    n = len(i) * 2 * sps + 2 * sps
    ui = np.zeros(n)
    uq = np.zeros(n)
    ui[0:len(i) * 2 * sps:2 * sps] = i
    uq[sps:sps + len(q) * 2 * sps:2 * sps] = q
    h = rrc(2 * sps, 6, beta)
    s = np.convolve(ui + 1j * uq, h, mode="full")[12 * sps:12 * sps + n]
    return s


def to_rate(x, fs_in, fs_out):
    g = int(np.gcd(int(fs_in), int(fs_out)))
    return signal.resample_poly(x, int(fs_out) // g, int(fs_in) // g)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("kind", choices=["p", "burst", "c"])
    ap.add_argument("--rate", type=int, default=10500)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--script", nargs="*", default=[])
    ap.add_argument("--at", nargs=2, action="append", default=[], metavar=("T", "ITEM"))
    ap.add_argument("--aes", default="ABC123")
    ap.add_argument("--voice", default=None)
    ap.add_argument("--out-rate", type=int, default=24000)
    ap.add_argument("--offset", type=float, default=0.0)
    ap.add_argument("--ebn0", type=float, default=None)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--start", type=float, default=0.3, help="P / C: silence before the signal (s)")
    ap.add_argument("--phase-noise", type=float, default=0.0, help="random-walk phase noise, Hz rms of the frequency")
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    fs_out = a.out_rate
    rate = 8400 if a.kind == "c" else a.rate
    qpsk = rate >= 4800
    sps = int(np.ceil(96000 / rate))
    if qpsk and sps % 2:
        sps += 1
    fs_gen = rate * sps
    if a.kind == "p":
        bits = p_channel(rate, a.seconds, a.script)
        x = mod_aqpsk(bits, sps) if qpsk else mod_abpsk(bits, sps)
        lead = int(a.start * fs_gen)
        x = np.concatenate([np.zeros(lead, complex), x])
        total = len(x)
    elif a.kind == "c":
        voice = None
        if a.voice:
            raw = open(a.voice, "rb").read()
            voice = [raw[i:i + 12] for i in range(0, len(raw) - 11, 12)]
        bits = c_channel(a.seconds, int(a.aes, 16), voice, rng)
        x = mod_aqpsk(bits, sps)
        lead = int(a.start * fs_gen)
        x = np.concatenate([np.zeros(lead, complex), x])
        total = len(x)
    else:
        events = sorted((float(t), it) for t, it in a.at)
        total = int((max(t for t, _ in events) + 3.0) * fs_gen)
        x = np.zeros(total, complex)
        for t, it in events:
            pre, bits, unmod = burst(rate, it)
            if qpsk:
                s = mod_aqpsk(pre + bits, sps, unmod=0)
            else:
                s = mod_abpsk(bits, sps, unmod=unmod, phase0=rng.uniform(0, 2 * np.pi))
            s = s * np.exp(1j * rng.uniform(0, 2 * np.pi))
            i0 = int(t * fs_gen)
            x[i0:i0 + len(s)] += s[:max(0, min(len(s), total - i0))]
    y = to_rate(x, fs_gen, fs_out)
    n = np.arange(len(y))
    ph = 2 * np.pi * a.offset * n / fs_out
    if a.phase_noise > 0:
        ph += np.cumsum(rng.normal(0, 2 * np.pi * a.phase_noise / fs_out, len(y)))
    y = y * np.exp(1j * ph)
    p = np.mean(np.abs(y[np.abs(y) > 1e-3]) ** 2) if np.any(np.abs(y) > 1e-3) else 1.0
    y = y / np.sqrt(p) * 0.5
    if a.ebn0 is not None:
        # Eb/N0 per channel bit: N0 = Eb / 10^(ebn0/10), Eb = P / rate; noise power over fs_out = N0 * fs_out
        n0 = 0.25 / rate / (10 ** (a.ebn0 / 10))
        sig = np.sqrt(n0 * fs_out / 2)
        y = y + sig * (rng.standard_normal(len(y)) + 1j * rng.standard_normal(len(y)))
    y.astype(np.complex64).tofile(a.out)
    with open(a.out.rsplit(".", 1)[0] + ".json", "w") as f:
        json.dump({"rate": fs_out, "sample_rate": fs_out}, f)
    print("%s: %.2f s at %d Hz" % (a.out, len(y) / fs_out, fs_out), file=sys.stderr)


if __name__ == "__main__":
    main()
