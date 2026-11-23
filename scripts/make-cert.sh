#!/usr/bin/env bash
#
# scripts/make-cert.sh - the HTTPS certificate of kraken_doa's web UI (8080)
#
#   scripts/make-cert.sh <dir> [--force] [--new-ca]
#
# Implements kraken_doa_v2/docs/dev/https-certificate.md, the contract this
# script shares with kraken_doa's in-process generator
# (kraken_doa_v2/src/utils/tls_compat.cpp, tlscert::ensure) and the page:
# change them together - each must find the other's files `kept`. In short,
# each receiver has its own small certificate authority (krakensdr-ca.key /
# krakensdr-ca.crt), which users install once per device. It signs the
# server certificate (server.crt = the server certificate followed by the CA
# certificate), which is re-issued here whenever the names or addresses
# change or it gets old - always for the same server.key - without anyone
# having to install anything again. The CA itself is replaced only when it
# can't vouch for a wanted name (a rename, an avahi conflict name, a new
# search domain), when the scope in cert_options differs from its own, when
# it expires within a year, or with --new-ca; the old CA certificate is kept
# as krakensdr-ca.YYYYMMDD.retired.crt (its key deleted) so the page can
# name what devices should remove. A certificate of the user's own (or a
# symlinked server.crt / server.key) is never touched.
#
#   <dir>      kraken_doa's working directory (kraken_doa_v2, /data/kraken_doa)
#   --force    re-issue the server certificate (same key, same CA)
#   --new-ca   make a new CA (every device then installs it again)
#
# Environment: KRAKEN_CERT_HOST (the host name, instead of the one avahi
# announces / hostname -s / scutil), KRAKEN_CERT_NAMES (more DNS names,
# comma-separated), OPENSSL (the openssl command to use); for tests only:
# KRAKEN_CERT_RESOLV_CONF (read instead of /etc/resolv.conf).
#
# Output (stdout): "reason: <why>" for issued / ca-created / not-managed and
# "retired: <file>" when a CA was replaced, then as the LAST line:
#   kept         nothing changed
#   issued       the server certificate was (re-)issued, the CA is unchanged:
#                devices that installed the CA need nothing
#   ca-created   a new CA: every device must install it (and remove the
#                retired one); "reason: first certificate" on a fresh install
#   not-managed  a certificate of the user's own: left alone
# Exit 0 in all four cases. Failure: exit 1, the reason on stderr, every
# file as it was (everything is made in a temporary folder in <dir> first
# and moved into place at the end; keys 0600, certificates 0644). Two runs
# at once take turns (<dir>/.make-cert.lock, waited for up to 10 s; one
# older than 120 s is a dead run's and taken over). The `kept` path makes no
# key and costs six openssl calls.
#
# Portable: bash 3.2 (macOS: no arrays, no [[, no ${x,,}, no mapfile - it
# runs under dash as well), OpenSSL 1.1.1 / 3.x / LibreSSL - only its own
# config files (-config, -extfile), never -addext on top of the system's
# openssl.cnf (1.1.1 then added a second Basic Constraints extension), and
# names / constraints read back with x509 -text (1.1.1 and LibreSSL have no
# x509 -ext). Certificates are signed with `openssl ca`, the one way all of
# them can set the start date (an hour back, like kraken_doa's generator: a
# device clock a little behind still accepts a certificate made just now).

set -u
set -f          # no pathname expansion: the lists below are split on spaces
umask 077       # keys and the work folders; certificates are chmod 644

me=make-cert.sh
usage="usage: $me <dir> [--force] [--new-ca]"
fail() { echo "$me: $*" >&2; exit 1; }
note() { echo "$me: $*" >&2; }

dir=""; force=0; new_ca=0
for a in "$@"; do
    case "$a" in
    --force)   force=1 ;;
    --new-ca)  new_ca=1 ;;
    -h|--help) awk 'NR > 2 && !/^#/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
    -*)        fail "unknown option $a ($usage)" ;;
    *)         [ -z "$dir" ] || fail "one directory only ($usage)"; dir=$a ;;
    esac
done
[ -n "$dir" ] || fail "$usage"
[ -d "$dir" ] || fail "$dir is not a directory"
D=$(cd "$dir" 2>/dev/null && pwd) || fail "cannot enter $dir"
[ "$D" != / ] || D=""
CA_KEY="$D/krakensdr-ca.key"; CA_CRT="$D/krakensdr-ca.crt"
KEY="$D/server.key"; CRT="$D/server.crt"; OPTS="$D/cert_options"
LOCK="$D/.make-cert.lock"
RESOLV=${KRAKEN_CERT_RESOLV_CONF:-/etc/resolv.conf}
OS=$(uname -s 2>/dev/null) || OS=unknown

