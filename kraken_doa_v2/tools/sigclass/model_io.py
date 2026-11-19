"""sigclass.model reader / writer and a numpy forward pass (numpy only).

The binary format is the one src/sigclass/model.cpp loads (documented there):
a header with the feature layout's version, hash and dimension (the C++ core
refuses a model whose layout differs from its own), an opaque JSON metadata
block, then one MLP per view (input standardisation, Linear + ReLU layers,
a linear output layer, softmax with a temperature), then a CRC-32
(zlib's) of everything before it.

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
FORMAT_VERSION = 1
NUM_VIEWS = 4
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


def write_model(path, meta, views, layout=None):
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
    out += struct.pack("<IIQI", FORMAT_VERSION, lv, lh, dim)
    out += struct.pack("<I", len(meta_b)) + meta_b
    items = _views_items(views)
    out += struct.pack("<I", len(items))
    for v, vm in items:
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
        layers = vm["layers"]
        out += struct.pack("<I", len(layers))
        prev = dim
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
    out += struct.pack("<I", crc32(out))
    with open(path, "wb") as f:
        f.write(out)


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
    if fv != FORMAT_VERSION:
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
        views[v] = {"classes": classes, "mean": mean, "std": std, "temperature": temp, "layers": layers}
    if p != len(data) - 4:
        raise ValueError("trailing bytes")
    header = {"format": fv, "layout_version": lv, "layout_hash": f"{lh:016x}", "dim": dim}
    return meta, views, header


def forward(vm, X):
    """numpy forward pass of one view's MLP: X (n, D) or (D,) -> probabilities"""
    X = np.atleast_2d(np.asarray(X, np.float64))
    X = np.where(np.isfinite(X), X, 0.0)
    std = np.asarray(vm["std"], np.float64)
    inv = np.where(np.abs(std) > 1e-12, 1.0 / np.where(std == 0, 1.0, std), 0.0)
    a = (X - np.asarray(vm["mean"], np.float64)) * inv
    for L in vm["layers"]:
        a = a @ np.asarray(L["W"], np.float64).T + np.asarray(L["b"], np.float64)
        if _ACT[L.get("act", "none")] == 1:
            a = np.maximum(a, 0.0)
    a = np.where(np.isfinite(a), a, 0.0)
    a = np.clip(a / float(vm.get("temperature", 1.0)), -1e4, 1e4)   # as model.cpp (LOGIT_MAX)
    a -= a.max(axis=1, keepdims=True)
    e = np.exp(a)
    return e / e.sum(axis=1, keepdims=True)
