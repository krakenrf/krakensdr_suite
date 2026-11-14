#!/usr/bin/env python3
#
# scripts/build_page.py - the temporary web page shown on the web UI ports
# while run.sh builds heimdall and kraken_doa (scripts/autobuild.sh), so a
# user who opens the usual URL of a headless Pi sees the build progress
# instead of a connection error:
#
#   python3 scripts/build_page.py --status STATUS.json [--http PORT]
#       [--https PORT] [--cert FILE --key FILE] [--pidfile FILE]
#
#   --status   the JSON progress file the build script rewrites about once a
#              second: {"state": "building" | "done" | "failed", "percent",
#              "phase", "step", "done", "total", "started", "updated",
#              "error", "log_tail": [...]}. Re-read per request; a missing or
#              half-written file reads as {"state": "starting"}.
#   --http     plain-HTTP port (run.sh: 8070, heimdall's web UI)
#   --https    HTTPS port (run.sh: 8080, kraken_doa's web UI)
#   --cert / --key
#              the TLS pair for --https; default kraken_doa_v2/server.crt +
#              server.key, kraken_doa's own pair, so a browser's certificate
#              exception carries over to the real UI. A missing pair is
#              made like kraken_doa makes it (tls_ensure_self_signed_cert in
#              kraken_doa_v2/src/utils/tls_compat.cpp) with the openssl CLI;
#              kraken_doa then finds and keeps it. No pair -> no HTTPS.
#   --pidfile  written once the ports are bound, removed on exit
#
# Routes: GET /__build/status -> the status JSON; any other GET / HEAD path
# -> the page (scripts/build_page.html, read once at startup); other methods
# -> 405; WebSocket upgrades (from a tab of the real UI) -> 503. Every
# response carries "X-KrakenSDR-Build: 1" - the page reloads itself once its
# URL answers without it (the real UI is up) - and "Cache-Control: no-store".
# Nothing is served from disk and no request data is echoed.
#
# Binds every interface like the apps (uSockets: one dual-stack IPv6 socket,
# IPv4 only where IPv6 is unavailable), with SO_REUSEADDR and no
# SO_REUSEPORT. Exits 1 at once if no requested port binds; serves the other
# one (with a warning) if only one does. Runs until SIGTERM / SIGINT and then
# closes its sockets and exits within about half a second, so the real app
# can bind the port right after.
#
# Python 3 standard library only (Debian 12 / 13, Raspberry Pi OS, macOS).
# Information goes to stdout, faults to stderr.

import argparse
import errno
import json
import os
import select
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

HEADER_LIMIT = 16384        # request line + headers
MAX_CONNECTIONS = 256       # concurrent client threads; more are dropped
IO_TIMEOUT = 10.0           # per socket operation (TLS handshake, reads, writes)
REQUEST_DEADLINE = 15.0     # whole request head, against slow-drip clients
STATUS_FILE_LIMIT = 1 << 20

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)

REASONS = {200: "OK", 400: "Bad Request", 405: "Method Not Allowed",
           431: "Request Header Fields Too Large", 503: "Service Unavailable"}

COMMON_HEADERS = (
    "X-KrakenSDR-Build: 1\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Referrer-Policy: no-referrer\r\n"
)
PAGE_CSP = ("Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
            "script-src 'unsafe-inline'; connect-src 'self'; img-src data:; "
            "base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n")

stop_event = threading.Event()


def info(msg):
    try:
        print("build page: " + msg, flush=True)
    except (OSError, ValueError):
        pass


def warn(msg):
    try:
        print("build page: " + msg, file=sys.stderr, flush=True)
    except (OSError, ValueError):
        pass


# --- status file ---------------------------------------------------------------

def _int(v, lo=None, hi=None):
    if isinstance(v, bool) or not isinstance(v, (int, float)) or v != v or v in (float("inf"), float("-inf")):
        return None
    v = int(v)
    if lo is not None:
        v = max(lo, v)
    if hi is not None:
        v = min(hi, v)
    return v


def _text(v, limit=2000):
    if v is None:
        return ""
    return str(v)[:limit]


