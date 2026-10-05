#!/usr/bin/env python3
"""KrakenSDR AI Signal Lab bridge.

Runs an LLM coding agent (Claude Code by default) to identify a signal and to
write decoder plugins for it. kraken_doa starts this script for the sidebar's
"Investigate this signal with AI" / "Create Decoder" buttons and forwards its
stdout - one JSON object per line ({"ev": ...}) - to the browser.

On the Pi (once, in a terminal):
  python3 ai/kraken_ai.py setup      check the LLM CLI + login, enable the lab
  python3 ai/kraken_ai.py disable    turn it off again

Used by kraken_doa:
  kraken_ai.py status                configuration as JSON (no LLM call)
  kraken_ai.py test                  a one-line round trip to the LLM
  kraken_ai.py investigate --session DIR --freq HZ [--rate HZ] [--seconds S]
                           [--context-json JSON] [--instructions TEXT]
                           [--capture-file REC.cf32]   (a recording instead of a capture)
  kraken_ai.py create --session DIR --plugin ID [--instructions TEXT]
  kraken_ai.py ask --session DIR --text QUESTION
  kraken_ai.py delete --session DIR        the session and its Claude transcript

Sessions live in ai/sessions/<id>/ (capture, plots, analysis.md,
session.json, chat.json = the conversation, activity.jsonl = the agent's
steps). Plugins are written to plugins/<id>/ (see plugins/SDK.md).
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid

AI_DIR = os.path.dirname(os.path.abspath(__file__))
KRAKEN_DIR = os.path.dirname(AI_DIR)
PLUGIN_DIR = os.path.abspath(os.environ.get("KRAKEN_PLUGIN_DIR") or os.path.join(KRAKEN_DIR, "plugins"))
SESSIONS = os.path.join(AI_DIR, "sessions")
CONFIG = os.environ.get("KRAKEN_AI_CONFIG") or os.path.join(AI_DIR, "ai_config.json")
SIGTOOL = os.path.join(AI_DIR, "sigtool.py")
BUILTIN = ("the decoder plugins listed above (every decoder is a plugin, including the protocols that "
           "ship with the suite: P25, DMR, TETRA, D-STAR, NXDN, MPT1327, POCSAG, APRS; Auto detect runs "
           "the ones the user ticked, all by default)")
ACTIVITY_MAX = 4 << 20   # activity.jsonl stops growing here
ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,31}$")

DEFAULTS = {
    "enabled": False,
    "backend": "claude",          # "claude" | "command"
    "model": "",                  # claude --model (empty = the CLI's default)
    "command": [],                # backend "command": argv, prompt on stdin
    "max_budget_usd": 0,          # claude --max-budget-usd (API-key accounts), 0 = no limit
    "investigate_timeout_s": 900,
    "create_timeout_s": 3600,
}


# ---------------------------------------------------------------------------
SESSION_DIR = None   # set by the commands that work on a session


def emit(ev, **kw):
    kw["ev"] = ev
    line = json.dumps(kw, ensure_ascii=False)
    sys.stdout.write(line + "\n")
    sys.stdout.flush()
    # the session keeps the agent's steps (shown again from the history)
    if SESSION_DIR and ev not in ("report",):
        try:
            p = os.path.join(SESSION_DIR, "activity.jsonl")
            if not os.path.exists(p) or os.path.getsize(p) < ACTIVITY_MAX:
                rec = dict(kw)
                rec["t"] = int(time.time() * 1000)
                with open(p, "a") as f:
                    f.write(json.dumps(rec, ensure_ascii=False) + "\n")
        except OSError:
            pass


def chat_append(sdir, role, kind, text):
    """The conversation of a session (chat.json): what the user asked and what
    the AI answered, in order"""
    p = os.path.join(sdir, "chat.json")
    try:
        with open(p) as f:
            chat = json.load(f)
        if not isinstance(chat, list):
            chat = []
    except (OSError, ValueError):
        chat = []
        # a session from before chat.json existed: start with its analysis
        sess = session_load(sdir)
        if sess.get("analysis") and not (role == "user" and kind == "investigate"):
            t0 = int(os.path.getmtime(os.path.join(sdir, "session.json")) * 1000)
            chat = [{"role": "user", "kind": "investigate", "t": t0,
                     "text": f"Investigate the signal at {float(sess.get('freq_hz') or 0) / 1e6:.5f} MHz"},
                    {"role": "assistant", "kind": "investigate", "t": t0, "text": sess["analysis"]}]
    chat.append({"role": role, "kind": kind, "text": text, "t": int(time.time() * 1000)})
    tmp = p + ".tmp"
    with open(tmp, "w") as f:
        json.dump(chat, f, ensure_ascii=False, indent=1)
    os.replace(tmp, p)


def set_session(sdir):
    global SESSION_DIR
    SESSION_DIR = sdir


def load_config():
    cfg = dict(DEFAULTS)
    try:
        with open(CONFIG) as f:
            cfg.update(json.load(f))
    except (OSError, ValueError):
        pass
    return cfg


def save_config(cfg):
    tmp = CONFIG + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f, indent=2)
    os.replace(tmp, CONFIG)


def claude_path():
    p = shutil.which("claude")
    if p:
        return p
    for c in (os.path.expanduser("~/.local/bin/claude"), os.path.expanduser("~/.claude/local/claude"),
              "/usr/local/bin/claude"):
        if os.access(c, os.X_OK):
            return c
    return None


def claude_supports(flag):
    try:
        h = subprocess.run([claude_path(), "--help"], capture_output=True, text=True, timeout=20).stdout
        return flag in h
    except (OSError, subprocess.SubprocessError):
        return False


def session_load(sdir):
    try:
        with open(os.path.join(sdir, "session.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def session_save(sdir, s):
    tmp = os.path.join(sdir, "session.json.tmp")
    with open(tmp, "w") as f:
        json.dump(s, f, indent=1, ensure_ascii=False)
    os.replace(tmp, os.path.join(sdir, "session.json"))


def sigtool_help():
    return """python3 {t} capture --freq HZ [--rate HZ] [--bw HZ] [--seconds S] -o FILE.cf32
      new capture from the receiver (channel 0, 2.4 MHz span around the current centre -
      in wideband / independent mode the tuner whose band holds HZ; the context's
      "tuner" / "center_freq_hz" say which; the rate becomes 2.4 MHz / integer). Use a longer capture for intermittent signals.