# --- openssl ------------------------------------------------------------------
OSSL=${OPENSSL:-}
if [ -z "$OSSL" ] && [ "$OS" = Darwin ]; then
    # macOS's own openssl is LibreSSL; install.sh installs Homebrew's openssl@3
    for c in /opt/homebrew/opt/openssl@3/bin/openssl /usr/local/opt/openssl@3/bin/openssl; do
        if [ -x "$c" ]; then OSSL=$c; break; fi
    done
fi
[ -n "$OSSL" ] || OSSL=$(command -v openssl 2>/dev/null) || OSSL=""
VER=""
[ -z "$OSSL" ] || VER=$("$OSSL" version 2>/dev/null </dev/null) || VER=""
[ -n "$VER" ] || fail "no openssl command (it makes the web UI's certificate) - nothing was changed"
ossl() { "$OSSL" "$@" </dev/null; }

# verify options: only our CA counts, not the system's trust store as well;
# -no_check_time for the managed check (an expired certificate of ours is
# still ours). By the version where it is known, else by verify -help
case "$VER" in
"OpenSSL 1.1."*) V_ONLY="-no-CApath"; V_NOTIME="-no_check_time" ;;
"OpenSSL "[3-9]*) V_ONLY="-no-CApath -no-CAstore"; V_NOTIME="-no_check_time" ;;
*)  vhelp=$(ossl verify -help 2>&1); V_ONLY=""; V_NOTIME=""
    case "$vhelp" in *-no-CApath*) V_ONLY="-no-CApath" ;; esac
    case "$vhelp" in *-no-CAstore*) V_ONLY="$V_ONLY -no-CAstore" ;; esac
    case "$vhelp" in *-no_check_time*) V_NOTIME="-no_check_time" ;; esac ;;
esac

WORK=$(mktemp -d "${TMPDIR:-/tmp}/make-cert.XXXXXX" 2>/dev/null) || fail "cannot make a temporary folder"
STAGE=""; LOCKED=0
cleanup() {
    rm -rf "$WORK"
    if [ -n "$STAGE" ]; then rm -rf "$STAGE"; fi
    if [ $LOCKED = 1 ]; then rm -rf "$LOCK"; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# must <what> <openssl arguments...>: run openssl in the stage folder (`ca`
# keeps its database there), or fail with its last error
must() {
    local what=$1; shift
    ( cd "$STAGE" && "$OSSL" "$@" ) </dev/null >"$WORK/out" 2>"$WORK/err" && return 0
    fail "openssl could not $what: $(grep -v '^[[:space:]]*$' "$WORK/err" | tail -n 1) - nothing was changed"
}
nap() { sleep 0.2 2>/dev/null || sleep 1; }

# --- one run at a time in <dir> -----------------------------------------------
# mkdir is atomic everywhere; the folder holds the time it was taken. A
# folder that can't be made at all (read-only <dir>): no lock - nothing can be
# written there anyway
take_lock() {
    local t0 now since
    t0=$(date +%s)
    while :; do
        if mkdir "$LOCK" 2>/dev/null; then
            LOCKED=1
            date +%s > "$LOCK/since" 2>/dev/null
            return 0
        fi
        [ -d "$LOCK" ] || return 0
        now=$(date +%s)
        since=$(cat "$LOCK/since" 2>/dev/null) || since=""
        case "$since" in ""|*[!0-9]*) since="" ;; esac
        # a run that was killed: taken over (also one that died before it
        # wrote its time, once the wait is over)
        if { [ -n "$since" ] && [ $((now - since)) -gt 120 ]; } ||
           { [ -z "$since" ] && [ $((now - t0)) -ge 10 ]; }; then
            rm -rf "$LOCK" 2>/dev/null
            [ -d "$LOCK" ] || continue
            return 0        # can't be removed (read-only <dir>): nobody can write there
        fi
        [ $((now - t0)) -lt 10 ] || fail "another make-cert.sh is working in ${D:-/} (for 10 s) - nothing was changed"
        nap
    done
}
take_lock

