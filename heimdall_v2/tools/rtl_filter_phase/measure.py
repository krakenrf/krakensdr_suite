#!/usr/bin/env python3
"""Register A/B harness for the R820T IF filter measurements
(docs/dev/librtlsdr-fork.md, *The R820T IF filter*; how to build the test heimdall:
../README.md). Run it from a scratch folder that holds heimdall_v2/rfp_heimdall.

measure.py <label> [--env K=V ...] [--freq HZ] [--gain DB] [--rounds N]
           [--exe PATH] [--offair-s S] [--force]

Starts the test heimdall (rfp_heimdall, built against a copy of the fork with
r82xx-env-test.patch) from a fresh scratch cwd runs/<label>/cwd
(heimdall_settings.conf deleted) with --serials 1000,1001,1002,1003,1004 and
the given RTL_R820_* environment. Waits for CONVERGED (8091 header state byte
4), sets frequency and gain explicitly on 8092 (compact JSON), waits for
CONVERGED again, logs get_status. Per round: CONVERGED -> 4 s -> noise-off
capture (off-air background, --offair-s) -> noise source on (8070 WebSocket
BIAS_TEE_ENABLE) -> ~2.5 s of 1024-point cross-spectra of every channel
against CH0 + every channel's auto-spectrum (+ fraction of I/Q bytes at the
rails 0 / 255 per channel) -> noise off -> CHECK_CAL -> get_status ->
reset_lag_compensation (next round). get_status again at the end.

Saves runs/<label>/results.json, curves.npz, heimdall.log, and appends a line
to RUNLOG.md.
"""
import argparse, base64, json, os, re, signal, socket, struct, subprocess, sys, time
import numpy as np

HOST = "127.0.0.1"
HERE = os.path.dirname(os.path.abspath(__file__))
NFFT = 1024
CONVERGED = 4
SERIALS = "1000,1001,1002,1003,1004"
DEFAULT_EXE = os.path.join(HERE, "heimdall_v2", "rfp_heimdall")


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%S")


def ctl(cmd, timeout=8.0):
    s = socket.create_connection((HOST, 8092), timeout=timeout)
    s.sendall((json.dumps(cmd, separators=(",", ":")) + "\n").encode())
    buf, t0 = b"", time.time()
    while time.time() - t0 < timeout:
        d = s.recv(65536)
        if not d:
            break
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            try:
                o = json.loads(line)
            except ValueError:
                continue
            if "status" in o:
                s.close()
                return o
    s.close()
    raise TimeoutError(cmd)


class WS:
    def __init__(self):
        self.s = socket.create_connection((HOST, 8070), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET / HTTP/1.1\r\nHost: localhost:8070\r\nUpgrade: websocket\r\n"
                        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                        f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            self.buf += self.s.recv(4096)
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise RuntimeError(head.decode(errors="replace"))

    def _need(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise ConnectionError("ws closed")
            self.buf += d

    def send(self, text):
        p = text.encode()
        mask = os.urandom(4)
        hdr = bytes([0x81, 0x80 | len(p)]) if len(p) < 126 else bytes([0x81, 0xFE]) + struct.pack(">H", len(p))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(p)))

    def recv(self):
        self._need(2)
        op, ln = self.buf[0] & 0x0F, self.buf[1] & 0x7F
        off = 2
        if ln == 126:
            self._need(4); ln = struct.unpack(">H", self.buf[2:4])[0]; off = 4
        elif ln == 127:
            self._need(10); ln = struct.unpack(">Q", self.buf[2:10])[0]; off = 10
        self._need(off + ln)
        p, self.buf = self.buf[off:off + ln], self.buf[off + ln:]
        return op, p

    def close(self):
        self.s.close()


class Stream:
    def __init__(self):
        self.s = socket.create_connection((HOST, 8091), timeout=5)
        self.buf = bytearray()

    def _need(self, n):
        while len(self.buf) < n:
            d = self.s.recv(1 << 20)
            if not d:
                raise ConnectionError("8091 closed")
            self.buf += d

    def frame(self):
        while True:
            self._need(32)
            if struct.unpack(">I", self.buf[0:4])[0] == 0x4D434851:
                break
            del self.buf[0]
        ch, ns, state, noise = struct.unpack(">IIII", self.buf[4:20])
        hs = 32 + ch * 8
        self._need(hs + ch * ns * 2)
        meta = struct.unpack("<" + "ff" * ch, bytes(self.buf[32:hs]))
        payload = np.frombuffer(bytes(self.buf[hs:hs + ch * ns * 2]), dtype=np.uint8).reshape(ch, ns, 2)
        del self.buf[:hs + ch * ns * 2]
        return state & 0xFF, noise, payload, meta

    def close(self):
        self.s.close()


