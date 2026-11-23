#!/bin/bash
#
# Container entrypoint for the KrakenSDR image (see DOCKER.md).
#
#   heimdall            coherent receiver server (needs the USB passthrough)
#   kraken_doa          DoA client; waits for heimdall's phase convergence
#   health-heimdall     healthcheck: the control port (8092) answers
#   health-kraken_doa   healthcheck: web UI port answers
#   <anything else>     run as a command (e.g. rtl_test, lsusb, bash)
#
# Both apps keep their state in their working directory, so each runs in its
# own directory under /data with the read-only web files symlinked in:
#   (VERSION, the image's version - docs/dev/version.md - is linked in too)
#   /data/heimdall    heimdall_settings.conf, s2p_calibration/
#   /data/kraken_doa  doa_settings.json, api_token, the HTTPS certificate
#                     (krakensdr-ca.crt/.key, server.crt/.key, cert_options),
#                     fft_wisdom.dat, doa_recordings/
#
# Environment:
#   VARIANT_FLAGS   hardware variant for heimdall: --wideband, --kerberos,
#                   --kerberos_sw, --ext_noise (kraken_doa gets --wideband
#                   from it; the rest it detects from the data stream)
#   KRAKEN_TUNERS   RTL-SDR dongles to wait for before heimdall starts
#                   (default 5; 0 = don't wait). A cold boot can start the
#                   container before every dongle has enumerated, and heimdall
#                   sizes the array from the dongles it finds
#   USB_WAIT        seconds to wait for them (default 45, then start anyway)
#   WAIT_TIMEOUT    seconds kraken_doa waits for convergence before starting
#                   anyway (default 300; 0 = forever)
#   KRAKEN_CERT_HOST   the host name the web UI's certificate names (default:
#                   the container's hostname = the host's; set it when avahi
#                   announces another one, e.g. krakensdr-2)
#   KRAKEN_CERT_NAMES  more names for it, comma-separated
#   /data/codecs    optional self-built digital voice codecs: libmbe.so.1
#                   (DMR / D-STAR), cdecoder + sdecoder (TETRA), libfdk-aac.so.2
#                   (DAB+ audio) - see README
set -euo pipefail

APP=/opt/krakensdr
DATA="${KRAKEN_DATA:-/data}"
DATA_PORT=8091
read -r -a VARIANT <<< "${VARIANT_FLAGS:-}"

# docker-compose.yml passes every optional setting, empty when not set in
# .env - and the apps treat an empty variable as set (HEIMDALL_VERBOSE_LOG=""
# would turn verbose logging on)
for v in $(compgen -e); do
    [[ $v == KRAKEN_* || $v == HEIMDALL_* ]] && [[ -z ${!v} ]] && unset "$v"
done

log() { echo "[krakensdr] $*" >&2; }

