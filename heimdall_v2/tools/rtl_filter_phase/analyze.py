#!/usr/bin/env python3
"""analyze.py <label> [<label> ...] [--baseline LABEL] [--quiet] [--brief]

Per run (runs/<label>/results.json + curves.npz) and channel, mean over rounds
(+ round-to-round sd):
- inter-channel phase vs CH0 as heimdall left it (raw: reference = mean over
  3/NFFT < |f| <= 0.05 fs) and detrended (constant + slope removed by the
  |S|-weighted fit over 3/NFFT < |f| <= 0.42 fs), at
  f/fs = +-0.1 0.2 0.3 0.35 0.4 0.42 0.45 (each value = mean over |f-fp| <= 4
  bins); worst |value| over |f| <= 0.4 fs (9-bin smoothed; channel, f);
- auto-spectrum per channel in dB rel. to its median over |f| < 0.1 fs, at the
  same points; each channel rel. to CH0; rel. to the baseline run (same channel);
- coherence |S|^2/(P_i P_0) at the same points.
Writes analysis/<label>.txt (full table), analysis/<label>_curves.npz and .csv
(per bin: f/fs, IF Hz, per channel mean raw phase, detrended phase, sd of each,
magnitude dB, coherence).
"""
import argparse, json, os, re, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
NFFT = 1024
FS = 2.4e6
f = (np.arange(NFFT) - NFFT // 2) / NFFT
NODC = np.abs(f) > 3 / NFFT
fit_band = (np.abs(f) <= 0.42) & NODC
ref_band = (np.abs(f) <= 0.05) & NODC
PTS = (-0.45, -0.42, -0.4, -0.35, -0.3, -0.2, -0.1, 0.1, 0.2, 0.3, 0.35, 0.4, 0.42, 0.45)
BRIEF_PTS = (-0.4, -0.3, -0.2, 0.2, 0.3, 0.4)
NCH = 5
SEGS = 16384 // NFFT      # heimdall NUM_SAMPLES per 8091 frame / FFT length


FIR_CORRECT = os.environ.get("NOFIR", "") == ""   # divide heimdall's trim FIR model out (default on)


def fir_dev(d, N=16):
    """heimdall's trim FIR (Hann-windowed sinc, centre N/2-1+d, unity DC gain) relative to an
    ideal delay of N/2-1+d samples, on the fftshifted grid f."""
    n = np.arange(N)
    c = N // 2 - 1
    x = n - c - d
    w = 0.5 - 0.5 * np.cos(2 * np.pi * (n + 0.5) / N)
    h = np.sinc(x) * w
    h = h / h.sum()
    H = np.exp(-2j * np.pi * np.outer(f, n)) @ h
    return H * np.exp(2j * np.pi * f * (c + d))


def detrend(ph, w):
    A = np.stack([np.ones_like(f), f], axis=1)[fit_band]
    W = w[fit_band]
    coef, *_ = np.linalg.lstsq(A * W[:, None], ph[fit_band] * W, rcond=None)
    return ph - (coef[0] + coef[1] * f), coef


def at(x, fp):
    m = np.abs(f - fp) <= 4 / NFFT
    return float(np.mean(x[m]))


def smooth9(x):
    k = np.ones(9) / 9
    return np.convolve(x, k, mode="same")


def worst(x, lim=0.4):
    m = (np.abs(f) <= lim) & (np.abs(f) > 8 / NFFT) & (np.abs(f) <= 0.5 - 5 / NFFT)
    xs = smooth9(x)
    i = np.argmax(np.where(m, np.abs(xs), -1))
    return float(xs[i]), float(f[i])


def if_centre(res):
    """IF centre (Hz) the tuner used: REGDUMP int_freq, else env, else 1.6 MHz."""
    for ln in res.get("register_lines", []):
        m = re.search(r"REGDUMP .* int_freq (-?\d+)", ln)
        if m:
            return float(m.group(1))
    v = res.get("env", {}).get("RTL_R820_IF_CENTER")
    return float(v) if v else 1.6e6


def load(label):
    run = os.path.join(HERE, "runs", label)
    res = json.load(open(os.path.join(run, "results.json")))
    cv = np.load(os.path.join(run, "curves.npz"))
    rounds = [r for r in res["rounds"] if f"S{r['round']}" in cv.files]
    out = {"res": res, "label": label, "ifc": if_centre(res), "rounds": rounds}
    raw, det, mag, coh, magoff = [], [], [], [], []
    lvl_on, lvl_off = [], []
    wpow = float(np.sum(np.hanning(NFFT) ** 2))
    last_trim = {c: 0.0 for c in range(NCH)}
    out["trims"] = []
    for r in rounds:
        S, P = cv[f"S{r['round']}"], cv[f"P{r['round']}"]
        tr = {}
        for c in range(1, NCH):
            v = r.get("trims", {}).get(str(c), r.get("trims", {}).get(c))
            if v is None:
                v = last_trim[c]
            tr[c] = float(v)
            last_trim[c] = tr[c]
        tr[0] = 0.0
        out["trims"].append([tr[c] for c in range(NCH)])
        P_orig = np.real(P)
        if FIR_CORRECT:
            S = S.copy(); P = np.real(P).copy()
            for c in range(1, NCH):
                G = fir_dev(tr[c])
                S[c] = S[c] / G
                P[c] = P[c] / np.abs(G) ** 2
        # total power of the 8-bit samples (Parseval), dB re 127.5^2 (dBFS per I/Q full scale)
        nseg = SEGS * r["frames"]
        lvl_on.append(10 * np.log10(P_orig.sum(axis=1) / (NFFT * wpow * nseg) / 127.5 ** 2))
        if f"Po{r['round']}" in cv.files and r.get("frames_off"):
            Po_ = np.real(cv[f"Po{r['round']}"])
            lvl_off.append(10 * np.log10(Po_.sum(axis=1) / (NFFT * wpow * SEGS * r["frames_off"]) / 127.5 ** 2))
        rr, dd, mm, cc = [], [], [], []
        for c in range(NCH):
            if c == 0:
                rr.append(np.zeros(NFFT)); dd.append(np.zeros(NFFT))
            else:
                ph = np.unwrap(np.angle(S[c]))
                ph -= ph[NFFT // 2]
                ph_raw = ph - np.mean(ph[ref_band])
                d, _ = detrend(ph, np.abs(S[c]))
                rr.append(np.degrees(ph_raw)); dd.append(np.degrees(d))
            p = np.real(P[c])
            mm.append(10 * np.log10(p / np.median(p[np.abs(f) < 0.1])))
            cc.append(np.abs(S[c]) ** 2 / (np.real(P[c]) * np.real(P[0])))
        raw.append(rr); det.append(dd); mag.append(mm); coh.append(cc)
        if f"Po{r['round']}" in cv.files:
            Po = np.real(cv[f"Po{r['round']}"])
            magoff.append([10 * np.log10(Po[c] / np.median(P[c][np.abs(f) < 0.1].real)) for c in range(NCH)])
    out["raw"] = np.array(raw)     # rounds x ch x bins (deg)
    out["det"] = np.array(det)
    out["mag"] = np.array(mag)     # dB rel. own mid-band median
    out["coh"] = np.array(coh)
    out["magoff"] = np.array(magoff) if magoff else None   # noise-off level rel. noise-on mid-band (dB)
    out["lvl_on"] = np.array(lvl_on)
    out["lvl_off"] = np.array(lvl_off) if lvl_off else None
    return out


def fmt(vals, w=6, p=2):
    return " ".join(f"{v:{w}.{p}f}" for v in vals)


def report(d, base=None, brief=False):
    L = []
    res = d["res"]
    nr = len(d["rounds"])
    ce = [r.get("coherence_events") for r in d["rounds"]]
    cc = [(r.get("cal_check") or {}).get("result") for r in d["rounds"]]
    rails = np.array([r.get("rail_frac_noise_on") or [np.nan] * NCH for r in d["rounds"]], dtype=float)
    L.append(f"=== {d['label']}  env={res.get('env')}  freq={res.get('freq', 0)/1e6:.3f} MHz gain={res.get('gain')}"
             f"  IF centre {d['ifc']/1e6:.4f} MHz  rounds {nr}  coherence_events {ce}  cal_check {cc}")
    if nr == 0:
        L.append("  no rounds: " + str(res.get("error")))
        return "\n".join(L)
    if np.isfinite(rails).any():
        L.append(f"  rail fraction (noise on, 8091 bytes at 0/255; CH0 raw, others after rotation): "
                 + " ".join(f"CH{c} {np.nanmean(rails[:, c])*100:.2f}%" for c in range(NCH)))
    L.append("  total power dBFS (re 127.5^2) noise on: " + " ".join(f"CH{c} {d['lvl_on'][:, c].mean():6.2f}" for c in range(NCH))
             + ("   noise off: " + " ".join(f"CH{c} {d['lvl_off'][:, c].mean():6.2f}" for c in range(NCH))
                if d["lvl_off"] is not None else ""))
    pts = BRIEF_PTS if brief else PTS
    hdr = " ".join(f"{p:+6.2f}" for p in pts)
    for kind in ("det", "raw"):
        L.append(f"  -- phase vs CH0 [{kind}] (deg), mean of {nr} rounds; f/fs: {hdr} | worst |.|<=0.4 (f) | sd@+0.4 sd@-0.4")
        for c in range(1, NCH):
            m = d[kind][:, c].mean(axis=0)
            sd4 = np.std([at(x, 0.4) for x in d[kind][:, c]])
            sdm4 = np.std([at(x, -0.4) for x in d[kind][:, c]])
            w, wf = worst(m)
            L.append(f"   CH{c} {fmt([at(m, p) for p in pts])} | {w:+6.2f} ({wf:+.3f}) | {sd4:.2f} {sdm4:.2f}")
    L.append(f"  -- magnitude (dB rel. own median |f|<0.1); f/fs: {hdr}")
    for c in range(NCH):
        m = d["mag"][:, c].mean(axis=0)
        sd = np.std([at(x, 0.4) for x in d["mag"][:, c]])
        L.append(f"   CH{c} {fmt([at(m, p) for p in pts])}   sd@+0.4 {sd:.2f}")
    L.append(f"  -- magnitude rel. CH0 (dB); f/fs: {hdr}")
    m0 = d["mag"][:, 0].mean(axis=0)
    for c in range(1, NCH):
        m = d["mag"][:, c].mean(axis=0) - m0
        L.append(f"   CH{c} {fmt([at(m, p) for p in pts])}")
    if base is not None:
        L.append(f"  -- magnitude rel. baseline {base['label']} same channel (dB); f/fs: {hdr}")
        for c in range(NCH):
            m = d["mag"][:, c].mean(axis=0) - base["mag"][:, c].mean(axis=0)
            L.append(f"   CH{c} {fmt([at(m, p) for p in pts])}")
        for kind in ("det", "raw"):
            L.append(f"  -- phase [{kind}] minus baseline {base['label']} (deg); f/fs: {hdr} | worst<=0.4")
            for c in range(1, NCH):
                m = d[kind][:, c].mean(axis=0) - base[kind][:, c].mean(axis=0)
                w, wf = worst(m)
                L.append(f"   CH{c} {fmt([at(m, p) for p in pts])} | {w:+6.2f} ({wf:+.3f})")
    if not brief:
        L.append(f"  -- coherence |S|^2/(Pi P0); f/fs: {hdr}")
        for c in range(1, NCH):
            m = d["coh"][:, c].mean(axis=0)
            L.append(f"   CH{c} {fmt([at(m, p) for p in pts], 6, 4)}")
        if d["magoff"] is not None:
            L.append(f"  -- noise-OFF level rel. noise-on mid-band (dB) = -SNR of the noise source; f/fs: {hdr}")
            for c in range(NCH):
                m = d["magoff"][:, c].mean(axis=0)
                L.append(f"   CH{c} {fmt([at(m, p) for p in pts])}")
        L.append("  -- per round: trims / cal_check / channel_comp phase_deg")
        for r in d["rounds"]:
            comp = r.get("channel_comp")
            L.append(f"   r{r['round']} conv {r.get('converge_s')} s trims {r.get('trims')} cal {r.get('cal_check')}"
                     f" comp {comp if comp is None else [round(x.get('phase_deg', 0), 2) for x in comp]}")
    return "\n".join(L)


def save_curves(d):
    os.makedirs(os.path.join(HERE, "analysis"), exist_ok=True)
    ifhz = d["ifc"] - f * FS     # inferred orientation (high-side LO): baseband +f at IF = centre - f
    out = {"f_fs": f, "if_hz": ifhz}
    cols = ["f_fs", "if_hz"]
    for c in range(NCH):
        for kind in ("raw", "det"):
            out[f"ch{c}_{kind}_deg"] = d[kind][:, c].mean(axis=0)
            out[f"ch{c}_{kind}_sd"] = d[kind][:, c].std(axis=0)
            cols += [f"ch{c}_{kind}_deg", f"ch{c}_{kind}_sd"]
        out[f"ch{c}_mag_db"] = d["mag"][:, c].mean(axis=0)
        out[f"ch{c}_coh"] = d["coh"][:, c].mean(axis=0)
        cols += [f"ch{c}_mag_db", f"ch{c}_coh"]
        if d["magoff"] is not None:
            out[f"ch{c}_off_db"] = d["magoff"][:, c].mean(axis=0)
            cols.append(f"ch{c}_off_db")
    base = os.path.join(HERE, "analysis", d["label"] + "_curves")
    np.savez(base + ".npz", **out)
    arr = np.stack([out[k] for k in cols], axis=1)
    np.savetxt(base + ".csv", arr, delimiter=",", header=",".join(cols), comments="", fmt="%.6g")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("labels", nargs="+")
    ap.add_argument("--baseline")
    ap.add_argument("--brief", action="store_true")
    ap.add_argument("--quiet", action="store_true", help="write files only")
    a = ap.parse_args()
    base = load(a.baseline) if a.baseline else None
    os.makedirs(os.path.join(HERE, "analysis"), exist_ok=True)
    for lab in a.labels:
        try:
            d = load(lab)
        except FileNotFoundError as e:
            print(f"=== {lab}: missing ({e})")
            continue
        txt = report(d, base if (base is not None and lab != a.baseline) else None, brief=False)
        open(os.path.join(HERE, "analysis", lab + ".txt"), "w").write(txt + "\n")
        if len(d["rounds"]):
            save_curves(d)
        if not a.quiet:
            print(report(d, base if (base is not None and lab != a.baseline) else None, brief=a.brief))


if __name__ == "__main__":
    main()
