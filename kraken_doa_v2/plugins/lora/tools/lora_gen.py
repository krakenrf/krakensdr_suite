#!/usr/bin/env python3
"""Synthetic LoRa / Meshtastic / LoRaWAN packets at known bearings on
kraken_doa's UCA, at 2.4 MHz - test signals for the lora plugin (CSS demod,
the coding chain, Meshtastic default-key decrypt, LoRaWAN headers, ID
separation and channel filtering).

  lora_gen.py SCENE.json OUT.cu8|OUT.cf32

Every antenna, sample by sample (antenna 0 first), written in 0.1 s pieces;
OUT.json gets rate, channels, rf_hz, radius_m and the truth (each
transmitter's bearing and the packets it sent).

The LoRa PHY coding chain here MIRRORS plugins/lora/coding.hpp exactly
(Gray, diagonal interleave, Hamming, whitening, CRC16, explicit header) and
the CSS modulation mirrors lora_phy - decoder and generator are a verified
round trip. Clean-room from the public LoRa reverse-engineering literature.
Meshtastic packets are encrypted AES-128-CTR with the published default key
(d4f1bb3a20290759f0bcffabcf4e6901); a "private" channel uses a random key
(the plugin then shows only the clear header - as it must).

SCENE (JSON):
{
  "rf_hz": 921.8e6,  "seconds": 6, "channels": 5, "radius_m": 0.12,
  "seed": 1, "noise_lsb": 2.0, "station": [-41.0, 174.0],
  "sources": [
    {"proto": "meshtastic", "name": "!a1b2c3d4", "bearing": 60,
     "sf": 11, "bw": 250000, "cr": 1, "freq_mhz": 921.8, "level_db": 14,
     "period_s": 2.0, "t0": 0.4,
     "from": 2712847316, "to": 4294967295, "id": 123456789,
     "hop_limit": 3, "hop_start": 3, "channel": "LongFast",
     "port": "text", "text": "hello mesh", "lat": null, "lon": null},
    {"proto": "meshtastic_relay", "name": "!deadbeef", "bearing": 300,
     "relay_of": "!a1b2c3d4", "relay_node_last": 222, ...},
    {"proto": "lorawan", "name": "DevAddr 26011F88", "bearing": 200,
     "sf": 9, "bw": 125000, "cr": 1, "freq_mhz": 922.0,
     "devaddr": "0x26011F88", "fcnt": 42, "fport": 1, "mtype": "unconf_up"},
    {"proto": "lora", "name": "raw", "text": "...", ...}
  ]
}
Per source also: "preamble" (symbols; default 16 for Meshtastic, 8 else),
"sync_word" (default 0x2B Meshtastic, 0x34 LoRaWAN, 0x12 else), "df_hz"
(carrier offset), "crc". IDs (from / to / id / devaddr / deveui ...) may be
numbers or hex strings ("0x26011F88"); random bytes (private keys, LoRaWAN
FRMPayload / MIC) come from the scene's seed - reproducible scenes.
The waveform has continuous phase (exact symbol timing at any rate).

Bearings as the MUSIC DoA plot shows them (CCW from ANT0); antennas on
kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE (as uca_scene.py).

Test:  plugins/lora/build/decoder --file OUT.cu8 --channels 5 --uca 0.12 --rf 921.8e6
"""
import json
import math
import os
import sys

import numpy as np

FS = 2.4e6
C = 299792458.0
DEFAULT_KEY = bytes.fromhex("d4f1bb3a20290759f0bcffabcf4e6901")

# ---------------------------------------------------------------- coding chain
# The radios' conventions (checked on real AU915 gateway downlinks - see
# plugins/lora/coding.hpp, which this mirrors): inverse-Gray mapped values,
# diagonal interleave cw[k] bit (cw_len-1-i) -> bit ((k - i) mod sf_app) of
# symbol i, codewords d0 d1 d2 d3 p.. (first symbol first), LFSR whitening
# 0xFF / (w << 1) | (w7^w5^w4^w3) over the payload only, CRC16 of the first
# n-2 bytes XOR the last two, the header checksum of the SX127x; data symbols
# sent as value + 1, reduced-rate ones as 4 value + 1.
def gray_unmap(g):
    m = g >> 1
    while m:
        g ^= m
        m >>= 1
    return g