# --- what is wanted: scope, host name, names, addresses ----------------------
lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }
trim() { printf '%s' "$1" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'; }
nodots() { local s=$1; while :; do case "$s" in *.) s=${s%.} ;; *) break ;; esac; done; printf '%s' "$s"; }
run_t() {       # run_t <seconds> <command...>: with a time limit where timeout(1) exists
    local t=$1; shift
    if command -v timeout >/dev/null 2>&1; then timeout "$t" "$@"; else "$@"; fi
}
# a DNS label: 1-63 of a-z 0-9 -, not starting or ending with - (as tlscert's valid_label)
valid_label() {
    case "$1" in ""|-*|*-|*[!a-z0-9-]*) return 1 ;; esac
    [ ${#1} -le 63 ]
}
# a DNS name: such labels joined by dots, 253 characters at most
valid_dns() {
    local rest=$1 l
    [ -n "$1" ] && [ ${#1} -le 253 ] || return 1
    while :; do
        l=${rest%%.*}
        valid_label "$l" || return 1
        [ "$l" = "$rest" ] && return 0
        rest=${rest#*.}
    done
}

# cert_options: the last line "scope=..." (the line and the value trimmed);
# anything but "wide" is narrow, as kraken_doa reads it
SCOPE=narrow
if [ -f "$OPTS" ]; then
    v=$(sed -n 's/^[[:space:]]*scope=//p' "$OPTS" 2>/dev/null | tail -n 1)
    v=$(trim "$v")
    case "$v" in
    wide) SCOPE=wide ;;
    narrow) ;;
    *) note "cert_options has no scope=narrow / scope=wide line - narrow scope" ;;
    esac
fi

# This machine's IPv4 addresses on up interfaces that aren't the loopback,
# one per line (127/8 and link-local 169.254/16 left out) - getifaddrs' view
local_ipv4s() {
    local out=""
    if command -v ip >/dev/null 2>&1; then
        out=$(ip -4 -o addr show up 2>/dev/null </dev/null |
              awk '$2 != "lo" { for (i = 1; i < NF; i++) if ($i == "inet") { a = $(i + 1); sub(/\/.*/, "", a); print a; break } }')
    fi
    if [ -z "$out" ] && command -v ifconfig >/dev/null 2>&1; then
        if [ "$OS" = Darwin ]; then out=$(ifconfig -u 2>/dev/null </dev/null); else out=$(ifconfig 2>/dev/null </dev/null); fi
        out=$(printf '%s\n' "$out" | awk '/^[^ \t]/ { lo = ($0 ~ /LOOPBACK|Loopback/) }
                                           $1 == "inet" && !lo { a = $2; sub(/^addr:/, "", a); print a }')
    fi
    if [ -z "$out" ] && [ "$OS" != Darwin ]; then
        out=$(hostname -I 2>/dev/null </dev/null | tr ' ' '\n')
    fi
    printf '%s\n' "$out" | awk -F. 'NF == 4 && $1 != 127 && !($1 == 169 && $2 == 254)'
}
# stdin: IPv4 addresses -> the ones inside the wide scope's private ranges
# (10/8, 172.16/12, 192.168/16, 100.64/10), each once. A public address is
# left out: a name outside the CA's constraints would fail the whole
# certificate
private_v4() {
    awk -F. 'NF == 4 && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ && $4 ~ /^[0-9]+$/ &&
             $1 + 0 <= 255 && $2 + 0 <= 255 && $3 + 0 <= 255 && $4 + 0 <= 255 &&
             ($1 == 10 || ($1 == 172 && $2 >= 16 && $2 <= 31) || ($1 == 192 && $2 == 168) ||
              ($1 == 100 && $2 >= 64 && $2 <= 127)) {
                 a = ($1 + 0) "." ($2 + 0) "." ($3 + 0) "." ($4 + 0); if (!seen[a]++) print a }'
}

# The name avahi announces: after an mDNS name conflict it is not hostname's
# (the second "krakensdr" on a network becomes "krakensdr-2"). Quietly
# tries what exists; prints nothing when none answers.
avahi_host() {
    local h="" s i a
    if command -v busctl >/dev/null 2>&1; then
        # still registering (1) or resolving a conflict (3): let it settle (2 = running)
        i=0
        while [ $i -lt 6 ]; do
            s=$(run_t 3 busctl --timeout=2 call org.freedesktop.Avahi / org.freedesktop.Avahi.Server GetState 2>/dev/null </dev/null) || break
            case "$s" in "i 1"|"i 3") sleep 0.5 2>/dev/null || sleep 1 ;; *) break ;; esac
            i=$((i + 1))
        done
        h=$(run_t 3 busctl --timeout=2 call org.freedesktop.Avahi / org.freedesktop.Avahi.Server GetHostName 2>/dev/null </dev/null |
            sed -n 's/^s "\(.*\)"$/\1/p')
    fi
    if [ -z "$h" ] && command -v gdbus >/dev/null 2>&1; then
        h=$(run_t 3 gdbus call --system --timeout 2 --dest org.freedesktop.Avahi --object-path / \
                --method org.freedesktop.Avahi.Server.GetHostName 2>/dev/null </dev/null |
            sed -n "s/^('\(.*\)',)\$/\1/p")
    fi
    if [ -z "$h" ] && command -v avahi-resolve >/dev/null 2>&1; then
        a=$(local_ipv4s | head -n 1)
        if [ -n "$a" ]; then
            h=$(run_t 3 avahi-resolve -a "$a" 2>/dev/null </dev/null | awk 'NF >= 2 { print $2; exit }')
            h=$(nodots "$h")
            case "$h" in *.local) h=${h%.local} ;; *) h="" ;; esac   # an mDNS answer only
        fi
    fi
    printf '%s' "$h"
}