def read_status(path):
    """The status as a JSON body; defaults when the file is missing, being
    rewritten or not valid JSON. Only the known fields are passed on, with
    their types checked, plus "now" (this machine's clock - the page uses it
    for elapsed / stale times, so a browser with another clock still shows
    the right ones)."""
    raw = None
    try:
        with open(path, "rb") as f:
            data = f.read(STATUS_FILE_LIMIT + 1)
        if len(data) <= STATUS_FILE_LIMIT:
            raw = json.loads(data.decode("utf-8", "replace"))
    except (OSError, ValueError, RecursionError):
        raw = None
    if not isinstance(raw, dict):
        raw = {}
    state = raw.get("state")
    if state not in ("building", "done", "failed"):
        state = "starting"
    out = {
        "state": state,
        "percent": _int(raw.get("percent"), 0, 100) or 0,
        "phase": _text(raw.get("phase"), 200),
        "step": _text(raw.get("step"), 500),
        "done": _int(raw.get("done"), 0) or 0,
        "total": _int(raw.get("total"), 0) or 0,
        "started": _int(raw.get("started"), 0) or 0,
        "updated": _int(raw.get("updated"), 0) or 0,
        "error": _text(raw.get("error"), 2000),
        "log_tail": [],
        "now": int(time.time()),
    }
    tail = raw.get("log_tail")
    if isinstance(tail, list):
        out["log_tail"] = [_text(x, 2000) for x in tail[-50:]]
    return json.dumps(out).encode("utf-8")


# --- HTTP ------------------------------------------------------------------------

def respond(conn, code, ctype=None, body=b"", head_only=False, extra=""):
    hdr = "HTTP/1.1 %d %s\r\n" % (code, REASONS.get(code, "Error")) + COMMON_HEADERS + extra
    if ctype:
        hdr += "Content-Type: %s\r\n" % ctype
    hdr += "Content-Length: %d\r\n\r\n" % len(body)
    conn.sendall(hdr.encode("ascii") + (b"" if head_only else body))


def read_head(conn):
    """Request line + headers (bytes), b"" on EOF, None when too large."""
    buf = b""
    deadline = time.monotonic() + REQUEST_DEADLINE
    while b"\r\n\r\n" not in buf and b"\n\n" not in buf:
        if len(buf) > HEADER_LIMIT:
            return None
        if time.monotonic() > deadline:
            raise socket.timeout("request too slow")
        chunk = conn.recv(4096)
        if not chunk:
            return b"" if not buf.strip() else buf
        buf += chunk
    return buf if len(buf) <= HEADER_LIMIT else None


def drain(conn):
    """Read what is left of the request (a POST body) for a moment before
    closing, so the close doesn't reset the connection under the response."""
    try:
        conn.settimeout(0.2)
        got = 0
        while got < 65536:
            chunk = conn.recv(8192)
            if not chunk:
                break
            got += len(chunk)
    except (OSError, ValueError):
        pass


def handle_request(conn, page, status_path):
    head = read_head(conn)
    if head is None:
        respond(conn, 431, "text/plain; charset=utf-8", b"Request header too large\n")
        drain(conn)
        return
    if not head:
        return
    lines = head.split(b"\n")
    parts = lines[0].strip().split()
    if len(parts) != 3 or not parts[2].startswith(b"HTTP/"):
        respond(conn, 400, "text/plain; charset=utf-8", b"Bad request\n")
        drain(conn)
        return
    method, target = parts[0], parts[1]
    headers = {}
    for line in lines[1:]:
        k, sep, v = line.partition(b":")
        if sep:
            headers[k.strip().lower()] = v.strip().lower()

    if b"websocket" in headers.get(b"upgrade", b""):
        # an open tab of the real UI reconnecting: no 101, it retries later
        respond(conn, 503, "text/plain; charset=utf-8",
                b"KrakenSDR is being built - the web UI starts when the build is done\n",
                extra="Retry-After: 5\r\n")
        drain(conn)
        return
    if method not in (b"GET", b"HEAD"):
        respond(conn, 405, "text/plain; charset=utf-8", b"Method not allowed\n",
                head_only=method == b"HEAD", extra="Allow: GET, HEAD\r\n")
        drain(conn)
        return
    head_only = method == b"HEAD"
    path = target.split(b"?", 1)[0].split(b"#", 1)[0]
    if path.startswith(b"http://") or path.startswith(b"https://"):   # absolute form
        path = b"/" + path.split(b"/", 3)[3] if path.count(b"/") >= 3 else b"/"
    if path == b"/__build/status":
        respond(conn, 200, "application/json", read_status(status_path), head_only)
    else:
        respond(conn, 200, "text/html; charset=utf-8", page, head_only, extra=PAGE_CSP)