def state_now():
    try:
        st = Stream()
        s = [st.frame()[0] for _ in range(3)][-1]
        st.close()
        return s
    except OSError:
        return None


def wait_state(pred, timeout, proc=None):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if proc is not None and proc.poll() is not None:
            return None
        s = state_now()
        if s is not None and pred(s):
            return time.time() - t0
        time.sleep(0.5)
    return None


def capture(want_noise, seconds, skip_s):
    """Average 1024-pt Hann cross-spectra vs CH0 and auto-spectra over frames
    with the given noise flag in state CONVERGED. Also the rail fraction."""
    st = Stream()
    win = np.hanning(NFFT).astype(np.float32)
    S = P = None
    rails = None
    nbytes = 0
    n_used, t_ok = 0, None
    meta = None
    t0 = time.time()
    while time.time() - t0 < seconds + skip_s + 15:
        state, noise, pl, mt = st.frame()
        if bool(noise) != want_noise or state != CONVERGED:
            t_ok = None
            continue
        if t_ok is None:
            t_ok = time.time()
        if time.time() - t_ok < skip_s:
            continue
        meta = mt
        z = (pl[..., 0].astype(np.float32) - 127.5) + 1j * (pl[..., 1].astype(np.float32) - 127.5)
        ch, ns = z.shape
        seg = z[:, : ns // NFFT * NFFT].reshape(ch, -1, NFFT) * win
        X = np.fft.fftshift(np.fft.fft(seg, axis=-1), axes=-1)
        s = (X * np.conj(X[0:1])).sum(axis=1)
        p = (np.abs(X) ** 2).sum(axis=1)
        r = ((pl == 0) | (pl == 255)).reshape(ch, -1).sum(axis=1)
        S = s if S is None else S + s
        P = p if P is None else P + p
        rails = r if rails is None else rails + r
        nbytes += ns * 2
        n_used += 1
        if time.time() - t_ok > skip_s + seconds:
            break
    st.close()
    rail_frac = (rails / nbytes).tolist() if rails is not None else None
    return S, P, n_used, rail_frac, meta


def ws_cmd(text):
    w = WS()
    w.send(text)
    time.sleep(0.2)
    w.close()


def check_cal(timeout=25):
    w = WS()
    w.s.settimeout(timeout)
    w.send("CHECK_CAL")
    t0, seen_running, last = time.time(), False, None
    try:
        while time.time() - t0 < timeout:
            op, p = w.recv()
            if op != 1:
                continue
            if p.startswith(b"STATE:"):
                p = p[6:]
            if not p.startswith(b"{"):
                continue
            try:
                o = json.loads(p)
            except ValueError:
                continue
            cc = o.get("cal_check")
            if not cc:
                continue
            last = cc
            if cc.get("running"):
                seen_running = True
            elif seen_running:
                break
    except (socket.timeout, OSError):
        pass
    w.close()
    return last


TRIM_RE = re.compile(r"Channel (\d+) software frac delay: ([-0-9.e]+) -> ([-0-9.e]+)")
REG_RE = re.compile(r"^\[sn[^\]]*\].*$|^\*\*\* .*$", re.M)


def read_from(log_path, offset):
    with open(log_path, errors="replace") as f:
        f.seek(offset)
        return f.read()


def trims_since(log_path, offset):
    d = {}
    for m in TRIM_RE.finditer(read_from(log_path, offset)):
        d[int(m.group(1))] = float(m.group(3))
    return d


def port_busy(port):
    try:
        s = socket.create_connection((HOST, port), timeout=0.5)
        s.close()
        return True
    except OSError:
        return False


def precheck():
    out = subprocess.run(["pgrep", "-a", "heimdall"], capture_output=True, text=True).stdout.strip()
    busy = [p for p in (8070, 8091, 8092, 1234) if port_busy(p)]
    tm = subprocess.run(["tmux", "has-session", "-t", "krakensdr"], capture_output=True).returncode == 0
    return out, busy, tm


def runlog(line):
    with open(os.path.join(HERE, "RUNLOG.md"), "a") as f:
        f.write(line + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--env", action="append", default=[], help="K=V (RTL_R820_* knobs)")
    ap.add_argument("--freq", type=float, default=100e6)
    ap.add_argument("--gain", type=float, default=49.6)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--offair-s", type=float, default=1.0)
    ap.add_argument("--noise-s", type=float, default=2.5)
    ap.add_argument("--first-timeout", type=float, default=120.0)
    ap.add_argument("--conv-timeout", type=float, default=90.0)
    ap.add_argument("--if-vga", type=int, default=None, help="8092 set_if_vga index (diagnostic), then full recal")
    ap.add_argument("--note", default="")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()

    run = os.path.join(HERE, "runs", a.label)
    if os.path.exists(os.path.join(run, "results.json")) and not a.force:
        sys.exit(f"{run} already has results.json (use --force)")
    cwd = os.path.join(run, "cwd")
    os.makedirs(cwd, exist_ok=True)
    for fn in ("heimdall_settings.conf",):
        p = os.path.join(cwd, fn)
        if os.path.exists(p):
            os.remove(p)
    exe = os.path.abspath(a.exe)
    hdir = os.path.dirname(exe)
    dst = os.path.join(cwd, "index.html")
    if not os.path.lexists(dst):
        os.symlink(os.path.join(hdir, "index.html"), dst)

    pgrep, busy, tm = precheck()
    if pgrep or busy or tm:
        sys.exit(f"hardware not free: pgrep={pgrep!r} busy_ports={busy} krakensdr_tmux={tm}")

    knobs = {}
    for kv in a.env:
        k, v = kv.split("=", 1)
        knobs[k] = v
    env = {k: v for k, v in os.environ.items() if not k.startswith("RTL_R820_")}
    env["HEIMDALL_VERBOSE_LOG"] = "1"
    env.update(knobs)
    argv = [exe, "--serials", SERIALS]
    cmdline = " ".join([f"{k}={v}" for k, v in sorted(knobs.items())] + ["HEIMDALL_VERBOSE_LOG=1"] + argv)
    log_path = os.path.join(run, "heimdall.log")
    if a.if_vga is not None:
        cmdline += f"  (+ 8092 set_if_vga {a.if_vga} after set_gain, then reset_lag_compensation)"
    res = {"label": a.label, "env": knobs, "command": cmdline, "cwd": cwd, "freq": a.freq, "gain": a.gain,
           "if_vga": a.if_vga,
           "rounds_requested": a.rounds, "note": a.note, "t_start": now(),
           "loadavg_start": os.getloadavg(), "rounds": []}
    print(f"[{now()}] {a.label}: {cmdline}", flush=True)
    runlog(f"| {a.label} | {' '.join(f'{k}={v}' for k, v in sorted(knobs.items())) or '(none)'} | "
           f"{a.freq/1e6:.3f} MHz | {a.gain} | start {res['t_start']} | load {res['loadavg_start'][0]:.1f} | {a.note}")
    logf = open(log_path, "w")
    proc = subprocess.Popen(argv, cwd=cwd, stdout=logf, stderr=subprocess.STDOUT, env=env)
    res["pid"] = proc.pid
    curves = {}
    ok = False
    try:
        t1 = wait_state(lambda s: s == CONVERGED, a.first_timeout, proc)
        res["first_converge_s"] = None if t1 is None else round(t1, 1)
        if t1 is None:
            res["error"] = f"no CONVERGED within {a.first_timeout} s of start (exit={proc.poll()})"
            print(res["error"], flush=True)
            return
        r1 = ctl({"command": "set_frequency", "frequency": int(round(a.freq))})
        time.sleep(0.5)
        r2 = ctl({"command": "set_gain", "gain": a.gain})
        res["set_replies"] = [r1, r2]
        time.sleep(1.0)
        t2 = wait_state(lambda s: s == CONVERGED, a.conv_timeout, proc)
        res["reconverge_s"] = None if t2 is None else round(t2, 1)
        if t2 is None:
            res["error"] = f"no CONVERGED within {a.conv_timeout} s after set_frequency/set_gain"
            print(res["error"], flush=True)
            return
        if a.if_vga is not None:
            # diagnostic: fixed R820T IF VGA step (not persisted), then a full recalibration so
            # that calibration and measurement use the same step
            res["if_vga_reply"] = ctl({"command": "set_if_vga", "index": a.if_vga})
            time.sleep(0.5)
            ctl({"command": "reset_lag_compensation"})
            wait_state(lambda s: s != CONVERGED, 30, proc)
            t3 = wait_state(lambda s: s == CONVERGED, a.conv_timeout, proc)
            res["vga_reconverge_s"] = None if t3 is None else round(t3, 1)
            if t3 is None:
                res["error"] = f"no CONVERGED within {a.conv_timeout} s after set_if_vga {a.if_vga}"
                print(res["error"], flush=True)
                return
        res["status_start"] = ctl({"command": "get_status"})
        for r in range(a.rounds):
            log_off = os.path.getsize(log_path)
            if r > 0:
                ctl({"command": "reset_lag_compensation"})
                wait_state(lambda s: s != CONVERGED, 30, proc)
                tconv = wait_state(lambda s: s == CONVERGED, a.conv_timeout, proc)
            else:
                tconv = 0.0
                log_off = 0
            if tconv is None:
                print(f"round {r}: no CONVERGED", flush=True)
                res["error"] = f"round {r}: no CONVERGED within {a.conv_timeout} s"
                break
            time.sleep(4.0)
            trims = trims_since(log_path, log_off)
            So, Po, no, rails_off, _ = capture(False, a.offair_s, 0.3) if a.offair_s > 0 else (None, None, 0, None, None)
            ws_cmd("BIAS_TEE_ENABLE")
            S, P, n, rails, meta = capture(True, a.noise_s, 0.6)
            ws_cmd("BIAS_TEE_DISABLE")
            time.sleep(2.0)
            cc = check_cal()
            stat = ctl({"command": "get_status"})
            rr = {"round": r, "t": now(), "converge_s": round(tconv, 1), "trims": trims, "frames": n,
                  "frames_off": no, "rail_frac_noise_on": rails, "rail_frac_noise_off": rails_off,
                  "meta_freq_gain": meta, "cal_check": cc,
                  "coherence_events": stat.get("coherence_events"), "lag_no_peak": stat.get("lag_no_peak"),
                  "channel_comp": stat.get("channel_comp"), "settings": stat.get("settings"),
                  "tuner_frequencies": stat.get("tuner_frequencies"), "loadavg": os.getloadavg()}
            print(json.dumps({k: rr[k] for k in ("round", "converge_s", "frames", "frames_off", "cal_check",
                                                "coherence_events", "rail_frac_noise_on", "loadavg")}), flush=True)
            res["rounds"].append(rr)
            if S is not None:
                curves[f"S{r}"], curves[f"P{r}"] = S, P
            if So is not None:
                curves[f"So{r}"], curves[f"Po{r}"] = So, Po
        res["status_end"] = ctl({"command": "get_status"})
        ok = "error" not in res
    except Exception as e:  # noqa: BLE001 - record and stop heimdall
        res["error"] = f"{type(e).__name__}: {e}"
        print(res["error"], flush=True)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(20)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(5)
        logf.close()
        res["exit_code"] = proc.returncode
        res["t_end"] = now()
        res["loadavg_end"] = os.getloadavg()
        txt = read_from(log_path, 0)
        res["register_lines"] = REG_RE.findall(txt)
        json.dump(res, open(os.path.join(run, "results.json"), "w"), indent=1)
        np.savez(os.path.join(run, "curves.npz"), **curves)
        ce = [x.get("coherence_events") for x in res["rounds"]]
        runlog(f"|   -> {a.label} | end {res['t_end']} | rounds {len(res['rounds'])} | "
               f"coherence_events {ce} | {'OK' if ok else 'ERROR: ' + str(res.get('error'))}")
        time.sleep(1.0)
        pg, busy, _ = precheck()
        print(f"[{now()}] stopped (exit {proc.returncode}); pgrep={pg!r} busy={busy}", flush=True)


if __name__ == "__main__":
    main()