if [ -n "${KRAKEN_CERT_HOST:-}" ]; then
    RAW=$(trim "$KRAKEN_CERT_HOST")         # as it is: a dot makes it unusable
else
    RAW=""
    if [ "$OS" = Darwin ]; then
        RAW=$(scutil --get LocalHostName 2>/dev/null </dev/null) || RAW=""
    else
        RAW=$(avahi_host)
    fi
    [ -n "$RAW" ] || RAW=$(hostname -s 2>/dev/null </dev/null) || RAW=""
    [ -n "$RAW" ] || RAW=$(hostname 2>/dev/null </dev/null) || RAW=""
    [ -n "$RAW" ] || RAW=$(uname -n 2>/dev/null) || RAW=""
    RAW=$(trim "${RAW%%.*}")
fi
HOST=$(lower "$RAW")
# not a DNS label: then only localhost (and in wide scope the addresses)
if valid_label "$HOST"; then USABLE=1; CN=$HOST; else USABLE=0; CN=localhost; fi
[ $USABLE = 1 ] || note "the host name '$RAW' is not usable in DNS - the certificate names only localhost"

NAMES=" "
add_name() { case "$NAMES" in *" $1 "*) ;; *) NAMES="$NAMES$1 " ;; esac; }
if [ $USABLE = 1 ]; then
    add_name "$HOST"
    add_name "$HOST.local"
    if [ -r "$RESOLV" ]; then
        for d in $(awk '$1 == "search" || $1 == "domain" { for (i = 2; i <= NF; i++) print $i }' "$RESOLV" 2>/dev/null); do
            d=$(nodots "$(lower "$d")")
            if [ "$d" != local ] && valid_dns "$d" && valid_dns "$HOST.$d"; then add_name "$HOST.$d"; fi
        done
    fi
fi
add_name localhost
# KRAKEN_CERT_NAMES: split at commas, each trimmed, lowercased, trailing dots off
if [ -n "${KRAKEN_CERT_NAMES:-}" ]; then
    rest="$KRAKEN_CERT_NAMES,"
    while [ -n "$rest" ]; do
        n=${rest%%,*}; rest=${rest#*,}
        n=$(nodots "$(lower "$(trim "$n")")")
        [ -n "$n" ] || continue
        if ! valid_dns "$n"; then
            note "KRAKEN_CERT_NAMES: '$n' is not a DNS name - left out"
            continue
        fi
        case "$n" in
        *[!0-9.]*) ;;
        *) note "KRAKEN_CERT_NAMES: $n is named as a DNS name - browsers don't match an address against it (wide scope names the addresses)" ;;
        esac
        add_name "$n"
    done
fi
IPS="127.0.0.1 ::1"
if [ $SCOPE = wide ]; then
    for a in $(local_ipv4s | private_v4); do IPS="$IPS $a"; done
fi
# the subjectAltName set wanted, one "DNS:x" / "IP:a" per line, sorted
WANT_SAN=$( { for n in $NAMES; do echo "DNS:$n"; done; for a in $IPS; do echo "IP:$a"; done; } | LC_ALL=C sort -u)
# the CA's permitted DNS subtrees
NC_DNS=$NAMES
[ $SCOPE = narrow ] || NC_DNS="${NAMES}local lan "

