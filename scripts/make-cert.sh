#!/usr/bin/env bash
#
# scripts/make-cert.sh - the HTTPS certificate of kraken_doa's web UI (8080)
#
#   scripts/make-cert.sh <dir> [--force] [--new-ca]
#
# Implements kraken_doa_v2/docs/dev/https-certificate.md, the contract this
# script shares with kraken_doa's in-process generator
# (kraken_doa_v2/src/utils/tls_compat.cpp) and the page: change them
# together. In short, each receiver has its own small certificate authority
# (krakensdr-ca.key / krakensdr-ca.crt), which users install once per
# device. It signs the server certificate (server.crt = the server
# certificate followed by the CA certificate), which is re-issued here
# whenever the names or addresses change or it gets old - always for the
# same server.key - without anyone having to install anything again. The CA
# itself is replaced only when it can't vouch for a wanted name (a rename,
# an avahi conflict name, a new search domain), when the scope in
# cert_options differs from its own, when it expires within a year, or with
# --new-ca; the old CA certificate is kept as krakensdr-ca.YYYYMMDD.retired.crt
# (its key deleted) so the page can name what devices should remove.
# A certificate of the user's own (or a symlinked server.crt / server.key)
# is never touched.
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
# and moved into place at the end; keys 0600, certificates 0644).
#
# Portable: bash 3.2 (macOS: no arrays, no [[, no ${x,,}, no mapfile - it
# runs under dash as well), OpenSSL 1.1.1 / 3.x / LibreSSL - only its own
# config files (-config, -extfile), never -addext on top of the system's
# openssl.cnf (1.1.1 then added a second Basic Constraints extension), and
# names / constraints read back with x509 -text (1.1.1 and LibreSSL have no
# x509 -ext).

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
case "$dir" in /) D="" ;; *) D=${dir%/} ;; esac
CA_KEY="$D/krakensdr-ca.key"; CA_CRT="$D/krakensdr-ca.crt"
KEY="$D/server.key"; CRT="$D/server.crt"; OPTS="$D/cert_options"
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
{ [ -n "$OSSL" ] && "$OSSL" version >/dev/null 2>&1 </dev/null; } ||
    fail "no openssl command (it makes the web UI's certificate) - nothing was changed"
ossl() { "$OSSL" "$@" </dev/null; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/make-cert.XXXXXX" 2>/dev/null) || fail "cannot make a temporary folder"
STAGE=""
cleanup() { rm -rf "$WORK"; if [ -n "$STAGE" ]; then rm -rf "$STAGE"; fi; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# must <what> <openssl arguments...>: run openssl, or fail with its last error
must() {
    local what=$1; shift
    "$OSSL" "$@" </dev/null >"$WORK/out" 2>"$WORK/err" && return 0
    fail "openssl could not $what: $(grep -v '^[[:space:]]*$' "$WORK/err" | tail -n 1) - nothing was changed"
}

# --- what is wanted: scope, host name, names, addresses ----------------------
lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }
run_t() {       # run_t <seconds> <command...>: with a time limit where timeout(1) exists
    local t=$1; shift
    if command -v timeout >/dev/null 2>&1; then timeout "$t" "$@"; else "$@"; fi
}

SCOPE=narrow
if [ -f "$OPTS" ]; then
    v=$(sed -n 's/^[[:space:]]*scope[[:space:]]*=[[:space:]]*\([A-Za-z]*\)[[:space:]]*$/\1/p' "$OPTS" 2>/dev/null | tail -n 1)
    v=$(lower "$v")
    case "$v" in
    narrow|wide) SCOPE=$v ;;
    *) note "cert_options has no scope=narrow / scope=wide line - narrow scope" ;;
    esac
fi