def hamming_encode(data, cr):
    d0, d1, d2, d3 = data & 1, (data >> 1) & 1, (data >> 2) & 1, (data >> 3) & 1
    if cr == 1:
        return d0 << 4 | d1 << 3 | d2 << 2 | d3 << 1 | (d0 ^ d1 ^ d2 ^ d3)
    p0, p1, p2, p3 = d0 ^ d1 ^ d2, d1 ^ d2 ^ d3, d0 ^ d1 ^ d3, d0 ^ d2 ^ d3
    full = d0 << 7 | d1 << 6 | d2 << 5 | d3 << 4 | p0 << 3 | p1 << 2 | p2 << 1 | p3
    return full >> (4 - cr)


def interleave(cw, cr, sf_app):
    n = 4 + cr
    sym = [0] * n
    for i in range(n):
        for k in range(sf_app):
            sym[i] |= ((cw[k] >> (n - 1 - i)) & 1) << ((k - i) % sf_app)
    return sym


class Whitening:
    def __init__(self):
        self.s = 0xFF

    def nxt(self):
        out = self.s
        fb = ((self.s >> 7) ^ (self.s >> 5) ^ (self.s >> 4) ^ (self.s >> 3)) & 1
        self.s = ((self.s << 1) & 0xFF) | fb
        return out


def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if (c & 0x8000) else (c << 1) & 0xFFFF
    return c


def payload_crc(p):
    if len(p) == 0:
        return 0
    if len(p) == 1:
        return p[0]
    return crc16(p[:-2]) ^ p[-1] ^ (p[-2] << 8)


def header_checksum(n0, n1, n2):
    b = [(n0 >> 3) & 1, (n0 >> 2) & 1, (n0 >> 1) & 1, n0 & 1,
         (n1 >> 3) & 1, (n1 >> 2) & 1, (n1 >> 1) & 1, n1 & 1,
         (n2 >> 3) & 1, (n2 >> 2) & 1, (n2 >> 1) & 1, n2 & 1]
    c4 = b[0] ^ b[1] ^ b[2] ^ b[3]
    c3 = b[0] ^ b[4] ^ b[5] ^ b[6] ^ b[11]
    c2 = b[1] ^ b[4] ^ b[7] ^ b[8] ^ b[10]
    c1 = b[2] ^ b[5] ^ b[7] ^ b[9] ^ b[10] ^ b[11]
    c0 = b[3] ^ b[6] ^ b[8] ^ b[9] ^ b[10] ^ b[11]
    return (c4 << 4) | (c3 << 3) | (c2 << 2) | (c1 << 1) | c0


def lora_symbols(payload, sf, cr, has_crc, ldro):
    N = 1 << sf
    data = bytearray(payload)
    wh = Whitening()
    for i in range(len(payload)):
        data[i] ^= wh.nxt()
    if has_crc:
        crc = payload_crc(bytes(payload))
        data += bytes([crc & 0xFF, crc >> 8])
    nib = []
    for b in data:
        nib.append(b & 0xF)
        nib.append(b >> 4)
    plen = len(payload)
    h0, h1, h2 = (plen >> 4) & 0xF, plen & 0xF, ((cr << 1) | (1 if has_crc else 0))
    chk = header_checksum(h0, h1, h2)
    hn = [h0, h1, h2, (chk >> 4) & 1, chk & 0xF]

    syms = []
    sfh = sf - 2
    hblk = list(hn)
    rest = list(nib)
    for _ in range(max(0, sfh - 5)):
        if rest:
            hblk.append(rest.pop(0))
    while len(hblk) < sfh:
        hblk.append(0)
    cw = [hamming_encode(hblk[j], 4) for j in range(sfh)]
    for v in interleave(cw, 4, sfh):
        syms.append((4 * gray_unmap(v) + 1) % N)
    sfa = sf - 2 if ldro else sf
    while rest:
        blk = [rest.pop(0) if rest else 0 for _ in range(sfa)]
        cw = [hamming_encode(blk[j], cr) for j in range(sfa)]
        for v in interleave(cw, cr, sfa):
            syms.append((4 * gray_unmap(v) + 1) % N if ldro else (gray_unmap(v) + 1) % N)
    return syms