def serve_connection(conn, tls_ctx, page, status_path, slots):
    try:
        conn.settimeout(IO_TIMEOUT)
        if tls_ctx is not None:
            # the handshake runs here, in the connection's own thread: plain
            # HTTP on this port or a rejected certificate only ends this one
            conn = tls_ctx.wrap_socket(conn, server_side=True)
        handle_request(conn, page, status_path)
    except (OSError, ValueError, ssl.SSLError, socket.timeout):
        pass
    except Exception as e:   # never let one client take the server down
        warn("connection error: %s" % e.__class__.__name__)
    finally:
        try:
            conn.close()
        except OSError:
            pass
        slots.release()


# --- sockets ---------------------------------------------------------------------

def bind_port(port):
    """A listening socket on every interface: dual-stack IPv6 like uSockets,
    IPv4 where IPv6 is unavailable. Raises OSError."""
    if socket.has_ipv6:
        s = None
        try:
            s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            s.bind(("::", port))
            s.listen(128)
            s.setblocking(False)
            return s
        except OSError as e:
            if s is not None:
                s.close()
            if e.errno not in (errno.EAFNOSUPPORT, errno.EADDRNOTAVAIL, errno.EPROTONOSUPPORT,
                               errno.EINVAL, errno.ENOPROTOOPT):
                raise
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", port))
        s.listen(128)
        s.setblocking(False)
    except OSError:
        s.close()
        raise
    return s


# --- TLS -------------------------------------------------------------------------

# kraken_doa's own certificate (tls_ensure_self_signed_cert): RSA 2048,
# SHA-256, v3, subject = issuer = CN=krakensdr, 10 years, an end-entity
# certificate with the names in the SAN (browsers want them there; Firefox
# refuses a CA certificate as a server's)
OPENSSL_CONF = """\
[req]
distinguished_name = dn
x509_extensions = v3_server
prompt = no
[dn]
CN = krakensdr
[v3_server]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
subjectAltName = DNS:krakensdr,DNS:localhost,IP:127.0.0.1,IP:::1
"""


def nonempty(path):
    try:
        return os.path.getsize(path) > 0
    except OSError:
        return False


def make_cert(cert, key):
    """Make the pair the way kraken_doa does when it is missing: key 0600,
    certificate 0644; the key goes into place first and the certificate last,
    so an interrupted run leaves no certificate rather than one that doesn't
    match its key. An existing pair is never replaced. True when a usable
    pair is there."""
    if nonempty(cert) and nonempty(key):
        return True
    openssl = shutil.which("openssl")
    if not openssl:
        warn("no openssl command to make %s + %s" % (cert, key))
        return False
    d = os.path.dirname(os.path.abspath(cert))
    tmp = None
    try:
        tmp = tempfile.mkdtemp(prefix=".build_page_cert.", dir=d)   # 0700
        conf = os.path.join(tmp, "openssl.cnf")
        # *.key / *.crt: what .gitignore keeps out of git, should a kill
        # leave this folder behind
        tkey, tcert = os.path.join(tmp, "tmp.key"), os.path.join(tmp, "tmp.crt")
        with open(conf, "w") as f:
            f.write(OPENSSL_CONF)
        r = subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256",
                            "-days", "3650", "-config", conf, "-keyout", tkey, "-out", tcert],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, timeout=120)
        if r.returncode != 0 or not nonempty(tkey) or not nonempty(tcert):
            err = r.stderr.decode("utf-8", "replace").strip().splitlines()
            warn("openssl could not make the certificate%s" % (": " + err[-1] if err else ""))
            return False
        os.chmod(tkey, 0o600)
        os.chmod(tcert, 0o644)
        if os.path.exists(cert):
            os.remove(cert)
        os.replace(tkey, key)
        os.replace(tcert, cert)
        info("created a self-signed certificate for the web UI (%s + %s)" % (cert, key))
        return True
    except (OSError, subprocess.SubprocessError) as e:
        warn("cannot make the certificate: %s" % e)
        return False
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


