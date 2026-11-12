#!/usr/bin/env bash
# TLS certificate of the DoA client web UI (kraken_doa_v2/server.crt + .key).
#
#   scripts/make-cert.sh <dir> [--force]
#
# Writes a self-signed SERVER certificate (not a CA: it cannot sign anything,
# so trusting it on a PC or phone only ever vouches for this one receiver).
# It names the host the way people type it - <hostname>, <hostname>.local,
# <hostname>.<each DNS search domain>, localhost - plus the IPv4 addresses
# the machine has right now, so a browser that has been told to trust the
# certificate accepts those names without a warning.
#
# An existing certificate is kept as long as it matches its key, carries the
# current host name and has more than 30 days left: whoever installed it must
# not have to do that again after every start. When it is replaced, the key
# is reused. 825 days is the longest validity Apple devices accept.
#
# Exit status 0: certificate present (kept or written). The last line of
# output says which: "kept", "created" or "renewed".
set -u
dir="${1:?usage: make-cert.sh <dir> [--force]}"; force="${2:-}"
crt="$dir/server.crt"; key="$dir/server.key"
command -v openssl >/dev/null 2>&1 || { echo "openssl not found" >&2; exit 1; }

host="$(hostname -s 2>/dev/null || hostname 2>/dev/null || echo krakensdr)"
host="$(printf '%s' "$host" | tr 'A-Z' 'a-z' | tr -cd 'a-z0-9.-')"
[[ -n "$host" ]] || host=krakensdr

names=("$host" "$host.local" localhost)
if [[ -r /etc/resolv.conf ]]; then
    for d in $(awk '$1=="search"||$1=="domain"{for(i=2;i<=NF;i++)print $i}' /etc/resolv.conf 2>/dev/null); do
        d="$(printf '%s' "$d" | tr 'A-Z' 'a-z' | tr -cd 'a-z0-9.-')"; d="${d%.}"
        [[ -n "$d" && "$d" != local ]] && names+=("$host.$d")
    done
fi
ips=(127.0.0.1)
if command -v ip >/dev/null 2>&1; then
    for a in $(ip -4 -o addr show scope global 2>/dev/null | awk '{sub(/\/.*/,"",$4);print $4}'); do ips+=("$a"); done
elif command -v ifconfig >/dev/null 2>&1; then
    for a in $(ifconfig 2>/dev/null | awk '$1=="inet"&&$2!="127.0.0.1"{print $2}'); do ips+=("$a"); done
fi

san=""; seen=" "
for n in "${names[@]}"; do [[ "$seen" == *" $n "* ]] && continue; seen+="$n "; san+="${san:+,}DNS:$n"; done
for a in "${ips[@]}";   do [[ "$seen" == *" $a "* ]] && continue; seen+="$a "; san+="${san:+,}IP:$a"; done

key_ok=0; state=created
if [[ -s "$key" ]] && openssl pkey -in "$key" -noout 2>/dev/null; then key_ok=1; fi
if [[ "$force" != "--force" && $key_ok == 1 && -s "$crt" ]]; then
    if [[ "$(openssl x509 -noout -pubkey -in "$crt" 2>/dev/null)" == "$(openssl pkey -in "$key" -pubout 2>/dev/null)" ]] &&
       openssl x509 -noout -checkend $((30 * 86400)) -in "$crt" >/dev/null 2>&1 &&
       openssl x509 -noout -ext subjectAltName -in "$crt" 2>/dev/null | tr ',' '\n' | grep -qx "[[:space:]]*DNS:$host"; then
        echo kept; exit 0
    fi
    state=renewed
fi

tmp="$(mktemp "$dir/.server.crt.XXXXXX")" || exit 1
trap 'rm -f "$tmp"' EXIT
if [[ $key_ok == 1 ]]; then keyarg=(-key "$key"); else keyarg=(-newkey rsa:2048 -nodes -keyout "$key"); fi
openssl req -x509 -sha256 -days 825 "${keyarg[@]}" -out "$tmp" \
    -subj "/CN=$host/O=KrakenSDR receiver" \
    -addext "basicConstraints=critical,CA:FALSE" \
    -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
    -addext "extendedKeyUsage=serverAuth" \
    -addext "subjectAltName=$san" 2>/dev/null || { echo "openssl could not write the certificate" >&2; exit 1; }
chmod 600 "$key" 2>/dev/null
chmod 644 "$tmp" && mv -f "$tmp" "$crt" || exit 1
trap - EXIT
echo "$state"