python3 {t} analyze FILE.cf32 [--start S] [--seconds S]      report + PNG plots (Read them)
python3 {t} spectrogram FILE.cf32 -o OUT.png [--start S] [--seconds S] [--fft N]
python3 {t} extract FILE.cf32 --offset HZ [--rate HZ] [--bw HZ] -o OUT.cf32   re-centre / narrow
python3 {t} demod FILE.cf32 --mode fm|am|usb|lsb -o OUT.wav
python3 {t} tones FILE.cf32|FILE.wav                          audio tones after FM demod
python3 {t} symbols FILE.cf32 --baud B [--levels 2|4] [--demod fm|am] [-o OUT.txt]
      symbol recovery, eye quality, repeated words (sync candidates), frame period""".format(t=SIGTOOL)


# ---------------------------------------------------------------------------
# Backends
# ---------------------------------------------------------------------------
class Runner:
    """Runs one agent turn and turns its output into events. Returns
    (ok, final_text)."""

    def __init__(self, cfg, sdir, timeout):
        self.cfg, self.sdir, self.timeout = cfg, sdir, timeout
        self.proc = None

    def run(self, prompt, allowed, resume_id=None, new_id=None, system=""):
        if self.cfg.get("backend", "claude") == "command":
            return self.run_command(prompt, system)
        return self.run_claude(prompt, allowed, resume_id, new_id, system)

    def run_command(self, prompt, system):
        argv = self.cfg.get("command") or []
        if not argv:
            emit("error", msg="backend \"command\" has no command configured in ai/ai_config.json")
            return False, ""
        full = (system + "\n\n" + prompt) if system else prompt
        emit("status", state="thinking", msg="running " + os.path.basename(argv[0]))
        try:
            p = subprocess.Popen(argv, cwd=self.sdir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, bufsize=1)
        except OSError as e:
            emit("error", msg=f"cannot run {argv[0]}: {e}")
            return False, ""
        self.proc = p
        p.stdin.write(full)
        p.stdin.close()
        out = []
        t0 = time.time()
        for line in p.stdout:
            out.append(line)
            emit("text", text=line.rstrip("\n"))
            if time.time() - t0 > self.timeout:
                p.kill()
                emit("error", msg="timed out")
                return False, "".join(out)
        rc = p.wait()
        return rc == 0, "".join(out)

    def run_claude(self, prompt, allowed, resume_id, new_id, system):
        exe = claude_path()
        if not exe:
            emit("error", msg="Claude Code (claude) is not installed - see the README (AI Signal Lab)")
            return False, ""
        argv = [exe, "-p", "--output-format", "stream-json", "--verbose", "--permission-mode", "dontAsk"]
        if claude_supports("--safe-mode"):
            argv.append("--safe-mode")      # no user hooks / MCP servers / CLAUDE.md in the agent
        if self.cfg.get("model"):
            argv += ["--model", self.cfg["model"]]
        if float(self.cfg.get("max_budget_usd") or 0) > 0:
            argv += ["--max-budget-usd", str(self.cfg["max_budget_usd"])]
        if resume_id:
            argv += ["--resume", resume_id]
        elif new_id:
            argv += ["--session-id", new_id]
        if system:
            argv += ["--append-system-prompt", system]
        argv += ["--add-dir", PLUGIN_DIR, AI_DIR]
        argv += ["--allowedTools"] + allowed
        try:
            p = subprocess.Popen(argv, cwd=self.sdir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True, bufsize=1)
        except OSError as e:
            emit("error", msg=f"cannot run claude: {e}")
            return False, ""
        self.proc = p
        p.stdin.write(prompt)
        p.stdin.close()
        final, ok = "", False
        t0 = time.time()
        denied = 0
        for line in p.stdout:
            if time.time() - t0 > self.timeout:
                p.kill()
                emit("error", msg=f"timed out after {self.timeout} s")
                break
            try:
                d = json.loads(line)
            except ValueError:
                continue
            t = d.get("type")
            if t == "system" and d.get("subtype") == "init":
                emit("status", state="thinking", msg="agent started (" + str(d.get("model", "")) + ")",
                     session=d.get("session_id"))
            elif t == "system" and d.get("subtype") == "permission_denied":
                denied += 1
            elif t == "assistant":
                for c in d.get("message", {}).get("content", []):
                    if c.get("type") == "text" and c.get("text", "").strip():
                        emit("text", text=c["text"])
                    elif c.get("type") == "tool_use":
                        emit("tool", name=c.get("name", ""), detail=tool_detail(c.get("name", ""), c.get("input", {})))
            elif t == "user":
                for c in d.get("message", {}).get("content", []):
                    if isinstance(c, dict) and c.get("type") == "tool_result":
                        cc = c.get("content")
                        if isinstance(cc, list):
                            cc = " ".join(x.get("text", "") for x in cc if isinstance(x, dict))
                        emit("tool_result", ok=not c.get("is_error"), detail=str(cc or "")[:600])
            elif t == "result":
                final = d.get("result") or ""
                ok = d.get("subtype") == "success" and not d.get("is_error")
                emit("usage", cost_usd=d.get("total_cost_usd"), turns=d.get("num_turns"),
                     duration_s=round((d.get("duration_ms") or 0) / 1000), denied=denied)
        rc = p.wait()
        err = p.stderr.read().strip()
        if not ok:
            hint = err[-600:] if err else ""
            if "login" in hint.lower() or "auth" in hint.lower() or "api key" in hint.lower():
                hint += "\n(Claude Code is not logged in: run `claude` once in a terminal on the Pi)"
            emit("error", msg=("agent failed (exit %d)" % rc) + (": " + hint if hint else ""))
        return ok, final


def tool_detail(name, inp):
    if name == "Bash":
        return inp.get("command", "")[:300]
    if name in ("Read", "Write", "Edit", "MultiEdit"):
        return inp.get("file_path", "")
    if name in ("Glob", "Grep"):
        return inp.get("pattern", "")
    if name in ("WebSearch",):
        return inp.get("query", "")
    if name in ("WebFetch",):
        return inp.get("url", "")
    return json.dumps(inp)[:200]


def rel(p):
    return "//" + os.path.abspath(p).lstrip("/")


def base_tools(sdir):
    return [
        f"Read({rel(sdir)}/**)", f"Read({rel(PLUGIN_DIR)}/**)", f"Read({rel(AI_DIR)}/*.py)",
        "Glob", "Grep", "WebSearch", "WebFetch",
        f"Edit({rel(sdir)}/**)",
        f"Bash(python3 {SIGTOOL}:*)",
        # scripts the agent writes into its session directory (a `:*` prefix
        # rule only matches whole words, so a wildcard)
        f"Bash(python3 {os.path.abspath(sdir)}/*)",
    ]


SYSTEM = """You are the AI Signal Lab of a KrakenSDR receiver (a 5-channel coherent RTL-SDR array
on a Raspberry Pi). You analyse radio signals the user points you at and write decoder plugins.
- Work only in your session directory {sdir} (scratch scripts and notes go there) and, when asked
  to write a decoder, in the plugin directory you are given. Python 3 with numpy is available
  (no scipy/matplotlib). Run your own scripts as: python3 {sdir}/<script>.py