# This machine's IPv4 addresses, one per line (loopback may be among them)
local_ipv4s() {
    local out=""
    if command -v ip >/dev/null 2>&1; then
        out=$(ip -4 -o addr show scope global 2>/dev/null </dev/null |
              awk '{ for (i = 1; i < NF; i++) if ($i == "inet") { a = $(i + 1); sub(/\/.*/, "", a); print a; break } }')
    fi
    if [ -z "$out" ] && command -v ifconfig >/dev/null 2>&1; then
        out=$(ifconfig 2>/dev/null </dev/null | awk '$1 == "inet" { a = $2; sub(/^addr:/, "", a); print a }')
    fi
    if [ -z "$out" ] && [ "$OS" != Darwin ]; then
        out=$(hostname -I 2>/dev/null </dev/null | tr ' ' '\n')
    fi
    printf '%s\n' "$out"
}
# stdin: IPv4 addresses -> the ones inside the wide scope's private ranges
# (10/8, 172.16/12, 192.168/16, 100.64/10; not 127/8 - 127.0.0.1 is named
# anyway), each once. A public address is left out: a name outside the CA's
# constraints would fail the whole certificate
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
            case "$s" in "i 1"|"i 3") sleep 0.5 ;; *) break ;; esac
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
        a=$(local_ipv4s | private_v4 | head -n 1)
        if [ -n "$a" ]; then
            h=$(run_t 3 avahi-resolve -a "$a" 2>/dev/null </dev/null | awk 'NF >= 2 { print $2; exit }')
            h=${h%.}
            case "$h" in *.local) h=${h%.local} ;; *) h="" ;; esac   # an mDNS answer only
        fi
    fi
    printf '%s' "$h"
}

if [ -n "${KRAKEN_CERT_HOST:-}" ]; then
    RAW=$KRAKEN_CERT_HOST
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
    RAW=${RAW%%.*}
fi
HOST=$(lower "$RAW")
# a character outside a-z 0-9 - makes it unusable for DNS: then only
# localhost (and in wide scope the addresses) is named
case "$HOST" in ""|*[!a-z0-9-]*) USABLE=0 ;; *) USABLE=1 ;; esac
if [ $USABLE = 1 ]; then CN=$HOST; else CN=localhost; fi
[ $USABLE = 1 ] || note "the host name '$RAW' is not usable in DNS - the certificate names only localhost"

valid_dns() { case "$1" in ""|*[!a-z0-9.-]*|.*|*.|*..*) return 1 ;; esac; return 0; }
NAMES=" "
add_name() { case "$NAMES" in *" $1 "*) ;; *) NAMES="$NAMES$1 " ;; esac; }
if [ $USABLE = 1 ]; then
    add_name "$HOST"
    add_name "$HOST.local"
    if [ -r "$RESOLV" ]; then
        for d in $(awk '$1 == "search" || $1 == "domain" { for (i = 2; i <= NF; i++) print $i }' "$RESOLV" 2>/dev/null); do
            d=$(lower "$d"); d=${d%.}
            if [ "$d" != local ] && valid_dns "$d"; then add_name "$HOST.$d"; fi
        done
    fi
fi
add_name localhost
for n in $(printf '%s' "${KRAKEN_CERT_NAMES:-}" | tr ',' ' '); do
    n=$(lower "$n"); n=${n%.}
    if ! valid_dns "$n"; then
        note "KRAKEN_CERT_NAMES: '$n' is not a DNS name - left out"
    else
        case "$n" in
        *[!0-9.]*) add_name "$n" ;;
        *) note "KRAKEN_CERT_NAMES: $n is an address, not a name - left out" ;;
        esac
    fi