# --- reading certificates -----------------------------------------------------
# info <cert file> <days>: one openssl call -> I_OK, I_SUBJ, I_ISSUER (RFC 2253),
# I_PUB (its public key), I_VALID (still valid in <days> days; judged by the
# message: OpenSSL 3.0.2 exits 0 for "will expire" too), I_TEXT (x509 -text)
info() {
    I_TEXT=$(ossl x509 -in "$1" -noout -nameopt RFC2253 -subject -issuer -pubkey -checkend $(($2 * 86400)) -text 2>/dev/null)
    I_SUBJ=$(printf '%s\n' "$I_TEXT" | sed -n 's/^subject= *//p' | head -n 1)
    I_ISSUER=$(printf '%s\n' "$I_TEXT" | sed -n 's/^issuer= *//p' | head -n 1)
    I_PUB=$(printf '%s\n' "$I_TEXT" | sed -n '/-----BEGIN PUBLIC KEY-----/,/-----END PUBLIC KEY-----/p')
    case "$I_TEXT" in *"will not expire"*) I_VALID=1 ;; *) I_VALID=0 ;; esac
    if [ -n "$I_SUBJ" ] && [ -n "$I_PUB" ]; then I_OK=1; else I_OK=0; fi
}
key_pub() { ossl pkey -in "$1" -passin pass: -pubout 2>/dev/null; }
# subjectAltName of x509 -text (stdin) as sorted "DNS:x" / "IP:a" lines (::1 is printed in full)
san_of() {
    awk '/X509v3 Subject Alternative Name/ { getline; n = split($0, p, ",")
            for (i = 1; i <= n; i++) { s = p[i]; gsub(/^[ \t]+|[ \t]+$/, "", s); sub(/^IP Address:/, "IP:", s); print s }
            exit }' | sed 's/^IP:0:0:0:0:0:0:0:1$/IP:::1/' | LC_ALL=C sort -u
}
# the permitted subtrees of a CA's name constraints in x509 -text (stdin),
# "DNS:x" / "IP:a/mask" per line
permitted_of() {
    awk '/X509v3 Name Constraints/ { inside = 1; next }
        !inside { next }
        /^[ \t]*Permitted:/ { perm = 1; next }
        /^[ \t]*Excluded:/ { perm = 0; next }
        /^[ \t]*(DNS|IP|email|URI|DirName|othername|Registered ID):/ { if (perm) { s = $0; gsub(/^[ \t]+|[ \t]+$/, "", s); print s }; next }
        { exit }'
}
# dns_permitted <name> <DNS constraints, space-separated>: RFC 5280 / OpenSSL's
# nc_dns - the name itself or anything under it (".x": only under it). No DNS
# constraint at all = every name.
dns_permitted() {
    local c any=0
    for c in $2; do
        any=1
        case "$c" in
        .*) case "$1" in *"$c") return 0 ;; esac ;;
        *)  [ "$1" = "$c" ] && return 0
            case "$1" in *".$c") return 0 ;; esac ;;
        esac
    done
    [ $any = 0 ]
}
# verifies <cert> <ca> [notime]: issued by that CA, a server certificate inside
# its constraints - valid now, unless "notime"
verifies() {
    local t=""
    [ "${3:-}" != notime ] || t=$V_NOTIME
    ossl verify $V_ONLY $t -CAfile "$2" -purpose sslserver "$1" >/dev/null 2>&1
}
# blocks <file> <prefix>: its PEM certificates as <prefix>1.pem, 2, ... -> how many
blocks() {
    awk -v p="$2" '/-----BEGIN CERTIFICATE-----/ { n++; f = p n ".pem" }
        f != "" { print > f }
        /-----END CERTIFICATE-----/ { if (f != "") close(f); f = "" }
        END { print n + 0 }' "$1" 2>/dev/null || echo 0
}
b64() { sed '/-----/d' "$1" 2>/dev/null | tr -d ' \t\r\n'; }   # a PEM block's base64, for comparing
# fix_mode <file>: tighten a key left readable by others
fix_mode() { case "$(ls -l "$1" 2>/dev/null)" in "-rw-------"*) ;; *) chmod 600 "$1" 2>/dev/null ;; esac; }
not_managed() {     # <reason>: left alone - as long as the pair is there
    [ -s "$CRT" ] && [ -s "$KEY" ] ||
        fail "$1, and server.crt / server.key is missing - nothing was changed"
    echo "reason: $1"
    echo not-managed
    exit 0
}