def lora_waveform(payload, sf, bw, cr, has_crc, sync_word, fs=FS, preamble=8, inverted=False):
    """Full CSS packet at fs with CONTINUOUS phase (exact symbol timing):
    8 preamble up-chirps, 2 sync symbols, 2.25 SFD down-chirps, data symbols.
    inverted: the whole packet conjugated (LoRaWAN downlinks, inverted IQ)."""
    N = 1 << sf
    ldro = (N / bw) >= 0.016
    syms = lora_symbols(payload, sf, cr, has_crc, ldro)
    Ts = N / bw
    rate = bw / Ts
    segs = [(1, 0, 1.0)] * int(preamble)
    segs += [(1, ((sync_word >> 4) & 0xF) * 8, 1.0), (1, (sync_word & 0xF) * 8, 1.0)]
    segs += [(-1, 0, 2.25)]
    segs += [(1, v, 1.0) for v in syms]
    durs = np.array([s[2] for s in segs]) * Ts
    seg_t0 = np.concatenate([[0.0], np.cumsum(durs)])
    total_T = seg_t0[-1]
    Nout = int(math.floor(total_T * fs))
    t = np.arange(Nout) / fs
    si = np.clip(np.searchsorted(seg_t0, t, side="right") - 1, 0, len(segs) - 1)
    sign = np.array([s[0] for s in segs], float)[si]
    val = np.array([s[1] for s in segs], float)[si]
    tau = t - seg_t0[si]
    up = ((val * bw / N) + rate * tau) % bw - bw / 2
    dn = bw / 2 - (rate * tau % bw)
    f = np.where(sign > 0, up, dn)
    phase = np.cumsum(2 * np.pi * f / fs)
    if inverted:
        phase = -phase
    return np.exp(1j * phase), len(syms)




# ---------------------------------------------------------------- AES-128-CTR
def _aes_expand(key):
    sbox = _SBOX
    rk = bytearray(key)
    rcon = [1, 2, 4, 8, 16, 32, 64, 128, 0x1b, 0x36]
    r = 0
    i = 16
    while i < 176:
        t = bytearray(rk[i - 4:i])
        if i % 16 == 0:
            t = bytearray([sbox[t[1]] ^ rcon[r], sbox[t[2]], sbox[t[3]], sbox[t[0]]])
            r += 1
        for j in range(4):
            rk.append(rk[i - 16 + j] ^ t[j])
        i += 4
    return rk


def _xtime(x):
    return ((x << 1) ^ 0x1b) & 0xFF if (x & 0x80) else (x << 1) & 0xFF


def _aes_enc_block(rk, s):
    s = bytearray(s)
    sbox = _SBOX
    for i in range(16):
        s[i] ^= rk[i]
    for rnd in range(1, 11):
        for i in range(16):
            s[i] = sbox[s[i]]
        s[1], s[5], s[9], s[13] = s[5], s[9], s[13], s[1]
        s[2], s[6], s[10], s[14] = s[10], s[14], s[2], s[6]
        s[3], s[7], s[11], s[15] = s[15], s[3], s[7], s[11]
        if rnd < 10:
            for c in range(0, 16, 4):
                a0, a1, a2, a3 = s[c], s[c + 1], s[c + 2], s[c + 3]
                x = a0 ^ a1 ^ a2 ^ a3
                s[c] ^= x ^ _xtime(a0 ^ a1)
                s[c + 1] ^= x ^ _xtime(a1 ^ a2)
                s[c + 2] ^= x ^ _xtime(a2 ^ a3)
                s[c + 3] ^= x ^ _xtime(a3 ^ a0)
        for i in range(16):
            s[i] ^= rk[rnd * 16 + i]
    return bytes(s)