done
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
subject_of() { ossl x509 -in "$1" -noout -subject -nameopt RFC2253 2>/dev/null | sed 's/^subject= *//'; }
issuer_of()  { ossl x509 -in "$1" -noout -issuer -nameopt RFC2253 2>/dev/null | sed 's/^issuer= *//'; }
cert_pub()   { ossl x509 -in "$1" -noout -pubkey 2>/dev/null; }
key_pub()    { ossl pkey -in "$1" -passin pass: -pubout 2>/dev/null; }
fingerprint() { ossl x509 -in "$1" -noout -fingerprint -sha256 2>/dev/null; }
# valid_for <cert> <days>: still valid in <days> days. By its message, not
# its exit status: OpenSSL 3.0.2 (Ubuntu 22.04) exits 0 for "will expire" too
valid_for() {
    case "$(ossl x509 -in "$1" -noout -checkend $(($2 * 86400)) 2>/dev/null)" in
    *"will not expire"*) return 0 ;;
    *) return 1 ;;
    esac
}
# subjectAltName as sorted "DNS:x" / "IP:a" lines (::1 is printed in full)
cert_san() {
    ossl x509 -in "$1" -noout -text 2>/dev/null | awk '
        /X509v3 Subject Alternative Name/ { getline; n = split($0, p, ",")
            for (i = 1; i <= n; i++) { s = p[i]; gsub(/^[ \t]+|[ \t]+$/, "", s); sub(/^IP Address:/, "IP:", s); print s }
            exit }' | sed 's/^IP:0:0:0:0:0:0:0:1$/IP:::1/' | LC_ALL=C sort -u
}
# the permitted subtrees of a CA's name constraints, "DNS:x" / "IP:a/mask"
# per line; "none" when it has no Name Constraints extension
ca_permitted() {
    ossl x509 -in "$1" -noout -text 2>/dev/null | awk '
        /X509v3 Name Constraints/ { inside = 1; found = 1; next }
        !inside { next }
        /^[ \t]*Permitted:/ { perm = 1; next }
        /^[ \t]*Excluded:/ { perm = 0; next }
        /^[ \t]*(DNS|IP|email|URI|DirName|othername|Registered ID):/ { if (perm) { s = $0; gsub(/^[ \t]+|[ \t]+$/, "", s); print s }; next }
        { exit }
        END { if (!found) print "none" }'
}
# dns_permitted <name> <DNS constraints, space-separated>: RFC 5280 - the
# name itself or anything under it (".x": only under it). No DNS
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

VERIFY_OPTS=""
vhelp=$(ossl verify -help 2>&1)
# time is checked separately (-checkend): a Pi without a clock that booted
# before its last NTP sync must not make a good certificate look foreign
case "$vhelp" in *-no_check_time*) VERIFY_OPTS="$VERIFY_OPTS -no_check_time" ;; esac
# only our CA: not the system's trust store as well
case "$vhelp" in *-no-CApath*) VERIFY_OPTS="$VERIFY_OPTS -no-CApath" ;; esac
case "$vhelp" in *-no-CAstore*) VERIFY_OPTS="$VERIFY_OPTS -no-CAstore" ;; esac
# verifies <cert> <ca>: issued by that CA, a server certificate inside its constraints
verifies() {
    [ "$(issuer_of "$1")" = "$(subject_of "$2")" ] || return 1
    ossl verify $VERIFY_OPTS -CAfile "$2" -purpose sslserver "$1" >/dev/null 2>&1
}
# fix_mode <file> <mode string> <mode>: tighten a key left readable by others
fix_mode() { case "$(ls -l "$1" 2>/dev/null)" in "$2"*) ;; *) chmod "$3" "$1" 2>/dev/null ;; esac; }

# --- managed or not -----------------------------------------------------------
if [ -L "$CRT" ] || [ -L "$KEY" ]; then
    echo "reason: server.crt or server.key is a symbolic link"
    echo not-managed
    exit 0
fi
HAVE_CRT=0
if [ -s "$CRT" ]; then
    ossl x509 -in "$CRT" -out "$WORK/leaf.pem" >/dev/null 2>&1 ||
        fail "$CRT holds no certificate openssl can read - move it away (a new one is made then) or replace it"
    HAVE_CRT=1
    subj=$(subject_of "$WORK/leaf.pem")
    managed=0
    if [ "$subj" = "CN=krakensdr" ]; then
        managed=1       # install.sh / Docker / Windows before PR #5
    else
        case ",$subj," in *",O=KrakenSDR receiver,"*) managed=1 ;; esac    # PR #5's, and ours
    fi
    if [ $managed = 0 ] && [ -s "$CA_CRT" ] && verifies "$WORK/leaf.pem" "$CA_CRT"; then managed=1; fi
    if [ $managed = 0 ]; then
        echo "reason: server.crt is not a KrakenSDR certificate ($subj)"
        echo not-managed
        exit 0
    fi