# --- managed or not -----------------------------------------------------------
if [ -L "$CRT" ] || [ -L "$KEY" ]; then not_managed "server.crt or server.key is a symbolic link"; fi
HAVE_CRT=0
if [ -s "$CRT" ]; then
    nb=$(blocks "$CRT" "$WORK/blk")
    [ "$nb" -ge 1 ] 2>/dev/null || not_managed "server.crt holds no certificate"
    info "$WORK/blk1.pem" 60
    [ $I_OK = 1 ] || not_managed "server.crt holds no certificate openssl can read"
    HAVE_CRT=1
    L_SUBJ=$I_SUBJ; L_ISSUER=$I_ISSUER; L_PUB=$I_PUB; L_VALID=$I_VALID
    L_SAN=$(printf '%s\n' "$I_TEXT" | san_of)
    managed=0
    if [ "$L_SUBJ" = "CN=krakensdr" ]; then
        managed=1       # install.sh / Docker / Windows before PR #5
    else
        case ",$L_SUBJ," in *",O=KrakenSDR receiver,"*) managed=1 ;; esac    # PR #5's, and ours
    fi
    if [ $managed = 0 ] && [ -s "$CA_CRT" ] && verifies "$WORK/blk1.pem" "$CA_CRT" notime; then managed=1; fi
    [ $managed = 1 ] || not_managed "server.crt is not a KrakenSDR certificate ($L_SUBJ)"
fi

# --- the server key: kept whatever its type -----------------------------------
KEY_NEW=0; K_PUB=""
if [ -s "$KEY" ]; then
    K_PUB=$(key_pub "$KEY")
    case "$K_PUB" in
    *"BEGIN PUBLIC KEY"*) ;;
    *)  # an encrypted key with no certificate is not ours to replace
        [ $HAVE_CRT = 1 ] || fail "server.key can't be read (a passphrase?) and there is no server.crt - nothing was changed"
        KEY_NEW=1; K_PUB=""
        note "server.key is not a readable private key - a new one" ;;
    esac
else
    KEY_NEW=1
fi

# --- the CA -------------------------------------------------------------------
CA_OK=0; C_TEXT=""
if [ -s "$CA_CRT" ] && [ -s "$CA_KEY" ]; then
    info "$CA_CRT" 365
    C_TEXT=$I_TEXT; C_VALID=$I_VALID
    if [ $I_OK = 1 ] && [ "$I_PUB" = "$(key_pub "$CA_KEY")" ]; then
        case "$C_TEXT" in *"CA:TRUE"*) CA_OK=1 ;; esac
    fi
fi
NEED_CA=0; REASON=""
if [ $new_ca = 1 ]; then
    NEED_CA=1; REASON="--new-ca"
elif [ $CA_OK = 0 ]; then
    NEED_CA=1
    if [ -e "$CA_CRT" ] || [ -e "$CA_KEY" ]; then REASON="the CA key is missing or does not match"
    elif [ $HAVE_CRT = 1 ]; then REASON="replaces the self-signed certificate"
    else REASON="first certificate"; fi
elif [ $C_VALID = 0 ]; then
    NEED_CA=1; REASON="the old one expires within a year"
else
    perm=$(printf '%s\n' "$C_TEXT" | permitted_of)
    nc=$(printf '%s\n' "$perm" | sed -n 's/^DNS://p' | tr '[:upper:]' '[:lower:]' | tr '\n' ' ')
    # wide = the CA permits every name under "local" (as tlscert's ca_is_wide);
    # a CA without name constraints counts as narrow and permits every name
    case " $nc " in *" local "*) ca_scope=wide ;; *) ca_scope=narrow ;; esac
    if [ "$ca_scope" != "$SCOPE" ]; then
        NEED_CA=1; REASON="scope $ca_scope -> $SCOPE"
    else
        for n in $NAMES; do
            if ! dns_permitted "$n" "$nc"; then NEED_CA=1; REASON="new name $n"; break; fi
        done
    fi
fi

# --- the server certificate ---------------------------------------------------
ISSUE=$NEED_CA
if [ $ISSUE = 0 ]; then
    ISSUE=1
    if [ $force = 1 ]; then REASON="--force"
    elif [ $HAVE_CRT = 0 ]; then REASON="no server certificate"
    elif [ $KEY_NEW = 1 ]; then REASON="a new server key"
    elif [ "$L_PUB" != "$K_PUB" ]; then REASON="server.crt does not match server.key"
    elif [ "$L_SAN" != "$WANT_SAN" ]; then
        if [ "$(printf '%s\n' "$L_SAN" | grep '^DNS:')" = "$(printf '%s\n' "$WANT_SAN" | grep '^DNS:')" ]; then
            REASON="the addresses changed"
        else
            REASON="the names changed"
        fi
    elif [ $L_VALID = 0 ]; then REASON="it expires within 60 days"
    elif [ "$nb" != 2 ] || [ "$(blocks "$CA_CRT" "$WORK/cablk")" -lt 1 ] ||
         [ "$(b64 "$WORK/blk2.pem")" != "$(b64 "$WORK/cablk1.pem")" ]; then
        REASON="server.crt does not hold the CA certificate after it"
    elif ! verifies "$WORK/blk1.pem" "$CA_CRT"; then REASON="it does not verify against the CA"
    else ISSUE=0
    fi