def aes_ctr(key, nonce, data):
    rk = _aes_expand(key)
    out = bytearray(data)
    ctr = bytearray(nonce)
    off = 0
    while off < len(data):
        ks = _aes_enc_block(rk, ctr)
        for i in range(min(16, len(data) - off)):
            out[off + i] ^= ks[i]
        for i in range(12, 16):
            ctr[i] = (ctr[i] + 1) & 0xFF
            if ctr[i]:
                break
        off += 16
    return bytes(out)


_SBOX = bytes.fromhex(
    "637c777bf26b6fc53001672bfed7ab76ca82c97dfa5947f0add4a2af9ca472c0"
    "b7fd9326363ff7cc34a5e5f171d8311504c723c31896059a071280e2eb27b275"
    "09832c1a1b6e5aa0523bd6b329e32f8453d100ed20fcb15b6acbbe394a4c58cf"
    "d0efaafb434d338545f9027f503c9fa851a3408f929d38f5bcb6da2110fff3d2"
    "cd0c13ec5f974417c4a77e3d645d197360814fdc222a908846eeb814de5e0bdb"
    "e0323a0a4906245cc2d3ac629195e479e7c8376d8dd54ea96c56f4ea657aae08"
    "ba78252e1ca6b4c6e8dd741f4bbd8b8a703eb5664803f60e613557b986c11d9e"
    "e1f8981169d98e949b1e87e9ce5528df8ca1890dbfe6426841992d0fb054bb16")


# ---------------------------------------------------------------- protobuf (encode)
def pb_varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            break
    return bytes(out)


def pb_field(fnum, wire, payload):
    tag = pb_varint((fnum << 3) | wire)
    if wire == 2:
        return tag + pb_varint(len(payload)) + payload
    return tag + payload


def mesh_data(portnum, payload_bytes):
    return pb_field(1, 0, pb_varint(portnum)) + pb_field(2, 2, payload_bytes)


def mesh_position(lat, lon, alt):
    la = int(round(lat * 1e7)) & 0xFFFFFFFF
    lo = int(round(lon * 1e7)) & 0xFFFFFFFF
    b = pb_field(1, 5, la.to_bytes(4, "little")) + pb_field(2, 5, lo.to_bytes(4, "little"))
    if alt is not None:
        b += pb_field(3, 0, pb_varint(int(alt)))
    return b


def mesh_nodeinfo(node_id, longname, shortname, hwmodel):
    b = pb_field(1, 2, node_id.encode()) + pb_field(2, 2, longname.encode()) + pb_field(3, 2, shortname.encode())
    b += pb_field(6, 0, pb_varint(hwmodel))
    return b


def num(v, default=0):
    """an integer from JSON: a number or a string like "0x26011F88"."""
    if v is None:
        return default
    return int(v, 0) if isinstance(v, str) else int(v)


RNG = np.random.default_rng(1)   # reseeded from the scene in main(): reproducible bytes


def rbytes(n):
    return RNG.bytes(n)


PORTS = {"text": 1, "position": 3, "nodeinfo": 4, "routing": 5, "telemetry": 67}


def build_meshtastic(s):
    frm = num(s.get("from"), 0xABCDEF12) & 0xFFFFFFFF
    to = num(s.get("to"), 0xFFFFFFFF) & 0xFFFFFFFF
    pid = num(s.get("id"), 0x12345678) & 0xFFFFFFFF
    hop_limit = int(s.get("hop_limit", 3))
    hop_start = int(s.get("hop_start", 3))
    want_ack = 1 if s.get("want_ack") else 0
    chan_hash = num(s.get("channel_hash"), 0x08) & 0xFF
    next_hop = int(s.get("next_hop", 0)) & 0xFF
    relay_node = int(s.get("relay_node_last", 0)) & 0xFF
    flags = (hop_limit & 0x7) | (want_ack << 3) | ((hop_start & 0x7) << 5)
    hdr = (to.to_bytes(4, "little") + frm.to_bytes(4, "little") + pid.to_bytes(4, "little")
           + bytes([flags, chan_hash, next_hop, relay_node]))
    port = s.get("port", "text")
    if port == "position":
        inner = mesh_position(s["lat"], s["lon"], s.get("alt"))
    elif port == "nodeinfo":
        inner = mesh_nodeinfo(s.get("node_id", "!%08x" % frm), s.get("longname", "Node"),
                              s.get("shortname", "ND"), s.get("hwmodel", 9))
    else:
        inner = s.get("text", "hi").encode()
    data = mesh_data(PORTS.get(port, 1), inner)
    key = DEFAULT_KEY if s.get("key", "default") == "default" else rbytes(16)
    nonce = pid.to_bytes(8, "little") + frm.to_bytes(4, "little") + b"\x00\x00\x00\x00"
    enc = aes_ctr(key, nonce, data)
    return hdr + enc