- The Pi has 4 cores: never build with more than -j3; keep scripts efficient.
- Your text output is shown live in the receiver's web UI: keep progress notes short.
- Be factual: state confidence and the measurements behind a conclusion; say "unknown" rather
  than guessing. Do not decode/describe the content of private communications beyond what is
  needed to identify the system."""


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def cmd_status(_a):
    cfg = load_config()
    out = {"enabled": bool(cfg.get("enabled")), "backend": cfg.get("backend", "claude"),
           "model": cfg.get("model", ""), "config": CONFIG}
    if out["backend"] == "claude":
        exe = claude_path()
        out["cli"] = exe or ""
        if exe:
            try:
                out["version"] = subprocess.run([exe, "--version"], capture_output=True, text=True,
                                                timeout=20).stdout.strip()
            except (OSError, subprocess.SubprocessError):
                out["version"] = ""
    else:
        out["cli"] = " ".join(cfg.get("command") or [])
    print(json.dumps(out))


def cmd_test(_a):
    cfg = load_config()
    emit("status", state="testing", msg="asking the LLM for a one-line reply")
    if cfg.get("backend", "claude") == "command":
        r = Runner(cfg, AI_DIR, 120)
        ok, text = r.run_command("Reply with exactly: KRAKEN_OK", "")
    else:
        exe = claude_path()
        if not exe:
            emit("done", kind="test", ok=False, text="Claude Code is not installed")
            return
        argv = [exe, "-p", "--output-format", "json", "--permission-mode", "dontAsk"]
        if cfg.get("model"):
            argv += ["--model", cfg["model"]]
        try:
            p = subprocess.run(argv, input="Reply with exactly: KRAKEN_OK", capture_output=True, text=True,
                               timeout=180, cwd=AI_DIR)
            d = json.loads(p.stdout) if p.stdout.strip().startswith("{") else {}
            text = d.get("result", "") or p.stderr.strip()[-400:]
            ok = "KRAKEN_OK" in text
        except (OSError, subprocess.SubprocessError, ValueError) as e:
            ok, text = False, str(e)
    emit("done", kind="test", ok=ok, text=text if ok else "No answer: " + text +
         "\nIs the CLI logged in? Run `claude` once in a terminal on the Pi.")


def cmd_setup(a):
    cfg = load_config()
    print("KrakenSDR AI Signal Lab setup\n")
    if a.backend:
        cfg["backend"] = a.backend
    if a.command:
        cfg["command"] = a.command.split()
    if a.model is not None:
        cfg["model"] = a.model
    if cfg.get("backend", "claude") == "claude":
        exe = claude_path()
        if not exe:
            print("Claude Code is not installed. Install it with:\n"
                  "    curl -fsSL https://claude.ai/install.sh | bash\n"
                  "then run `claude` once to log in, and run this setup again.")
            return 1
        print(f"Claude Code found: {exe}")
        print("Checking the login with a one-line request ...")
        try:
            p = subprocess.run([exe, "-p", "--output-format", "json"], input="Reply with exactly: KRAKEN_OK",
                               capture_output=True, text=True, timeout=180, cwd=AI_DIR)
            ok = "KRAKEN_OK" in p.stdout
        except (OSError, subprocess.SubprocessError):
            ok = False
        if not ok:
            print("No answer - log in first: run `claude` in a terminal (or set ANTHROPIC_API_KEY),"
                  " then run this setup again.")
            return 1
        print("OK - the LLM answered.")
    else:
        if not cfg.get("command"):
            print("backend 'command' needs --command \"your-cli args\" (prompt on stdin)")
            return 1
        print("Using command:", " ".join(cfg["command"]))
    print("\nThe AI Signal Lab lets anyone who can use this receiver's web UI start an AI agent on")
    print("this computer, which captures signals, runs analysis scripts and writes + compiles decoder")
    print("plugins (native code that then runs here). It uses your LLM account (usage counts against")
    print("your plan / API budget). Set an API token for the web UI (see README) if others can reach it.")
    if not a.yes:
        ans = input("\nEnable the AI Signal Lab? [y/N] ").strip().lower()
        if ans not in ("y", "yes"):
            print("Not enabled.")
            return 1
    cfg["enabled"] = True
    save_config(cfg)
    print(f"Enabled ({CONFIG}). Open the receiver's web UI: sidebar -> AI Signal Lab.")
    return 0


def cmd_disable(_a):
    cfg = load_config()
    cfg["enabled"] = False
    save_config(cfg)
    print("AI Signal Lab disabled.")


def require_enabled(cfg):
    if not cfg.get("enabled"):
        emit("error", msg="The AI Signal Lab is not enabled on this receiver: run "
                          "`python3 ai/kraken_ai.py setup` on the Pi")
        sys.exit(1)


def install_term_handler(runner):
    def h(signum, _f):
        if runner.proc and runner.proc.poll() is None:
            runner.proc.terminate()
        emit("error", msg="cancelled")
        sys.exit(1)
    signal.signal(signal.SIGTERM, h)
    signal.signal(signal.SIGINT, h)


def context_summary(ctx):
    if not ctx:
        return "(no receiver context)"
    lines = []
    for k in ("vfo_id", "vfo_freq_hz", "vfo_offset_hz", "vfo_rate_hz", "vfo_bandwidth_hz", "demod", "center_freq_hz",
              "gain_db", "num_elements", "squelch_db"):
        if k in ctx:
            lines.append(f"- {k}: {ctx[k]}")
    dig = ctx.get("digital")
    if isinstance(dig, dict) and dig:
        lines.append(f"- built-in digital decoder on this VFO: mode {dig.get('mode')}, state {dig.get('state')}, "
                     f"detected '{dig.get('detected')}'")
        info = dig.get("info") or {}
        for proto, facts in info.items():
            if facts:
                lines.append(f"  {proto}: " + "; ".join(f"{f[0]}={f[1]}" for f in facts[:12]))
    plugins = ctx.get("plugins")
    if plugins:
        lines.append("- installed decoder plugins: " + ", ".join(f"{p.get('id')} ({p.get('name')})" for p in plugins))
    return "\n".join(lines)


def cmd_investigate(a):
    cfg = load_config()
    require_enabled(cfg)
    sdir = os.path.abspath(a.session)
    os.makedirs(sdir, exist_ok=True)
    ctx = {}
    if a.context_json:
        try:
            ctx = json.loads(a.context_json)
        except ValueError:
            ctx = {}
    with open(os.path.join(sdir, "context.json"), "w") as f:
        json.dump(ctx, f, indent=1)
    set_session(sdir)
    chat_append(sdir, "user", "investigate",
                f"Investigate the signal at {a.freq / 1e6:.5f} MHz" + (f"\n\n{a.instructions}" if a.instructions else ""))
    sess = session_load(sdir)
    sess.update({"id": os.path.basename(sdir), "created": sess.get("created") or time.strftime("%Y-%m-%d %H:%M:%S"),
                 "freq_hz": a.freq, "vfo": ctx.get("vfo_id"), "state": "investigating"})
    session_save(sdir, sess)

    rate = a.rate or 48000
    cap = os.path.join(sdir, "capture.cf32")
    if a.capture_file:
        # analyse an existing recording (.cf32 + .json sidecar from sigtool)
        emit("status", state="capturing", msg="using the recording " + os.path.basename(a.capture_file))
        shutil.copyfile(a.capture_file, cap)
        side = os.path.splitext(a.capture_file)[0] + ".json"
        if not os.path.exists(side):
            side = a.capture_file + ".json"
        try:
            with open(side) as f:
                m = json.load(f)
        except (OSError, ValueError):
            m = {"rate": rate}
        m.setdefault("rf_hz", a.freq)
        with open(os.path.join(sdir, "capture.json"), "w") as f:
            json.dump(m, f, indent=1)
        a.seconds = os.path.getsize(cap) / 8 / float(m.get("rate", rate))
    else:
        emit("status", state="capturing", msg=f"capturing {a.seconds:g} s at {a.freq / 1e6:.5f} MHz")
        p = subprocess.run([sys.executable, SIGTOOL, "capture", "--freq", str(a.freq), "--rate", str(rate),
                            "--seconds", str(a.seconds), "-o", cap], capture_output=True, text=True)
        if p.returncode != 0:
            emit("error", msg="capture failed: " + (p.stderr or p.stdout).strip()[-500:])
            sess["state"] = "failed"
            session_save(sdir, sess)
            sys.exit(1)
        emit("tool_result", ok=True, detail=p.stdout.strip())
    emit("status", state="analyzing", msg="measuring the capture")
    p = subprocess.run([sys.executable, SIGTOOL, "analyze", cap], capture_output=True, text=True)
    report = p.stdout.strip() if p.returncode == 0 else "analysis failed: " + p.stderr[-400:]
    emit("report", text=report)

    prompt = f"""Identify the radio signal the user pointed the receiver at.