fi
if [ $ISSUE = 0 ]; then
    fix_mode "$KEY"
    fix_mode "$CA_KEY"
    echo kept
    exit 0
fi

# --- make everything in a folder of <dir>, then move it into place ------------
STAGE=$(mktemp -d "$D/.make-cert.XXXXXX" 2>/dev/null) ||
    fail "cannot write in ${D:-/} (read-only?) - the certificate stays as it was"
NOW=$(date -u +%s)
case "$NOW" in ""|*[!0-9]*) fail "date +%s gave no time - nothing was changed" ;; esac
START=$((NOW - 3600))       # an hour back, as kraken_doa's generator
# asn1_time <epoch seconds>: YYMMDDHHMMSSZ (UTCTime), YYYYMMDDHHMMSSZ from 2050
asn1_time() {
    awk -v t="$1" 'BEGIN {
        s = t % 86400; z = (t - s) / 86400 + 719468
        era = int(z / 146097); doe = z - era * 146097
        yoe = int((doe - int(doe / 1460) + int(doe / 36524) - int(doe / 146096)) / 365)
        y = yoe + era * 400; doy = doe - (365 * yoe + int(yoe / 4) - int(yoe / 100))
        mp = int((5 * doy + 2) / 153); d = doy - int((153 * mp + 2) / 5) + 1
        m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) y++
        h = int(s / 3600); mi = int((s % 3600) / 60); se = s % 60
        if (y < 2050) printf "%02d%02d%02d%02d%02d%02dZ\n", y % 100, m, d, h, mi, se
        else printf "%04d%02d%02d%02d%02d%02dZ\n", y, m, d, h, mi, se }'
}
T_START=$(asn1_time $START)
T_END_CA=$(asn1_time $((START + 3650 * 86400)))
T_END_SRV=$(asn1_time $((START + 825 * 86400)))
# a new random serial (128 bits) for the next `openssl ca`
serial() {
    local s
    s=$(ossl rand -hex 16 2>/dev/null) || fail "openssl could not make a serial number - nothing was changed"
    printf '%s\n' "$s" > "$STAGE/serial"
}

: > "$STAGE/index.txt"
{
    echo "[ca]"
    echo "default_ca = kca"
    echo "[kca]"
    echo "database = index.txt"
    echo "new_certs_dir = ."
    echo "serial = serial"
    echo "default_md = sha256"
    echo "policy = kpolicy"
    echo "unique_subject = no"
    echo "email_in_dn = no"
    echo "preserve = yes"
    echo "copy_extensions = none"
    echo "[kpolicy]"
    echo "commonName = supplied"
    echo "organizationName = optional"
    echo "[v3_ca]"
    echo "basicConstraints = critical, CA:TRUE, pathlen:0"
    echo "keyUsage = critical, keyCertSign, cRLSign"
    echo "subjectKeyIdentifier = hash"
    echo "nameConstraints = critical, @nc"
    echo "[nc]"
    i=0
    for n in $NC_DNS; do echo "permitted;DNS.$i = $n"; i=$((i + 1)); done
    if [ $SCOPE = narrow ]; then
        echo "permitted;IP.0 = 127.0.0.1/255.255.255.255"
        echo "permitted;IP.1 = ::1/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"
    else
        echo "permitted;IP.0 = 10.0.0.0/255.0.0.0"
        echo "permitted;IP.1 = 172.16.0.0/255.240.0.0"
        echo "permitted;IP.2 = 192.168.0.0/255.255.0.0"
        echo "permitted;IP.3 = 100.64.0.0/255.192.0.0"
        echo "permitted;IP.4 = 127.0.0.0/255.0.0.0"
        echo "permitted;IP.5 = fc00::/fe00::"
        echo "permitted;IP.6 = ::1/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"
    fi
    echo "[v3_server]"
    echo "basicConstraints = critical, CA:FALSE"
    echo "keyUsage = critical, digitalSignature, keyEncipherment"
    echo "extendedKeyUsage = serverAuth"
    echo "subjectKeyIdentifier = hash"
    echo "authorityKeyIdentifier = keyid:always"
    echo "subjectAltName = @san"
    echo "[san]"
    i=0
    for n in $NAMES; do echo "DNS.$i = $n"; i=$((i + 1)); done
    i=0
    for a in $IPS; do echo "IP.$i = $a"; i=$((i + 1)); done
} > "$STAGE/ca.cnf"
# req_cnf <CN>: a request config (no extensions: `ca` adds them)
req_cnf() {
    echo "[req]"
    echo "distinguished_name = dn"
    echo "prompt = no"
    echo "utf8 = yes"
    echo "string_mask = utf8only"
    echo "[dn]"
    echo "CN = $1"
    echo "O = KrakenSDR receiver"
}

