#!/usr/bin/env python3
"""Inmarsat-C TDM test signal: NCS common channel or LES TDM frames with real
framing (bulletin board, signalling channel descriptor, EGC double-header
packets, filler packets, idle fill), 16-bit sum checks, the group scrambler,
the rate-1/2 K=7 code, the 64 x 162 interleaver with its unique word, BPSK at
1200 symbols/s through a root-raised-cosine filter (alpha 1). Synthetic
content only.

  inmc_gen.py OUT.cf32 [--rate 48000] [--frames 3] [--start-symbol 3000]
      [--offset HZ] [--drift HZ_PER_S] [--esn0 DB] [--seed N] [--invert]
      [--origin 144] [--frame0 5000] [--lead S]
      [--egc "SERVICE:ADDRESS:LES:MSN:TEXT"] ...   (repeat for more messages)
      [--repeat]     every EGC message is sent a second time after the first
      [--drop SEQ]   leave packet SEQ of the first message out of its first sending

SERVICE is the hex service code (31 = MET / NAVAREA, 00 = all ships, 13 =
coastal, 14 = distress alert to a circular area, 02 = group call ...),
ADDRESS the address bytes in hex (as many as the service code's low digit:
"05" for NAVAREA 5, "" for all ships), LES the decimal LES id ("114"), MSN
the message sequence number. Writes OUT.cf32 + OUT.json (rate). Without
--esn0 the signal is clean (unit symbol energy).
"""
import argparse, json, os, sys
import numpy as np

ROWS, COLS = 64, 162
UW = 0x07EACDDA4E2F28C2
G1, G2 = 0x6D, 0x4F   # bit 0 = newest input (SDM G(1) = 1011011, G(2) = 1111001)


def fletcher(data):
    a = b = 0
    for x in data:
        a = (a + x) & 255
        b = (b + a) & 255
    return [(512 - a - b) & 255, b & 255]


def packet(type_byte, body):
    """A packet: type, (length), body, sum check. Short form when type < 0x80."""
    if type_byte < 0x80:   # length = (type & 0x0F) + 1
        assert len(body) + 2 <= 15
        p = [(type_byte & 0xF0) | (len(body) + 2)] + list(body)
    else:                  # length = second byte + 2
        p = [type_byte, len(body) + 2] + list(body)
    return p + fletcher(p)


def odd_parity(c):
    c &= 0x7F
    return c | (0x80 if bin(c).count("1") % 2 == 0 else 0)