Target frequency: {a.freq / 1e6:.6f} MHz
Receiver context:
{context_summary(ctx)}

The receiver already decodes with {BUILTIN}.

A {a.seconds:g} s capture was made: {cap} (complex float32, sidecar capture.json). Its automatic
analysis (PNG plots next to it - Read them, the waterfall is especially useful):
--------------------------------------------------------------------------------
{report}
--------------------------------------------------------------------------------

Tools (run with Bash):
{sigtool_help()}
You can also write numpy scripts into {sdir} and run them. Capture again (longer, other rate,
other offset) if the signal was weak or absent; intermittent signals may need 30-60 s.
{("Extra instructions from the user: " + a.instructions) if a.instructions else ""}

Deliver a Markdown report (it is shown to the user):
## Identification - what the signal is (system/standard/protocol, service, typical users) and your confidence
## Evidence - the measured parameters behind it (modulation, symbol rate, deviation/levels, bandwidth,
   burst timing, sync words / framing, anything decoded)
## Decoding - whether one of the receiver's decoder plugins handles it (which mode to pick in the
   VFO's Digital decoder list), otherwise what a decoder needs (demodulation, sync, FEC/CRC, framing) and
   what information it could show
## Notes - anything else useful (encryption, voice codec, legal/privacy remarks)
End with exactly these two lines:
SIGNAL_NAME: <short name>
SUGGESTED_PLUGIN_ID: <id: lowercase a-z 0-9 _ -, max 32 chars, or "none" if an existing decoder covers it>"""

    new_id = str(uuid.uuid4())
    runner = Runner(cfg, sdir, int(cfg.get("investigate_timeout_s", 900)))
    install_term_handler(runner)
    emit("status", state="thinking", msg="the AI is analysing the signal")
    ok, text = runner.run(prompt, base_tools(sdir), new_id=new_id, system=SYSTEM.format(sdir=sdir))
    sess = session_load(sdir)
    if cfg.get("backend", "claude") == "claude":
        sess["claude_session"] = new_id
    name = re.search(r"^SIGNAL_NAME:\s*(.+)$", text, re.M)
    pid = re.search(r"^SUGGESTED_PLUGIN_ID:\s*([A-Za-z0-9_-]+)", text, re.M)
    sess["signal_name"] = name.group(1).strip() if name else ""
    sug = pid.group(1).strip().lower() if pid else ""
    sess["suggested_plugin_id"] = sug if ID_RE.match(sug) and sug != "none" else ""
    sess["analysis"] = text
    sess["state"] = "investigated" if ok else "failed"
    session_save(sdir, sess)
    with open(os.path.join(sdir, "analysis.md"), "w") as f:
        f.write(text)
    chat_append(sdir, "assistant", "investigate", text or "(the investigation failed - see the agent activity)")
    emit("done", kind="investigate", ok=ok, analysis=text, signal_name=sess["signal_name"],
         plugin_id=sess["suggested_plugin_id"], session=os.path.basename(sdir))
    sys.exit(0 if ok else 1)


def plugin_ok(pid):
    exe = os.path.join(PLUGIN_DIR, pid, "build", "decoder")
    if not os.access(exe, os.X_OK):
        return False, "not built"
    try:
        p = subprocess.run([exe, "--info"], capture_output=True, text=True, timeout=10)
        j = json.loads(p.stdout)
        if j.get("id") != pid:
            return False, f"--info reports id {j.get('id')!r}"
        return True, j.get("name", pid)
    except (OSError, subprocess.SubprocessError, ValueError) as e:
        return False, str(e)


def cmd_create(a):
    cfg = load_config()
    require_enabled(cfg)
    sdir = os.path.abspath(a.session)
    sess = session_load(sdir)
    if not sess:
        emit("error", msg="no investigation in this session - investigate a signal first")
        sys.exit(1)
    pid = a.plugin.strip().lower()
    if not ID_RE.match(pid) or pid == "sdk":
        emit("error", msg="plugin id must be 1-32 characters a-z 0-9 _ - (not 'sdk')")
        sys.exit(1)
    pdir = os.path.join(PLUGIN_DIR, pid)
    exists = os.path.exists(os.path.join(pdir, "decoder.cpp"))
    os.makedirs(pdir, exist_ok=True)
    set_session(sdir)
    chat_append(sdir, "user", "create", ("Improve" if exists else "Create") + f" the decoder plugin \"{pid}\"" +
                (f"\n\n{a.instructions}" if a.instructions else ""))
    cap = os.path.join(sdir, "capture.cf32")
    caps = sorted(f for f in os.listdir(sdir) if f.endswith(".cf32"))
    sess.setdefault("instructions", [])
    if a.instructions:
        sess["instructions"].append(a.instructions)
    sess["plugin_id"] = pid
    sess["state"] = "creating"
    session_save(sdir, sess)

    task = ("IMPROVE the existing decoder plugin" if exists else "WRITE a new decoder plugin")
    prompt = f"""{task} for the signal you investigated ({sess.get('signal_name') or 'see your analysis'}).