if [ $NEED_CA = 1 ]; then
    must "make the CA key" genrsa -out ca.key 3072
    req_cnf "KrakenSDR $CN CA $(date +%Y-%m-%d)" > "$STAGE/ca_req.cnf"
    must "make the CA request" req -new -config ca_req.cnf -key ca.key -out ca.csr
    serial
    must "make the CA certificate" ca -batch -notext -config ca.cnf -selfsign -keyfile ca.key -in ca.csr \
        -extensions v3_ca -startdate "$T_START" -enddate "$T_END_CA" -out ca.crt
    USE_CA_KEY=$STAGE/ca.key; USE_CA_CRT=$STAGE/ca.crt
else
    USE_CA_KEY=$CA_KEY; USE_CA_CRT=$CA_CRT
fi
if [ $KEY_NEW = 1 ]; then
    must "make the server key" genrsa -out server.key 2048
    USE_KEY=$STAGE/server.key
else
    USE_KEY=$KEY
fi
req_cnf "$CN" > "$STAGE/server_req.cnf"
must "make the certificate request" req -new -config server_req.cnf -key "$USE_KEY" -out server.csr
serial
must "sign the server certificate" ca -batch -notext -config ca.cnf -cert "$USE_CA_CRT" -keyfile "$USE_CA_KEY" \
    -in server.csr -extensions v3_server -startdate "$T_START" -enddate "$T_END_SRV" -out leaf.crt
must "read the CA certificate" x509 -in "$USE_CA_CRT" -out ca.pem
verifies "$STAGE/leaf.crt" "$STAGE/ca.pem" ||
    fail "the new server certificate does not verify against the CA - nothing was changed"
info "$STAGE/leaf.crt" 0
[ "$I_PUB" = "$(key_pub "$USE_KEY")" ] ||
    fail "the new server certificate does not match the key - nothing was changed"
cat "$STAGE/leaf.crt" "$STAGE/ca.pem" > "$STAGE/server.crt" ||
    fail "cannot write in ${D:-/} - nothing was changed"

# Into place. Each step is a rename in one folder; the order keeps any
# interruption repairable by the next run (a CA key without its certificate,
# a key without its certificate: both are replaced / re-issued then)
mv_into() { chmod "$3" "$1" && mv -f "$1" "$2" || fail "cannot replace $2"; }
RETIRED=""
if [ $NEED_CA = 1 ]; then
    if [ -s "$CA_CRT" ] && [ ! -L "$CA_CRT" ] && ossl x509 -in "$CA_CRT" -noout >/dev/null 2>&1; then
        day=$(date +%Y%m%d); RETIRED="krakensdr-ca.$day.retired.crt"; i=2
        while [ -e "$D/$RETIRED" ]; do RETIRED="krakensdr-ca.$day-$i.retired.crt"; i=$((i + 1)); done
        mv -f "$CA_CRT" "$D/$RETIRED" || fail "cannot rename $CA_CRT - nothing was changed"
        chmod 644 "$D/$RETIRED" 2>/dev/null
    fi
    mv_into "$STAGE/ca.key" "$CA_KEY" 600       # the old CA's key is gone with this
    mv_into "$STAGE/ca.pem" "$CA_CRT" 644
fi
[ $KEY_NEW = 0 ] || mv_into "$STAGE/server.key" "$KEY" 600
mv_into "$STAGE/server.crt" "$CRT" 644
fix_mode "$KEY"

echo "reason: $REASON"
[ -z "$RETIRED" ] || echo "retired: $RETIRED"
if [ $NEED_CA = 1 ]; then echo ca-created; else echo issued; fi
exit 0