# One 8091 packet header (a fresh connection starts on a packet boundary):
# big-endian magic 'MCHQ' | channels | samples | phase_state. Prints the
# whole phase_state word (low byte = state, bit 8 = KerberosSDR manual mode).
phase_word() {
    local hex
    hex=$(timeout 5 bash -c "exec 3<>/dev/tcp/127.0.0.1/$DATA_PORT && head -c 16 <&3" 2>/dev/null |
          od -An -v -tx1 | tr -d ' \n') || return 1
    [[ ${#hex} -eq 32 && ${hex:0:8} == 4d434851 ]] || return 1
    echo $(( 16#${hex:24:8} ))
}

count_tuners() {
    local d n=0
    for d in /sys/bus/usb/devices/*; do
        [[ -r $d/idVendor ]] || continue
        [[ $(<"$d/idVendor") == 0bda && $(<"$d/idProduct") =~ ^283[28]$ ]] && n=$((n + 1))
    done
    echo "$n"
}

# Whether any attached RTL-SDR carries a KrakenSDR / KerberosSDR factory
# serial (1000-1007). Without one, heimdall runs the dongles it finds as
# generic RTL-SDRs (independent receivers), so there is no array to wait for.
kraken_serial_present() {
    local d
    for d in /sys/bus/usb/devices/*; do
        [[ -r $d/idVendor && -r $d/serial ]] || continue
        [[ $(<"$d/idVendor") == 0bda && $(<"$d/idProduct") =~ ^283[28]$ && $(<"$d/serial") =~ ^100[0-7]$ ]] &&
            return 0
    done
    return 1
}

link_files() {  # link_files <dir> <source dir> <file>...
    local dir="$1" src="$2"; shift 2
    local f
    for f in "$@"; do ln -sfn "$src/$f" "$dir/$f"; done
}

run_heimdall() {
    local want="${KRAKEN_TUNERS:-5}" limit="${USB_WAIT:-45}" n i generic_s=0
    if [[ ! -d /dev/bus/usb ]]; then
        log "/dev/bus/usb is not mounted - heimdall cannot see the dongles (see DOCKER.md)"
    fi
    if (( want > 0 )); then
        for (( i = 0; ; i++ )); do
            n=$(count_tuners)
            (( n >= want )) && break
            # Generic RTL-SDRs only (no factory serial for 10 s, no variant
            # flag or --serials): heimdall runs them as independent receivers
            if (( n > 0 )) && [[ ${#VARIANT[@]} -eq 0 && " $* " != *" --serials "* ]] && ! kraken_serial_present; then
                if (( ++generic_s >= 10 )); then
                    log "$n RTL-SDR dongle(s) without KrakenSDR serials - starting heimdall (generic RTL-SDR mode)"
                    break
                fi
            else
                generic_s=0
            fi
            if (( i >= limit )); then
                log "only $n of $want RTL-SDR dongles enumerated after ${limit}s; starting anyway"
                break
            fi
            (( i == 0 )) && log "waiting for $want RTL-SDR dongles ($n present)..."
            sleep 1
        done
    fi
    mkdir -p "$DATA/heimdall/s2p_calibration"
    link_files "$DATA/heimdall" "$APP/heimdall_v2" index.html VERSION
    cd "$DATA/heimdall"
    export HEIMDALL_NO_TUI=1
    log "starting heimdall ${VARIANT[*]} $*"
    exec "$APP/heimdall_v2/heimdall" "${VARIANT[@]}" "$@"
}

# The web UI's HTTPS certificate in the volume (scripts/make-cert.sh,
# kraken_doa_v2/docs/dev/https-certificate.md): this receiver's CA
# (krakensdr-ca.crt, what devices install once) and the server certificate it
# signs, re-issued at every start that finds the names / addresses changed or
# it old. The container can't ask the host's avahi for the name it announces:
# KRAKEN_CERT_HOST gives it (after an mDNS name conflict, e.g. krakensdr-2),
# KRAKEN_CERT_NAMES adds names. The messages are run.sh's (cert_check).
# Never fatal: kraken_doa then tries itself.
ensure_cert() {
    local out state reason rc=0
    out=$("$APP/scripts/make-cert.sh" "$DATA/kraken_doa" 2>&1) || rc=$?
    state=${out##*$'\n'}
    if (( rc != 0 )); then
        log "WARNING: web UI certificate not updated: $state"
        return 0
    fi
    reason=$(printf '%s\n' "$out" | sed -n 's/^reason: //p' | tail -n 1)
    # as run.sh's cert_check: names outside this receiver's CA, left out of
    # the certificate (fine rule 18); a key or certificate kept aside (12, 15)
    local nc moved
    nc=$(printf '%s\n' "$out" | sed -n 's/^not-covered: *//p' | tail -n 1)
    [[ -z $nc ]] || log "WARNING: web UI certificate: $nc not in it (outside this receiver's CA) - a new certificate authority takes them in (KrakenSDR DOA page: Telemetry sheet -> HTTPS certificate -> New certificate authority, or docker exec <container> $APP/scripts/make-cert.sh $DATA/kraken_doa --new-ca), then every device installs it"
    moved=$(printf '%s\n' "$out" | sed -n -e 's/^moved: //p' -e 's/^saved: //p' | tr '\n' ' ')
    [[ -z $moved ]] || log "WARNING: web UI certificate: kept aside in $DATA/kraken_doa: ${moved% }"
    case $state in
    issued)
        log "web UI certificate re-issued ($reason) - devices that installed this receiver's certificate need nothing new" ;;
    ca-created)
        if [[ $reason == "first certificate" ]]; then
            log "WARNING: web UI: install this receiver's certificate once on every device that opens the page (KrakenSDR DOA page: Telemetry sheet -> HTTPS certificate)"
        else
            log "WARNING: NEW web UI certificate for this receiver ($reason) - every device must install the new certificate and remove the old one (KrakenSDR DOA page: Telemetry sheet -> HTTPS certificate)"
        fi ;;
    not-managed)
        log "web UI certificate: server.crt is your own - left as it is" ;;
    esac
    # fine rule 26: the CA's SHA-256 in the container's log - only the owner sees it
    local fp
    fp=$(printf '%s\n' "$out" | sed -n 's/^ca-sha256: //p' | tail -n 1)
    [[ -z $fp ]] || log "web UI certificate authority SHA-256: $fp - compare it with what each device shows before trusting it"
}

# Like run.sh: start the client once heimdall's phases have converged, so the
# first bearings are valid. In KerberosSDR manual mode heimdall never
# converges on its own - start straight away. On a timeout start anyway (the
# client follows heimdall's state and reconnects if it restarts).
wait_converged() {
    local timeout="${WAIT_TIMEOUT:-300}" start=$SECONDS w last=""
    log "waiting for heimdall phase convergence (timeout ${timeout}s, 0 = none)"
    while :; do
        if w=$(phase_word); then
            if (( w >> 8 & 1 )); then
                log "KerberosSDR manual-calibration mode - not waiting (calibrate from heimdall's web UI)"
                return
            fi
            # bits 10 / 11: wideband scan / independent mode - no calibration runs
            if (( w >> 10 & 3 )); then
                log "heimdall is in wideband / independent mode - no calibration to wait for"
                return
            fi
            (( (w & 0xFF) == 4 )) && { log "heimdall converged"; return; }
            [[ $last != state ]] && log "heimdall is calibrating..." && last=state
        else
            [[ $last != port ]] && log "waiting for heimdall's data port $DATA_PORT..." && last=port
        fi
        if (( timeout > 0 && SECONDS - start >= timeout )); then
            log "no convergence within ${timeout}s - starting anyway"
            return
        fi
        sleep 1
    done
}

run_kraken_doa() {
    local args=() f
    for f in "${VARIANT[@]}"; do
        [[ $f == --wideband || $f == -w ]] && args+=(--wideband)
    done
    mkdir -p "$DATA/kraken_doa"
    link_files "$DATA/kraken_doa" "$APP/kraken_doa_v2" \
        kraken_doa.html array_calculator.html opus-decoder.min.js VERSION
    cd "$DATA/kraken_doa"
    # self-installed digital voice codecs (README "Digital voice codecs"):
    # libmbe.so.1, the ETSI TETRA cdecoder / sdecoder and libfdk-aac.so.2
    # (DAB+) in /data/codecs
    if [[ -z ${KRAKEN_MBELIB:-} && -e $DATA/codecs/libmbe.so.1 ]]; then
        export KRAKEN_MBELIB=$DATA/codecs/libmbe.so.1
    fi
    if [[ -z ${KRAKEN_AAC_LIB:-} && -e $DATA/codecs/libfdk-aac.so.2 ]]; then
        export KRAKEN_AAC_LIB=$DATA/codecs/libfdk-aac.so.2
    fi
    if [[ -z ${KRAKEN_TETRA_CODEC_DIR:-} && -x $DATA/codecs/cdecoder && -x $DATA/codecs/sdecoder ]]; then
        export KRAKEN_TETRA_CODEC_DIR=$DATA/codecs
    fi
    # decoder plugins built into the image (kraken_doa_v2/plugins/<id>/)
    export KRAKEN_PLUGIN_DIR=${KRAKEN_PLUGIN_DIR:-$APP/kraken_doa_v2/plugins}
    # 🗣 Voice to Text: the helper in the image, the speech models in the
    # volume (downloaded from the sidebar - they stay across image rebuilds)
    export KRAKEN_TRANSCRIBE_DIR=${KRAKEN_TRANSCRIBE_DIR:-$APP/kraken_doa_v2/transcribe}
    export KRAKEN_WHISPER_MODELS=${KRAKEN_WHISPER_MODELS:-$DATA/whisper_models}
    mkdir -p "$KRAKEN_WHISPER_MODELS"
    ensure_cert
    wait_converged
    export KRAKEN_DOA_NO_TUI=1
    log "starting kraken_doa ${args[*]} $*"
    exec "$APP/kraken_doa_v2/kraken_doa" "${args[@]}" "$@"
}

case "${1:-help}" in
    heimdall)           shift; run_heimdall "$@" ;;
    kraken_doa)         shift; run_kraken_doa "$@" ;;
    # not 8091: heimdall logs every data client that hangs up mid-packet
    # as an error, which would put a line in the log every interval
    health-heimdall)    timeout 5 bash -c 'exec 3<>/dev/tcp/127.0.0.1/8092 && head -n 1 <&3' 2>/dev/null |
                            grep -q '"num_channels"' ;;
    health-kraken_doa)  timeout 3 bash -c 'exec 3<>/dev/tcp/127.0.0.1/8080' 2>/dev/null ;;
    help|-h|--help)     sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//' ;;
    *)                  exec "$@" ;;
esac
