"""sigclass.model reader / writer and a numpy forward pass (numpy only).

The binary format is the one src/sigclass/model.cpp loads (documented there):
a header with the feature layout's version, hash and dimension (the C++ core
refuses a model whose layout differs from its own), an opaque JSON metadata
block, then one MLP per view (input standardisation, Linear + ReLU layers,
a linear output layer, softmax with a temperature), then a CRC-32
(zlib's) of everything before it. Format 2 adds an optional per-view
convolutional front end over a sequence slice of the features (the symbol
sequence): "conv": {"off", "S", "C", "W" (K, C, L), "b" (K,), "pools" (1 =
max, 2 = max + mean), "pmean", "pstd" (pools x K)}; the MLP then reads the
standardised features without the slice, followed by the standardised
pooled outputs (all the maxima, then the means). Format 3 adds the RF
plausibility table (from meta["classes"][i]["rf_hz"] / ["generic"] and
meta["bands"] at write time - classes.json's): rf_mask() applies it as
model.cpp does. Formats 1 and 2 still read (no table). Format 4 (2026-11-22)
allows an ensemble per view: {"members": [vm, vm, ...]} (each a view dict as
below, all with the same classes; at most MEMBERS_MAX) is written as one
entry per member, read back the same way, and forward() averages the
members' probabilities - as model.cpp does.

    layout = layout_from_tool("tools/build/sigclass_tool")   # or json.loads(...)
    views = {
        0: {"classes": [{"id": "nfm", "name": "NFM voice"}, ...],
            "mean": mean_D, "std": std_D, "temperature": 1.0,
            "layers": [{"W": W1, "b": b1, "act": "relu"}, ..., {"W": Wn, "b": bn, "act": "none"}]},
        ...                                  # views 0..3; a missing view = no model for it
    }
    write_model("sigclass.model", {"classes_version": 3, ...}, views, layout)
    meta, views, header = read_model("sigclass.model")
    probs = forward(views[0], X)             # X: (n, D) -> (n, classes)

W has shape (out, in) (row-major, as stored), b (out,). A std of 0 (a
constant feature) is fine: the C++ side then ignores that feature; forward()
does the same.
"""
import json
import struct
import subprocess
import zlib

import numpy as np

MAGIC = b"KSIGCLS\0"
FORMAT_VERSION = 3          # what write_model writes by default (4 when a view is an ensemble)
NUM_VIEWS = 4
MEMBERS_MAX = 16
_ACT = {"none": 0, "linear": 0, "relu": 1, 0: 0, 1: 1}
_ACT_NAME = {0: "none", 1: "relu"}


def crc32(data):
    """the trailer: CRC-32 (zlib's, IEEE 802.3) of every byte before it"""
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