fi

# --- the server key: kept whatever its type -----------------------------------
KEY_NEW=0
if [ -s "$KEY" ]; then
    if ! key_pub "$KEY" | grep -q 'BEGIN PUBLIC KEY'; then
        KEY_NEW=1
        note "server.key is not a readable private key - a new one"
    fi
else
    KEY_NEW=1
fi

# --- the CA -------------------------------------------------------------------
CA_OK=0
if [ -s "$CA_CRT" ] && [ -s "$CA_KEY" ]; then
    p=$(cert_pub "$CA_CRT")
    if [ -n "$p" ] && [ "$p" = "$(key_pub "$CA_KEY")" ] &&
       ossl x509 -in "$CA_CRT" -noout -text 2>/dev/null | grep -q 'CA:TRUE'; then
        CA_OK=1
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
elif ! valid_for "$CA_CRT" 365; then
    NEED_CA=1; REASON="the old one expires within a year"
else
    perm=$(ca_permitted "$CA_CRT")
    if [ "$perm" = none ]; then
        ca_scope=none
    else
        case "
$perm
" in *"
IP:10.0.0.0/255.0.0.0
"*) ca_scope=wide ;; *) ca_scope=narrow ;; esac
    fi
    if [ "$ca_scope" != "$SCOPE" ]; then
        NEED_CA=1; REASON="scope $ca_scope -> $SCOPE"
    else
        nc=$(printf '%s\n' "$perm" | sed -n 's/^DNS://p' | tr '[:upper:]' '[:lower:]' | tr '\n' ' ')
        for n in $NAMES; do
            if ! dns_permitted "$n" "$nc"; then NEED_CA=1; REASON="new name $n"; break; fi
        done
    fi
fi

# --- the server certificate ---------------------------------------------------
chain_ok() {    # server.crt = [this certificate, the CA certificate]
    awk -v d="$WORK" '/-----BEGIN CERTIFICATE-----/ { n++; f = d "/blk" n ".pem" }
        f != "" { print > f }
        /-----END CERTIFICATE-----/ { if (f != "") close(f); f = "" }
        END { print n + 0 > (d "/blkn") }' "$CRT"
    [ "$(cat "$WORK/blkn" 2>/dev/null)" = 2 ] || return 1
    [ "$(fingerprint "$WORK/blk2.pem")" = "$(fingerprint "$CA_CRT")" ]
}
ISSUE=$NEED_CA
if [ $ISSUE = 0 ]; then
    ISSUE=1
    if [ $force = 1 ]; then REASON="--force"
    elif [ $HAVE_CRT = 0 ]; then REASON="no server certificate"
    elif [ $KEY_NEW = 1 ]; then REASON="a new server key"
    elif [ "$(cert_pub "$WORK/leaf.pem")" != "$(key_pub "$KEY")" ]; then REASON="server.crt does not match server.key"
    elif ! verifies "$WORK/leaf.pem" "$CA_CRT"; then REASON="not issued by the current CA"
    else
        have_san=$(cert_san "$WORK/leaf.pem")
        if [ "$have_san" != "$WANT_SAN" ]; then
            if [ "$(printf '%s\n' "$have_san" | grep '^DNS:')" = "$(printf '%s\n' "$WANT_SAN" | grep '^DNS:')" ]; then
                REASON="the addresses changed"
            else
                REASON="the names changed"
            fi
        elif ! valid_for "$WORK/leaf.pem" 60; then REASON="it expires within 60 days"
        elif ! chain_ok; then REASON="server.crt does not hold the CA certificate after it"
        else ISSUE=0
        fi
    fi
fi
if [ $ISSUE = 0 ]; then
    fix_mode "$KEY" "-rw-------" 600
    fix_mode "$CA_KEY" "-rw-------" 600
    echo kept
    exit 0
