#!/usr/bin/env python3
"""V.23 AFSK telemetry test transmissions - the framing the v23_telemetry
plugin decodes (see its README): NBFM carrier, audio FSK mark 1300 Hz (1) /
space 2100 Hz (0), 1200 bit/s, continuous phase; a key-up lead-in of plain
mark tone; asynchronous characters (start 0, 8 data bits LSB first, parity,
1 or 2 stop bits) sent back to back; packet = FF x N (byte sync), 00, L,
payload, CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over L + payload,
high byte first; L counts the bytes from L itself through the CRC.

  v23_gen.py OUT.cf32 [--rate 48000] [--dev 3500] [--offset HZ] [--lead 0.5]
             [--sync 8] [--format 8O1|8O2|8E1|8N1]
             --packet T HEX [--packet T HEX]...  [--fm T DUR TONE_HZ DEV_HZ]

--packet: one transmission at T s with that payload (hex, without L / CRC).
--fm: an analog FM transmission (a voice-like interferer) - another
transmitter, give it its own output file + bearing. --format and --dev apply
to the packets after them on the command line. One transmitter per file:
complex baseband at --rate (cf32) + OUT.json {rate}; place transmitters at
bearings with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

F_MARK, F_SPACE, BAUD = 1300.0, 2100.0, 1200.0


def crc_ccitt_false(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def packet_bytes(payload, nsync):
    body = bytes([len(payload) + 3]) + payload
    c = crc_ccitt_false(body)
    return b'\xff' * nsync + b'\x00' + body + bytes([c >> 8, c & 0xFF])


def uart_bits(data, fmt):
    par, stops = fmt[1], int(fmt[2])
    bits = []
    for v in data:
        d = [(v >> k) & 1 for k in range(8)]
        bits += [0] + d
        if par == 'O':
            bits.append(1 - (sum(d) & 1))
        elif par == 'E':
            bits.append(sum(d) & 1)
        bits += [1] * stops
    return bits


def afsk_audio(bits, rate, lead_s, tail_s):
    """mark lead-in, the bits, a mark tail - continuous phase"""
    nb = len(bits)
    n_lead, n_tail = int(lead_s * rate), int(tail_s * rate)
    n_bits = int(np.ceil(nb * rate / BAUD))
    t = np.arange(n_bits) / rate
    k = np.minimum((t * BAUD).astype(int), nb - 1)
    f = np.concatenate([np.full(n_lead, F_MARK), np.where(np.asarray(bits)[k] == 1, F_MARK, F_SPACE),
                        np.full(n_tail, F_MARK)])
    return np.sin(2 * np.pi * np.cumsum(f) / rate)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    rate, dev, offset, lead, nsync, fmt = 48000.0, 3500.0, 0.0, 0.5, 8, '8O1'
    bursts = []
    i = 1
    while i < len(a):
        if a[i] == '--rate':
            rate = float(a[i + 1]); i += 2
        elif a[i] == '--dev':
            dev = float(a[i + 1]); i += 2
        elif a[i] == '--offset':
            offset = float(a[i + 1]); i += 2
        elif a[i] == '--lead':
            lead = float(a[i + 1]); i += 2
        elif a[i] == '--sync':
            nsync = int(a[i + 1]); i += 2
        elif a[i] == '--format':
            fmt = a[i + 1].upper(); i += 2
            if len(fmt) != 3 or fmt[0] != '8' or fmt[1] not in 'OEN' or fmt[2] not in '12':
                sys.exit('format: 8O1 / 8O2 / 8E1 / 8E2 / 8N1')
        elif a[i] == '--packet':
            t, payload = float(a[i + 1]), bytes.fromhex(a[i + 2])
            if not 1 <= len(payload) <= 252:
                sys.exit('payload: 1..252 bytes')
            data = packet_bytes(payload, nsync)
            audio = afsk_audio(uart_bits(data, fmt), rate, lead, 0.02)
            bursts.append((t, np.exp(2j * np.pi * dev * np.cumsum(audio) / rate)))
            print('%7.3f s  %s dev %.1f kHz  L=%d: %s  (%.3f s on air)' % (
                t, fmt, dev / 1e3, len(payload) + 3, ' '.join('%02X' % b for b in data), len(audio) / rate))
            i += 3
        elif a[i] == '--fm':
            t, dur, tone, fdev = (float(v) for v in a[i + 1:i + 5])
            n = int(dur * rate)
            tt = np.arange(n) / rate
            audio = np.sin(2 * np.pi * (tone * tt + 0.15 * tone / 3.0 * np.sin(2 * np.pi * 3.0 * tt)))
            audio *= 0.6 + 0.4 * np.sin(2 * np.pi * 1.7 * tt) ** 2
            bursts.append((t, np.exp(2j * np.pi * fdev * np.cumsum(audio) / rate)))
            print('%7.3f s  analog FM %.1f s, %.0f Hz tone, %.0f Hz deviation' % (t, dur, tone, fdev))
            i += 5
        else:
            print('unknown option', a[i]); sys.exit(2)
    end = max((t + len(y) / rate for t, y in bursts), default=0.0)
    n = int((end + 0.1) * rate)
    x = np.zeros(n, np.complex128)
    for t, y in bursts:
        n0 = int(round(t * rate))
        m = min(len(y), n - n0)
        ramp = np.ones(m)
        r = min(int(0.002 * rate), m // 2)
        ramp[:r] = np.linspace(0, 1, r)
        ramp[m - r:] = np.linspace(1, 0, r)
        k = np.arange(m) + n0
        x[n0:n0 + m] += y[:m] * ramp * np.exp(2j * np.pi * offset * k / rate)
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