def build_lorawan(s):
    mtypes = {"join_req": 0, "unconf_up": 2, "conf_up": 4, "unconf_dn": 3, "conf_dn": 5}
    mtype = mtypes.get(s.get("mtype", "unconf_up"), 2)
    mhdr = (mtype << 5) | 0x00
    if mtype == 0:   # join request
        joineui = num(s.get("joineui"), 0x70B3D57ED0000000).to_bytes(8, "little")
        deveui = num(s.get("deveui"), 0x0004A30B001C0530).to_bytes(8, "little")
        devnonce = num(s.get("devnonce"), 0x1234).to_bytes(2, "little")
        mac = bytes([mhdr]) + joineui + deveui + devnonce
        return mac + rbytes(4)
    devaddr = num(s.get("devaddr"), 0x26011F88) & 0xFFFFFFFF
    fctrl = int(s.get("fctrl", 0))
    fcnt = int(s.get("fcnt", 1)) & 0xFFFF
    fport = int(s.get("fport", 1))
    frmpayload = rbytes(int(s.get("payload_len", 6)))
    mac = (bytes([mhdr]) + devaddr.to_bytes(4, "little") + bytes([fctrl]) + fcnt.to_bytes(2, "little")
           + bytes([fport]) + frmpayload)
    return mac + rbytes(4)   # MIC (unknown key: random)


def build_payload(s):
    p = s.get("proto", "lora")
    if p.startswith("meshtastic"):
        return build_meshtastic(s)
    if p == "lorawan":
        return build_lorawan(s)
    return s.get("text", "raw lora").encode() if "text" in s else bytes(s.get("bytes", [1, 2, 3, 4]))