fi

# --- make everything in a folder of <dir>, then move it into place ------------
STAGE=$(mktemp -d "$D/.make-cert.XXXXXX" 2>/dev/null) ||
    fail "cannot write in ${D:-/} (read-only?) - the certificate stays as it was"
TODAY=$(date -u +%Y-%m-%d)
serial() { ossl rand -hex 16 2>/dev/null || fail "openssl could not make a serial number - nothing was changed"; }

if [ $NEED_CA = 1 ]; then
    must "make the CA key" genrsa -out "$STAGE/ca.key" 3072
    {
        echo "[req]"
        echo "distinguished_name = dn"
        echo "x509_extensions = v3_ca"
        echo "prompt = no"
        echo "utf8 = yes"
        echo "string_mask = utf8only"
        echo "[dn]"
        echo "CN = KrakenSDR $CN CA $TODAY"
        echo "O = KrakenSDR receiver"
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
    } > "$STAGE/ca.cnf"
    must "make the CA certificate" req -new -x509 -config "$STAGE/ca.cnf" -key "$STAGE/ca.key" \
        -days 3650 -sha256 -set_serial "0x$(serial)" -out "$STAGE/ca.crt"
    USE_CA_KEY=$STAGE/ca.key; USE_CA_CRT=$STAGE/ca.crt
else
    USE_CA_KEY=$CA_KEY; USE_CA_CRT=$CA_CRT
fi
if [ $KEY_NEW = 1 ]; then
    must "make the server key" genrsa -out "$STAGE/server.key" 2048
    USE_KEY=$STAGE/server.key
else
    USE_KEY=$KEY
fi

{
    echo "[req]"
    echo "distinguished_name = dn"
    echo "prompt = no"
    echo "utf8 = yes"
    echo "string_mask = utf8only"
    echo "[dn]"
    echo "CN = $CN"
    echo "O = KrakenSDR receiver"
} > "$STAGE/server.cnf"
{
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
} > "$STAGE/server.ext"
must "make the certificate request" req -new -config "$STAGE/server.cnf" -key "$USE_KEY" -out "$STAGE/server.csr"
must "sign the server certificate" x509 -req -in "$STAGE/server.csr" -CA "$USE_CA_CRT" -CAkey "$USE_CA_KEY" \
    -set_serial "0x$(serial)" -days 825 -sha256 -extfile "$STAGE/server.ext" -extensions v3_server -out "$STAGE/leaf.crt"
must "read the CA certificate" x509 -in "$USE_CA_CRT" -out "$STAGE/ca.pem"
verifies "$STAGE/leaf.crt" "$STAGE/ca.pem" ||
    fail "the new server certificate does not verify against the CA - nothing was changed"
[ "$(cert_pub "$STAGE/leaf.crt")" = "$(key_pub "$USE_KEY")" ] ||
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
        day=$(date -u +%Y%m%d); RETIRED="krakensdr-ca.$day.retired.crt"; i=2
        while [ -e "$D/$RETIRED" ]; do RETIRED="krakensdr-ca.$day-$i.retired.crt"; i=$((i + 1)); done
        mv -f "$CA_CRT" "$D/$RETIRED" || fail "cannot rename $CA_CRT - nothing was changed"
        chmod 644 "$D/$RETIRED" 2>/dev/null
    fi
    mv_into "$STAGE/ca.key" "$CA_KEY" 600       # the old CA's key is gone with this
    mv_into "$STAGE/ca.crt" "$CA_CRT" 644
fi
[ $KEY_NEW = 0 ] || mv_into "$STAGE/server.key" "$KEY" 600
mv_into "$STAGE/server.crt" "$CRT" 644
fix_mode "$KEY" "-rw-------" 600

echo "reason: $REASON"
[ -z "$RETIRED" ] || echo "retired: $RETIRED"
if [ $NEED_CA = 1 ]; then echo ca-created; else echo issued; fi
exit 0