def tls_context(cert, key):
    if not make_cert(cert, key):
        return None
    try:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_cert_chain(cert, key)
        return ctx
    except (OSError, ssl.SSLError, ValueError) as e:
        warn("cannot use %s + %s: %s" % (cert, key, e))
        return None


# --- main ------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Temporary build-progress web page for run.sh.")
    ap.add_argument("--status", required=True, help="the build status JSON file")
    ap.add_argument("--http", type=int, metavar="PORT", help="plain-HTTP port")
    ap.add_argument("--https", type=int, metavar="PORT", help="HTTPS port")
    ap.add_argument("--cert", default=os.path.join(ROOT_DIR, "kraken_doa_v2", "server.crt"))
    ap.add_argument("--key", default=os.path.join(ROOT_DIR, "kraken_doa_v2", "server.key"))
    ap.add_argument("--pidfile")
    args = ap.parse_args()
    if not args.http and not args.https:
        ap.error("give --http and / or --https")

    try:
        with open(os.path.join(SCRIPT_DIR, "build_page.html"), "rb") as f:
            page = f.read()
    except OSError as e:
        warn("cannot read build_page.html: %s" % e)
        return 1

    def on_signal(signum, frame):
        stop_event.set()
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    if hasattr(signal, "SIGHUP"):
        signal.signal(signal.SIGHUP, on_signal)

    listeners = []   # (socket, tls context or None, label)
    if args.http:
        try:
            listeners.append((bind_port(args.http), None, "http://*:%d" % args.http))
        except OSError as e:
            warn("cannot listen on port %d: %s" % (args.http, e.strerror or e))
    if args.https:
        ctx = tls_context(args.cert, args.key)
        if ctx is None:
            warn("no TLS certificate - not serving HTTPS on port %d" % args.https)
        else:
            try:
                listeners.append((bind_port(args.https), ctx, "https://*:%d" % args.https))
            except OSError as e:
                warn("cannot listen on port %d: %s" % (args.https, e.strerror or e))
    if not listeners:
        warn("no port to serve the build page on")
        return 1
    requested = (1 if args.http else 0) + (1 if args.https else 0)
    if len(listeners) < requested:
        warn("serving the build page on %s only" % listeners[0][2])

    if args.pidfile:
        try:
            with open(args.pidfile, "w") as f:
                f.write("%d\n" % os.getpid())
        except OSError as e:
            warn("cannot write %s: %s" % (args.pidfile, e))
    info("serving the build progress on %s" % ", ".join(l[2] for l in listeners))

    slots = threading.BoundedSemaphore(MAX_CONNECTIONS)
    by_fd = {l[0].fileno(): l for l in listeners}
    try:
        while not stop_event.is_set():
            try:
                ready, _, _ = select.select(list(by_fd), [], [], 0.25)
            except InterruptedError:
                continue
            for fd in ready:
                lsock, ctx, _ = by_fd[fd]
                try:
                    conn, _addr = lsock.accept()
                except (BlockingIOError, InterruptedError, ConnectionAbortedError):
                    continue
                except OSError as e:
                    if e.errno in (errno.EMFILE, errno.ENFILE, errno.ENOBUFS, errno.ENOMEM):
                        time.sleep(0.05)   # out of descriptors: let clients finish
                    continue
                if not slots.acquire(blocking=False):
                    conn.close()           # too many clients at once
                    continue
                try:
                    threading.Thread(target=serve_connection, daemon=True,
                                     args=(conn, ctx, page, args.status, slots)).start()
                except RuntimeError:
                    conn.close()
                    slots.release()
    finally:
        for lsock, _, _ in listeners:
            try:
                lsock.close()
            except OSError:
                pass
        if args.pidfile:
            try:
                with open(args.pidfile) as f:
                    mine = f.read().strip() == str(os.getpid())
                if mine:
                    os.remove(args.pidfile)
            except OSError:
                pass
    info("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