Plugin id: {pid}
Plugin directory (the only place you may write code): {pdir}/
  decoder.cpp (required; KRAKEN_PLUGIN(..., {{.id = "{pid}", ...}}) - the id MUST be "{pid}"),
  optional extra .cpp/.hpp files, and README.md (what the signal is, what is decoded, the sources /
  specifications used, test results, limitations).
Read first: {PLUGIN_DIR}/SDK.md (the API and rules) and the example {PLUGIN_DIR}/pocsag/decoder.cpp.
The other plugins in {PLUGIN_DIR}/ are further examples (p25, dmr, nxdn, dstar, tetra, mpt1327, aprs),
and {PLUGIN_DIR}/lib/ is a shared library you may use (FEC/CRC: dig_fec.hpp, 4FSK sync: dig_fsk4.hpp,
vocoders: dig_vocoder.hpp, front ends: dig_common.hpp).

Build:  make -C {PLUGIN_DIR} PLUGIN={pid}
Test:   {pdir}/build/decoder --file <capture.cf32> [--offset HZ] [--verbose]
Captures in this session: {", ".join(os.path.join(sdir, c) for c in caps) or cap}
(make new ones with sigtool capture if needed - e.g. a longer one with more traffic).

Iterate until the decoder works on the captures: frames are found and pass their checks
(VALID FRAMES > 0), the facts/events are meaningful, and it reports nothing on noise
(test e.g. on a capture offset to an empty frequency). Report through host.fact / host.event /
host.valid only what passed a check. Keep it efficient (it runs live on a Raspberry Pi
next to the receiver: target well under 10% of real time in the test summary).
{("User instructions: " + chr(10).join(sess["instructions"])) if sess["instructions"] else ""}