def layout_from_tool(tool):
    """the feature layout of a built sigclass_tool (dict with hash, version, dim, groups)"""
    out = subprocess.run([tool, "layout"], check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def _layout_triplet(layout):
    h = layout["hash"]
    h = int(h, 16) if isinstance(h, str) else int(h)
    return int(layout["version"]), h, int(layout["dim"])


def _views_items(views):
    if isinstance(views, dict):
        items = sorted((int(k), v) for k, v in views.items())
    else:
        items = sorted((int(v["view"]), v) for v in views)
    for v, _ in items:
        if not 0 <= v < NUM_VIEWS:
            raise ValueError(f"view {v} out of range")
    if len({v for v, _ in items}) != len(items):
        raise ValueError("a view appears twice")
    return items


def _class_pair(c):
    if isinstance(c, dict):
        return str(c.get("id", "")), str(c.get("name", c.get("id", "")))
    if isinstance(c, (list, tuple)):
        return str(c[0]), str(c[1] if len(c) > 1 else c[0])
    return str(c), str(c)


def write_model(path, meta, views, layout=None, fmt=FORMAT_VERSION):
    """meta: JSON-serialisable (or a JSON string); views: {view: {...}} or a
    list of dicts with "view"; layout: the tool's layout JSON (dict) or None
    to take meta["layout"]."""
    if layout is None:
        if not isinstance(meta, dict) or "layout" not in meta:
            raise ValueError("no layout given (pass layout_from_tool(...) or meta['layout'])")
        layout = meta["layout"]
    lv, lh, dim = _layout_triplet(layout)
    meta_b = (meta if isinstance(meta, str) else json.dumps(meta, separators=(",", ":"))).encode("utf-8")
    out = bytearray()
    out += MAGIC
    items = _views_items(views)
    ens = any('members' in vm for _, vm in items)
    if ens and fmt == FORMAT_VERSION:
        fmt = 4
    if fmt not in (1, 2, 3, 4):
        raise ValueError("format 1 - 4")
    if ens and fmt < 4:
        raise ValueError("an ensemble view needs format 4")
    entries = []
    for v, vm in items:
        mem = vm['members'] if 'members' in vm else [vm]
        if not 1 <= len(mem) <= MEMBERS_MAX:
            raise ValueError(f"view {v}: 1 .. {MEMBERS_MAX} members")
        c0 = [_class_pair(c) for c in mem[0]['classes']]
        for m in mem[1:]:
            if [_class_pair(c) for c in m['classes']] != c0:
                raise ValueError(f"view {v}: the members have different classes")
        entries += [(v, m) for m in mem]
    out += struct.pack("<IIQI", fmt, lv, lh, dim)
    out += struct.pack("<I", len(meta_b)) + meta_b
    out += struct.pack("<I", len(entries))
    for v, vm in entries:
        classes = [_class_pair(c) for c in vm["classes"]]
        temp = float(vm.get("temperature", 1.0))
        if not 1e-3 <= temp <= 1e3:   # the C++ loader's bounds (model.cpp TEMP_MIN / TEMP_MAX)
            raise ValueError("temperature must be 1e-3 .. 1e3")
        out += struct.pack("<IIf", v, len(classes), temp)
        for cid, name in classes:
            for s in (cid, name):
                b = s.encode("utf-8")
                if len(b) > 65535:
                    raise ValueError("class id / name too long")
                out += struct.pack("<H", len(b)) + b
        mean = np.asarray(vm["mean"], np.float32).reshape(-1)
        std = np.asarray(vm["std"], np.float32).reshape(-1)
        if mean.size != dim or std.size != dim:
            raise ValueError(f"view {v}: mean / std must have {dim} values")
        out += mean.astype("<f4").tobytes() + std.astype("<f4").tobytes()
        cv = vm.get("conv")
        sk = vm.get("skip")          # {"off", "S", "C"}: the sequence slice not read (format 2, front end 2)
        prev = dim
        if sk and not cv and fmt >= 2:
            off, S, C = int(sk["off"]), int(sk["S"]), int(sk["C"])
            if off + C * S > dim or C * S >= dim:
                raise ValueError(f"view {v}: bad sequence slice")
            out += struct.pack("<IIII", 2, off, S, C)
            prev = dim - C * S
        elif cv and fmt >= 2:
            Wc = np.asarray(cv["W"], np.float32)
            K, C, L = Wc.shape
            off, S, pools = int(cv["off"]), int(cv["S"]), int(cv.get("pools", 1))
            if C != int(cv["C"]) or off + C * S > dim or L > S or pools not in (1, 2):
                raise ValueError(f"view {v}: bad front end shape")
            bc = np.asarray(cv["b"], np.float32).reshape(-1)
            pm = np.asarray(cv["pmean"], np.float32).reshape(-1)
            ps = np.asarray(cv["pstd"], np.float32).reshape(-1)
            if bc.size != K or pm.size != pools * K or ps.size != pools * K:
                raise ValueError(f"view {v}: bad front end vectors")
            out += struct.pack("<IIIIIII", 1, off, S, C, K, L, pools)
            out += Wc.astype("<f4").tobytes() + bc.astype("<f4").tobytes() + pm.astype("<f4").tobytes() + ps.astype("<f4").tobytes()
            prev = dim - C * S + pools * K
        elif fmt >= 2:
            out += struct.pack("<I", 0)
        if (cv or sk) and fmt < 2:
            raise ValueError("a front end needs format 2")
        layers = vm["layers"]
        out += struct.pack("<I", len(layers))
        for i, L in enumerate(layers):
            W = np.asarray(L["W"], np.float32)
            b = np.asarray(L["b"], np.float32).reshape(-1)
            act = _ACT[L.get("act", "relu" if i + 1 < len(layers) else "none")]
            if W.ndim != 2 or W.shape[1] != prev or b.size != W.shape[0]:
                raise ValueError(f"view {v} layer {i}: W {W.shape}, b {b.shape}, input {prev}")
            if not (np.all(np.isfinite(W)) and np.all(np.isfinite(b))):
                raise ValueError(f"view {v} layer {i}: non-finite weights")
            out += struct.pack("<III", W.shape[1], W.shape[0], act)
            out += W.astype("<f4").tobytes() + b.astype("<f4").tobytes()
            prev = W.shape[0]
        if prev != len(classes) or _ACT[layers[-1].get("act", "none")] != 0:
            raise ValueError(f"view {v}: the last layer must be linear with one output per class")
    if fmt >= 3:
        out += rf_block(meta if isinstance(meta, dict) else json.loads(meta))
    out += struct.pack("<I", crc32(out))
    with open(path, "wb") as f:
        f.write(out)


def _id_bytes(s):
    b = str(s).encode("utf-8")
    if len(b) > 256:
        raise ValueError("RF class id too long")
    return struct.pack("<H", len(b)) + b


def rf_block(meta):
    """the format-3 RF table from the metadata's classes (rf_hz, generic) and bands"""
    cls = [c for c in meta.get("classes", []) if isinstance(c, dict) and (c.get("rf_hz") or c.get("generic"))]
    out = bytearray(struct.pack("<I", len(cls)))
    for c in cls:
        rg = c.get("rf_hz") or []
        out += _id_bytes(c["id"]) + struct.pack("<BI", 1 if c.get("generic") else 0, len(rg))
        for lo, hi in rg:
            if not (0 <= float(lo) < float(hi)):
                raise ValueError("bad rf_hz range of %s" % c["id"])
            out += struct.pack("<dd", float(lo), float(hi))
    bands = meta.get("bands") or []
    out += struct.pack("<I", len(bands))
    for b in bands:
        if not (0 <= float(b["lo"]) < float(b["hi"])):
            raise ValueError("bad band")
        out += struct.pack("<ddI", float(b["lo"]), float(b["hi"]), len(b["only"]))
        for i in b["only"]:
            out += _id_bytes(i)
    return bytes(out)


def rf_mask(rf, vm, lo, hi, P):
    """model.cpp's RF mask for one view: rf = header["rf"] ({"classes": {id: (generic, ranges)},
    "bands": [(lo, hi, {ids})]}); P (n, C) masked and renormalised in place-like (returns new)"""
    P = np.array(P, np.float64, copy=True)
    if not rf:
        return P
    ids = [c["id"] for c in vm["classes"]]
    f = 0.5 * (lo + hi)
    ok = np.ones(len(ids), bool)
    gen = np.zeros(len(ids), bool)
    for k, i in enumerate(ids):
        g, rg = rf["classes"].get(i, (False, []))
        gen[k] = g
        if g:
            continue
        if rg and not any(a <= hi and lo <= b for a, b in rg):
            ok[k] = False
        for a, b, only in rf["bands"]:
            if a <= f <= b and i not in only:
                ok[k] = False
    out = []
    for p in P:
        s = p[ok].sum()
        m = ok
        if not s > 0:
            m = gen
            s = p[m].sum()
            if not s > 0:
                out.append(p)
                continue
        out.append(np.where(m, p / s, 0.0))
    return np.array(out)


def read_model(path):
    """-> (meta, views, header); meta is parsed JSON (the raw string if it is not JSON),
    views {view: {"classes": [{"id", "name"}], "mean", "std", "temperature", "layers"}},
    header {"format", "layout_version", "layout_hash" (hex str), "dim"}"""
    data = open(path, "rb").read()
    if len(data) < 40 or data[:8] != MAGIC:
        raise ValueError("not a sigclass model")
    (chk,) = struct.unpack("<I", data[-4:])
    if chk != crc32(data[:-4]):
        raise ValueError("checksum mismatch")
    p = 8
    fv, lv, lh, dim = struct.unpack_from("<IIQI", data, p)
    p += 20
    if fv not in (1, 2, 3, 4):
        raise ValueError(f"format version {fv}")
    (ml,) = struct.unpack_from("<I", data, p)
    p += 4
    meta_s = data[p:p + ml].decode("utf-8")
    p += ml
    try:
        meta = json.loads(meta_s)
    except ValueError:
        meta = meta_s
    (nv,) = struct.unpack_from("<I", data, p)
    p += 4
    views = {}
    for _ in range(nv):
        v, C, temp = struct.unpack_from("<IIf", data, p)
        p += 12
        classes = []
        for _c in range(C):
            pair = []
            for _s in range(2):
                (n,) = struct.unpack_from("<H", data, p)
                p += 2
                pair.append(data[p:p + n].decode("utf-8"))
                p += n
            classes.append({"id": pair[0], "name": pair[1]})
        mean = np.frombuffer(data, "<f4", dim, p).copy()
        p += 4 * dim
        std = np.frombuffer(data, "<f4", dim, p).copy()
        p += 4 * dim
        conv = skip = None
        if fv >= 2:
            (fe,) = struct.unpack_from("<I", data, p)
            p += 4
            if fe == 2:
                off, S, C = struct.unpack_from("<III", data, p)
                p += 12
                skip = {"off": off, "S": S, "C": C}
            if fe == 1:
                off, S, C, K, L, pools = struct.unpack_from("<IIIIII", data, p)
                p += 24
                Wc = np.frombuffer(data, "<f4", K * C * L, p).reshape(K, C, L).copy()
                p += 4 * K * C * L
                bc = np.frombuffer(data, "<f4", K, p).copy()
                p += 4 * K
                pm = np.frombuffer(data, "<f4", pools * K, p).copy()
                p += 4 * pools * K
                ps = np.frombuffer(data, "<f4", pools * K, p).copy()
                p += 4 * pools * K
                conv = {"off": off, "S": S, "C": C, "W": Wc, "b": bc, "pools": pools, "pmean": pm, "pstd": ps}
        (nl,) = struct.unpack_from("<I", data, p)
        p += 4
        layers = []
        for _l in range(nl):
            fin, fout, act = struct.unpack_from("<III", data, p)
            p += 12
            W = np.frombuffer(data, "<f4", fin * fout, p).reshape(fout, fin).copy()
            p += 4 * fin * fout
            b = np.frombuffer(data, "<f4", fout, p).copy()
            p += 4 * fout
            layers.append({"W": W, "b": b, "act": _ACT_NAME[act]})
        vd = {"classes": classes, "mean": mean, "std": std, "temperature": temp, "layers": layers}
        if conv:
            vd["conv"] = conv
        if skip:
            vd["skip"] = skip
        if v in views:
            # format 4: another member of this view's ensemble
            if fv < 4:
                raise ValueError(f"view {v} twice")
            if "members" not in views[v]:
                views[v] = {"members": [views[v]], "classes": views[v]["classes"]}
            if [c["id"] for c in classes] != [c["id"] for c in views[v]["classes"]]:
                raise ValueError(f"view {v}: members with different classes")
            views[v]["members"].append(vd)
        else:
            views[v] = vd
    rf = None
    if fv >= 3:
        rf = {"classes": {}, "bands": []}
        (ne,) = struct.unpack_from("<I", data, p)
        p += 4
        for _ in range(ne):
            (n,) = struct.unpack_from("<H", data, p)
            p += 2
            cid = data[p:p + n].decode("utf-8")
            p += n
            g, nr = struct.unpack_from("<BI", data, p)
            p += 5
            rg = [struct.unpack_from("<dd", data, p + 16 * k) for k in range(nr)]
            p += 16 * nr
            rf["classes"][cid] = (bool(g & 1), rg)
        (nb,) = struct.unpack_from("<I", data, p)
        p += 4
        for _ in range(nb):
            lo, hi, no = struct.unpack_from("<ddI", data, p)
            p += 20
            only = set()
            for _k in range(no):
                (n,) = struct.unpack_from("<H", data, p)
                p += 2
                only.add(data[p:p + n].decode("utf-8"))
                p += n
            rf["bands"].append((lo, hi, only))
    if p != len(data) - 4:
        raise ValueError("trailing bytes")
    header = {"format": fv, "layout_version": lv, "layout_hash": f"{lh:016x}", "dim": dim, "rf": rf}
    return meta, views, header


def conv_pool(cv, seq):
    """the front end: seq (n, C, S) -> pooled (n, pools x K) before standardisation"""
    W = np.asarray(cv["W"], np.float64)
    K, C, L = W.shape
    S = seq.shape[2]
    T = S - L + 1
    # im2col: (n, T, C x L)
    idx = np.arange(T)[:, None] + np.arange(L)[None, :]
    cols = seq[:, :, idx]                              # (n, C, T, L)
    cols = cols.transpose(0, 2, 1, 3).reshape(seq.shape[0], T, C * L)
    y = cols @ W.reshape(K, C * L).T + np.asarray(cv["b"], np.float64)
    y = np.maximum(y, 0.0)
    out = [y.max(axis=1)]
    if int(cv.get("pools", 1)) == 2:
        out.append(y.mean(axis=1))
    return np.concatenate(out, axis=1)


def mlp_input(vm, X):
    """the standardised MLP input of one view (with the front end's pooled outputs)"""
    X = np.atleast_2d(np.asarray(X, np.float64))
    X = np.where(np.isfinite(X), X, 0.0)
    std = np.asarray(vm["std"], np.float64)
    inv = np.where(np.abs(std) > 1e-12, 1.0 / np.where(std == 0, 1.0, std), 0.0)
    a = (X - np.asarray(vm["mean"], np.float64)) * inv
    cv = vm.get("conv")
    sk = vm.get("skip")
    if sk and not cv:
        off, S, C = int(sk["off"]), int(sk["S"]), int(sk["C"])
        keep = np.ones(X.shape[1], bool)
        keep[off:off + C * S] = False
        a = a[:, keep]
    if cv:
        off, S, C = int(cv["off"]), int(cv["S"]), int(cv["C"])
        seq = np.clip(X[:, off:off + C * S], -16, 16).reshape(-1, C, S)
        keep = np.ones(X.shape[1], bool)
        keep[off:off + C * S] = False
        ps = np.asarray(cv["pstd"], np.float64)
        pinv = np.where(np.abs(ps) > 1e-12, 1.0 / np.where(ps == 0, 1.0, ps), 0.0)
        pooled = (conv_pool(cv, seq) - np.asarray(cv["pmean"], np.float64)) * pinv
        a = np.concatenate([a[:, keep], pooled], axis=1)
    return a


def forward(vm, X):
    """numpy forward pass of one view's model: X (n, D) or (D,) -> probabilities (an
    ensemble view - format 4 - the mean of its members')"""
    if "members" in vm:
        return np.mean([forward(m, X) for m in vm["members"]], axis=0)
    a = mlp_input(vm, X)
    for L in vm["layers"]:
        a = a @ np.asarray(L["W"], np.float64).T + np.asarray(L["b"], np.float64)
        if _ACT[L.get("act", "none")] == 1:
            a = np.maximum(a, 0.0)
    a = np.where(np.isfinite(a), a, 0.0)
    a = np.clip(a / float(vm.get("temperature", 1.0)), -1e4, 1e4)   # as model.cpp (LOGIT_MAX)
    a -= a.max(axis=1, keepdims=True)
    e = np.exp(a)
    return e / e.sum(axis=1, keepdims=True)