def station(s):
    s = int(s)
    return ((s // 100) << 6) | (s % 100)


def egc_packets(service, address, les, msn, text):
    """Double-header EGC packets: blocks of 256 text bytes, 16 in part 1 (0xB1)
    and the rest in part 2 (0xB2), each part with the same header + its own sum check."""
    body = [odd_parity(ord(c)) for c in text.replace("\n", "\r\n")]
    blocks = [body[i:i + 256] for i in range(0, len(body), 256)] or [[]]
    out = []
    for k, blk in enumerate(blocks):
        seq = k + 1
        more = k < len(blocks) - 1
        flags = (0x80 if more else 0) | 0x40   # C = more packets follow; P set as seen on the air
        parts = [(0xB1, blk[:16]), (0xB2, blk[16:])] if len(blk) > 16 else [(0xB1, blk), (0xB2, [])]
        for t, data in parts:
            hdr = [t, 0, service, flags, (msn >> 8) & 255, msn & 255, seq, 0x00, station(les)] + address
            n = len(hdr) + 2 + len(data) + 2
            hdr[1] = n - 2
            hdr = hdr + fletcher(hdr)
            p = hdr + data
            out.append((seq, p + fletcher(p)))
    return out


def scramble_groups():
    s = [0] * 160
    s[7] = 1
    for i in range(8, 160):
        s[i] = s[i - 4] ^ s[i - 5] ^ s[i - 6] ^ s[i - 8]
    return s


def conv_encode(bits):
    reg = 0
    out = np.zeros(2 * len(bits), dtype=np.uint8)
    for i, b in enumerate(bits):
        reg = ((reg << 1) | int(b)) & 0x7F
        out[2 * i] = bin(reg & G1).count("1") & 1
        out[2 * i + 1] = bin(reg & G2).count("1") & 1
    return out


def frame_symbols(info):
    """639 information bytes -> 10368 symbols (+1 / -1)."""
    assert len(info) == 639
    by = list(info) + [0]
    g = scramble_groups()
    by = [b ^ 0xFF if g[k // 4] else b for k, b in enumerate(by)]
    bits = np.array([(b >> i) & 1 for b in by for i in range(8)], dtype=np.uint8)
    sym = conv_encode(bits)
    M = np.zeros((ROWS, COLS), dtype=np.uint8)
    uwb = [(UW >> (63 - j)) & 1 for j in range(64)]
    for j in range(64):
        i = (j * 39) % 64
        M[i, 0] = M[i, 1] = uwb[j]
    for e in range(len(sym)):
        M[e % 64, 2 + e // 64] = sym[e]
    T = np.array([M[(j * 39) % 64] for j in range(64)]).reshape(-1)
    return 1.0 - 2.0 * T.astype(np.float64)


def rrc(sps, span, beta=1.0):
    t = (np.arange(-span * sps, span * sps + 1)) / sps
    h = np.zeros_like(t)
    for k, x in enumerate(t):
        if abs(x) < 1e-9:
            h[k] = 1 - beta + 4 * beta / np.pi
        elif abs(abs(x) - 1 / (4 * beta)) < 1e-9:
            h[k] = beta / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * beta)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * beta)))
        else:
            h[k] = (np.sin(np.pi * x * (1 - beta)) + 4 * beta * x * np.cos(np.pi * x * (1 + beta))) / (np.pi * x * (1 - (4 * beta * x) ** 2))
    return h / np.sqrt(np.sum(h ** 2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--start-symbol", type=int, default=3000, help="the file starts this many symbols into a frame")
    ap.add_argument("--lead", type=float, default=0.0, help="seconds of noise before the signal")
    ap.add_argument("--offset", type=float, default=0.0)
    ap.add_argument("--drift", type=float, default=0.0)
    ap.add_argument("--esn0", type=float, default=None)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--origin", default="144")
    ap.add_argument("--frame0", type=int, default=5000)
    ap.add_argument("--egc", action="append", default=[])
    ap.add_argument("--repeat", action="store_true")
    ap.add_argument("--drop", type=int, default=0)
    ap.add_argument("--fill", type=int, default=6, help="filler packets per frame")
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    sps = a.rate // 1200
    assert sps * 1200 == a.rate, "rate must be a multiple of 1200"

    # EGC packet queue (in order), possibly repeated
    queue = []
    msgs = []
    for spec in a.egc:
        f = spec.split(":", 4)
        msgs.append(egc_packets(int(f[0], 16), list(bytes.fromhex(f[1])), f[2], int(f[3]), f[4]))
    for mi, pk in enumerate(msgs):
        for seq, p in pk:
            if mi == 0 and a.drop and seq == a.drop:
                continue
            queue.append(p)
    if a.repeat:
        for pk in msgs:
            queue.extend(p for _, p in pk)
    origin = station(a.origin)
    syms = []
    for f in range(a.frames):
        fn = (a.frame0 + f) % 10000
        info = packet(0x7D, [109, fn >> 8, fn & 255, 0x04, 0x08, 0x20, origin, 0xF0, 0xF1, 0x39, 0x0A])
        info += packet(0x6C, [0xB4, 10056 >> 8, 10056 & 255] + [0x02, 0x00, 0x08, 0x00, 0x08, 0x00, 0x02])
        # EGC packets while they fit (at most ~2 messages' worth per frame, as on the air)
        used = 0
        while queue and len(info) + len(queue[0]) <= 600 and used < 6:
            info += queue.pop(0)
            used += 1
        for _ in range(a.fill):
            body = [int(x) for x in rng.integers(0, 256, 3)] + [station(rng.choice(["101", "102", "104", "112"]))] + \
                   [0x2E, 0x46] + [int(x) for x in rng.integers(0, 256, 9)]
            p = packet(0x81, body)
            if len(info) + len(p) <= 639:
                info += p
        info += [0] * (639 - len(info))
        syms.append(frame_symbols(info))
    s = np.concatenate(syms)[a.start_symbol:]
    if a.invert:
        s = -s
    # BPSK, RRC alpha 1
    up = np.zeros(len(s) * sps)
    up[::sps] = s
    h = rrc(sps, 8)
    x = np.convolve(up, h)[8 * sps:8 * sps + len(up)].astype(np.complex128)
    n = len(x)
    t = np.arange(n) / a.rate
    ph = 2 * np.pi * (a.offset * t + 0.5 * a.drift * t * t) + rng.uniform(0, 2 * np.pi)
    x = x * np.exp(1j * ph)
    lead = int(a.lead * a.rate)
    x = np.concatenate([np.zeros(lead, complex), x])
    if a.esn0 is not None:
        # unit symbol energy: Es/N0 = 1 / (sigma^2 / sps) -> sigma^2 = sps / 10^(esn0/10) ... per sample
        p = np.mean(np.abs(x[lead:]) ** 2)
        es = p * sps
        sigma2 = es / 10 ** (a.esn0 / 10)
        x = x + np.sqrt(sigma2 / 2) * (rng.standard_normal(len(x)) + 1j * rng.standard_normal(len(x)))
    x = x / max(1e-9, np.max(np.abs(x))) * 0.5
    x.astype(np.complex64).tofile(a.out)
    with open(os.path.splitext(a.out)[0] + ".json", "w") as fh:
        json.dump({"rate": a.rate, "frames": a.frames, "esn0_db": a.esn0, "offset_hz": a.offset}, fh)
    print("wrote %s: %.2f s at %d Hz, %d frames, %d EGC packets left unsent" % (a.out, len(x) / a.rate, a.rate, a.frames, len(queue)),
          file=sys.stderr)


if __name__ == "__main__":
    main()