Finish with a short Markdown summary for the user (what it decodes, test results,
limitations, how to use it: tick Digital decoder on a VFO and pick it in that VFO's decoder list), and end with exactly
one line:  PLUGIN_READY: {pid}   - or -   PLUGIN_FAILED: <reason>"""

    tools = base_tools(sdir) + [
        f"Edit({rel(pdir)}/**)",
        f"Bash(make -C {PLUGIN_DIR} PLUGIN={pid}:*)",
        f"Bash({pdir}/build/decoder:*)",
    ]
    runner = Runner(cfg, sdir, int(cfg.get("create_timeout_s", 3600)))
    install_term_handler(runner)
    emit("status", state="coding", msg=("improving" if exists else "writing") + f" plugin {pid}")
    resume = sess.get("claude_session") if cfg.get("backend", "claude") == "claude" else None
    if cfg.get("backend", "claude") != "claude":
        prompt = "Your earlier analysis:\n" + sess.get("analysis", "") + "\n\n" + prompt
    new_id = None if resume else str(uuid.uuid4())
    ok, text = runner.run(prompt, tools, resume_id=resume, new_id=new_id, system=SYSTEM.format(sdir=sdir))
    if new_id and cfg.get("backend", "claude") == "claude":
        s2 = session_load(sdir)
        s2["claude_session"] = new_id
        session_save(sdir, s2)
    emit("status", state="checking", msg=f"checking plugin {pid}")
    # build it ourselves too: the agent may have stopped after editing
    b = subprocess.run(["make", "-C", PLUGIN_DIR, f"PLUGIN={pid}"], capture_output=True, text=True)
    built, info = plugin_ok(pid)
    if b.returncode != 0:
        built = False
        info = "build failed: " + (b.stderr or b.stdout).strip()[-800:]
    ready = built and "PLUGIN_FAILED" not in text
    try:
        if not os.listdir(pdir):
            os.rmdir(pdir)      # the agent wrote nothing
    except OSError:
        pass
    sess = session_load(sdir)
    sess["state"] = "plugin ready" if ready else "plugin failed"
    sess.setdefault("history", []).append({"time": time.strftime("%Y-%m-%d %H:%M:%S"), "plugin": pid,
                                           "ok": ready, "summary": text[-4000:]})
    session_save(sdir, sess)
    chat_append(sdir, "assistant", "create", (text or "(no answer)") +
                ("" if ready else f"\n\n**Plugin not ready:** {info if not built else 'the agent reported a failure'}"))
    emit("done", kind="create", ok=ready, plugin_id=pid, summary=text, build=info if not built else "built",
         session=os.path.basename(sdir))
    sys.exit(0 if ready else 1)


def cmd_ask(a):
    cfg = load_config()
    require_enabled(cfg)
    sdir = os.path.abspath(a.session)
    sess = session_load(sdir)
    if not sess:
        emit("error", msg="no investigation in this session")
        sys.exit(1)
    runner = Runner(cfg, sdir, int(cfg.get("investigate_timeout_s", 900)))
    install_term_handler(runner)
    resume = sess.get("claude_session") if cfg.get("backend", "claude") == "claude" else None
    prompt = a.text if resume else "Your earlier analysis:\n" + sess.get("analysis", "") + "\n\nQuestion: " + a.text
    set_session(sdir)
    chat_append(sdir, "user", "ask", a.text)
    emit("status", state="thinking", msg="asking a follow-up question")
    ok, text = runner.run(prompt, base_tools(sdir), resume_id=resume, system=SYSTEM.format(sdir=sdir))
    chat_append(sdir, "assistant", "ask", text or "(no answer - see the agent activity)")
    emit("done", kind="ask", ok=ok, text=text, session=os.path.basename(sdir))


def cmd_delete(a):
    """Removes a session: its folder (captures, plots, chat) and the Claude
    Code transcript(s) of its conversation"""
    sdir = os.path.realpath(a.session)
    if os.path.dirname(sdir) != os.path.realpath(SESSIONS) or not os.path.isdir(sdir):
        print(json.dumps({"ok": False, "error": "not a session folder"}))
        return 1
    sess = session_load(sdir)
    removed = 0
    cid = sess.get("claude_session") or ""
    if re.fullmatch(r"[0-9a-f-]{36}", cid):
        proj = os.path.expanduser("~/.claude/projects")
        try:
            for d in os.listdir(proj):
                for name in (cid + ".jsonl",):
                    f = os.path.join(proj, d, name)
                    if os.path.isfile(f):
                        os.remove(f)
                        removed += 1
                sub = os.path.join(proj, d, cid)   # per-session tool output
                if os.path.isdir(sub):
                    shutil.rmtree(sub, ignore_errors=True)
        except OSError:
            pass
    shutil.rmtree(sdir, ignore_errors=True)
    print(json.dumps({"ok": not os.path.exists(sdir), "transcripts": removed}))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup")
    s.add_argument("--backend", choices=["claude", "command"])
    s.add_argument("--command", help="backend 'command': the CLI to run, prompt on stdin")
    s.add_argument("--model")
    s.add_argument("--yes", action="store_true")
    sub.add_parser("disable")
    sub.add_parser("status")
    sub.add_parser("test")
    i = sub.add_parser("investigate")
    i.add_argument("--session", required=True)
    i.add_argument("--freq", type=float, required=True)
    i.add_argument("--rate", type=float, default=0)
    i.add_argument("--seconds", type=float, default=10)
    i.add_argument("--context-json", default="")
    i.add_argument("--instructions", default="")
    i.add_argument("--capture-file", default="", help="analyse this .cf32 recording instead of capturing")
    c = sub.add_parser("create")
    c.add_argument("--session", required=True)
    c.add_argument("--plugin", required=True)
    c.add_argument("--instructions", default="")
    q = sub.add_parser("ask")
    q.add_argument("--session", required=True)
    q.add_argument("--text", required=True)
    d = sub.add_parser("delete")
    d.add_argument("--session", required=True)
    a = ap.parse_args()
    r = {"setup": cmd_setup, "disable": cmd_disable, "status": cmd_status, "test": cmd_test,
         "investigate": cmd_investigate, "create": cmd_create, "ask": cmd_ask, "delete": cmd_delete}[a.cmd](a)
    sys.exit(r or 0)


if __name__ == "__main__":
    main()