# ---------------------------------------------------------------- geometry
def destination(lat, lon, brg, km):
    R = 6371.0
    br = math.radians(brg)
    la1, lo1 = math.radians(lat), math.radians(lon)
    la2 = math.asin(math.sin(la1) * math.cos(km / R) + math.cos(la1) * math.sin(km / R) * math.cos(br))
    lo2 = lo1 + math.atan2(math.sin(br) * math.sin(km / R) * math.cos(la1),
                           math.cos(km / R) - math.sin(la1) * math.sin(la2))
    return math.degrees(la2), math.degrees(lo2)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    scene = json.load(open(sys.argv[1]))
    out = sys.argv[2]
    if out.rsplit(".", 1)[0] == sys.argv[1].rsplit(".", 1)[0]:
        sys.exit("the output sidecar would overwrite the scene")
    fmt = out.rsplit(".", 1)[-1]
    M = int(scene.get("channels", 5))
    R = float(scene.get("radius_m", 0.12))
    rf = float(scene.get("rf_hz", 921.8e6))
    T = float(scene.get("seconds", 6))
    Ntot = int(T * FS)
    global RNG
    rng = np.random.default_rng(int(scene.get("seed", 1)))
    RNG = np.random.default_rng(int(scene.get("seed", 1)) + 1000)
    sig_n = float(scene.get("noise_lsb", 2.0))
    noise_p = 2 * sig_n ** 2
    st = scene.get("station", [-41.0, 174.0])
    phi = -2 * np.pi * np.arange(M) / M
    by_name = {s.get("name"): s for s in scene["sources"]}

    waves, truth = [], []
    for si, s in enumerate(scene["sources"]):
        b = float(s.get("bearing", 0))
        src = s
        if s.get("proto") == "meshtastic_relay":
            o = by_name[s["relay_of"]]
            src = dict(o)
            src.update(s)   # relay keeps the originator's from/id, its own relay_node_last + bearing
        payload = build_payload(src)
        sf = int(s.get("sf", 11))
        bw = float(s.get("bw", 250000))
        cr = int(s.get("cr", 1))
        mesh = s.get("proto", "lora").startswith("meshtastic")
        sw = num(s.get("sync_word"), 0x2B if mesh else (0x34 if s.get("proto") == "lorawan" else 0x12))
        npre = int(s.get("preamble", 16 if mesh else 8))
        fch = float(s.get("freq_mhz", rf / 1e6)) * 1e6
        off = fch - rf + float(s.get("df_hz", 0))
        amp = math.sqrt(noise_p * 10 ** (float(s.get("level_db", 14)) / 10))
        down = s.get("proto") == "lorawan" and s.get("mtype", "unconf_up") in ("unconf_dn", "conf_dn")
        inv = bool(s.get("inverted", down))
        # LoRaWAN downlinks carry no payload CRC
        wav1, nsym = lora_waveform(payload, sf, bw, cr, s.get("crc", not down), sw, preamble=npre, inverted=inv)
        L = len(wav1)
        steer = np.exp(2j * np.pi * R / (C / rf) * np.cos(np.deg2rad(b) - phi))
        period = float(s.get("period_s", 2.0))
        t = float(s.get("t0", rng.uniform(0.3, 1.0)))
        sent = []
        while t < T - L / FS:
            k0 = int(round(t * FS))
            kk = np.arange(L)
            w = wav1 * amp * np.exp(1j * (rng.uniform(0, 2 * np.pi) + 2 * np.pi * off * (k0 + kk) / FS))
            waves.append((k0, w.astype(np.complex128), steer))
            sent.append([round(t, 6), round(fch / 1e6, 4), len(payload)])
            t += period
        tr = {"name": s.get("name", "s%d" % si), "proto": s.get("proto", "lora"), "bearing": b,
              "sf": sf, "bw": bw, "freq_mhz": fch / 1e6, "inverted": inv, "packets": sent, "payload_hex": payload.hex()}
        truth.append(tr)
        print("%-18s %-16s bearing %6.1f  SF%d BW%d  %3d pkts @ %.3f MHz  %+.0f dB" % (
            tr["name"], s.get("proto", "lora"), b, sf, int(bw / 1e3), len(sent), fch / 1e6, s.get("level_db", 14)))

    waves.sort(key=lambda x: x[0])
    starts = np.array([w[0] for w in waves]) if waves else np.zeros(0)
    maxlen = max([len(w[1]) for w in waves], default=0)
    PIECE = int(0.1 * FS)
    with open(out, "wb") as fo:
        for c0 in range(0, Ntot, PIECE):
            c1 = min(Ntot, c0 + PIECE)
            n = c1 - c0
            x = (rng.standard_normal((n, M)) + 1j * rng.standard_normal((n, M))) * sig_n
            lo = np.searchsorted(starts, c0 - maxlen)   # every packet still on air (SF12 / 125 kHz ones last > 1 s)
            hi = np.searchsorted(starts, c1)
            for k0, w, steer in waves[lo:hi]:
                a, e = max(c0, k0), min(c1, k0 + len(w))
                if e > a:
                    x[a - c0:e - c0, :] += w[a - k0:e - k0, None] * steer[None, :]
            if fmt == "cu8":
                u = np.empty((n, M, 2), np.uint8)
                u[:, :, 0] = np.clip(np.rint(127.5 + x.real), 0, 255)
                u[:, :, 1] = np.clip(np.rint(127.5 + x.imag), 0, 255)
                u.tofile(fo)
            else:
                y = x / 127.5
                (np.clip(y.real, -1, 1) + 1j * np.clip(y.imag, -1, 1)).astype(np.complex64).tofile(fo)
    side = {"rate": FS, "channels": M, "rf_hz": rf, "radius_m": R, "noise_lsb": sig_n, "station": st, "truth": truth}
    json.dump(side, open(out.rsplit(".", 1)[0] + ".json", "w"), indent=1)
    print("%s: %d antennas, %.1f s at 2.4 MHz (%s), %d packets" % (out, M, Ntot / FS, fmt, len(waves)))


if __name__ == "__main__":
    main()
