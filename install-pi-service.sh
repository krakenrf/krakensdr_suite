#!/bin/bash
#
# install-pi-service.sh - Run the KrakenSDR suite automatically at boot.
#
#   * systemd starts run.sh at boot (detached tmux session, no attach)
#   * desktop boot: a single desktop autostart opens ONE terminal viewing it
#   * console boot: tty1 autologin attaches instead
#
# Both viewers are installed and each checks the boot target at login, so the
# Pi can be switched between desktop and console boot later (raspi-config ->
# System Options -> Boot / Auto Login) without re-running this script.
#
# Safe to re-run; every step is idempotent.
#
# Usage:
#   ./install-pi-service.sh
#   VARIANT_FLAGS=--wideband ./install-pi-service.sh
#   VARIANT_FLAGS=--kerberos_sw KRAKEN_TUNERS=4 ./install-pi-service.sh
#   VARIANT_FLAGS=--ext_noise ./install-pi-service.sh   # add-on array noise source
#   APP_DIR=/opt/krakensdr_suite ./install-pi-service.sh
#   BOOT_MODE=console ./install-pi-service.sh   # auto|desktop|console|keep
#   POWER_BUTTONS=0 ./install-pi-service.sh     # no sudo rule for the web page's
#                                               # Shut down / Reboot computer
#   ./install-pi-service.sh --uninstall
#
#   ./install-pi-service.sh --power-only              # ONLY that sudo rule (any Linux
#   ./install-pi-service.sh --power-only --uninstall  # with systemd, no boot service);
#                                                     # removes only the rule
#
set -euo pipefail

SESSION="${TMUX_SESSION:-krakensdr}"
BOOT_MODE="${BOOT_MODE:-auto}"
VARIANT_FLAGS="${VARIANT_FLAGS:-}"
KRAKEN_TUNERS="${KRAKEN_TUNERS:-5}"
WAIT_TIMEOUT="${WAIT_TIMEOUT:-600}"

RUN_USER="${SUDO_USER:-$USER}"
RUN_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
APP_DIR="${APP_DIR:-$RUN_HOME/krakensdr_suite}"
RUN_SH="$APP_DIR/run.sh"
SVC=/etc/systemd/system/krakensdr.service
# The web page's ⏻ Shut down / Reboot computer (kraken_doa_v2/docs/dev/power.md)
POWER_SUDOERS=/etc/sudoers.d/krakensdr-power
POWER_BUTTONS="${POWER_BUTTONS:-1}"

# A value for a systemd unit line, double-quoted: an APP_DIR with a space
# split ExecStart into two words. '%' is a unit specifier and '$' an env
# reference inside Exec lines, so both are escaped too.
sd_quote() {
    local s="${1//\\/\\\\}"
    s="${s//\"/\\\"}"
    s="${s//%/%%}"
    s="${s//\$/\$\$}"
    printf '"%s"' "$s"
}

say()  { echo -e "\033[36;1m==>\033[0m \033[1m$*\033[0m"; }
ok()   { echo -e "\033[32m   ok  $*\033[0m"; }
warn() { echo -e "\033[33m   !   $*\033[0m"; }
die()  { echo -e "\033[31;1mError:\033[0m $*" >&2; exit 1; }
asuser() { sudo -u "$RUN_USER" "$@"; }

# Any ~/.bash_profile makes login bash skip ~/.profile (and with it the PATH
# setup and ~/.bashrc), so one we create always chains to ~/.profile, and we
# delete it again once our block is the only thing in it.
PROFILE_CHAIN='[ -f ~/.profile ] && . ~/.profile'
strip_console_attach() {
    local p="$RUN_HOME/.bash_profile" rest
    [[ -f "$p" ]] || return 0
    sed -i '/# --- KrakenSDR console attach/,/# --- end KrakenSDR/d' "$p"
    rest="$(grep -vxF "$PROFILE_CHAIN" "$p" || true)"
    [[ "$rest" =~ [^[:space:]] ]] || rm -f "$p"
}

# ------------------------------------------ web page: Shut down / Reboot -----
# kraken_doa's ⏻ Shut down and "Reboot computer" run `sudo -n systemctl
# poweroff|reboot` when logind doesn't allow this user without a password -
# which is the case for the service (no login session), and for a desktop PC
# whose user runs run.sh outside a local session. This rule allows exactly
# those two commands for $RUN_USER, nothing else (kraken_doa checks it with
# `sudo -n -l` at startup and offers only the buttons that work). ↻ Restart
# software needs no rule: run.sh's supervisors restart the apps.
power_rule_install() {
    local tmp_rule
    tmp_rule="$(mktemp)"
    cat >"$tmp_rule" <<EOF
# KrakenSDR web page: Shut down / Reboot computer (install-pi-service.sh;
# remove with: ./install-pi-service.sh --power-only --uninstall, or --uninstall)
$RUN_USER ALL=(root) NOPASSWD: /usr/bin/systemctl poweroff, /usr/bin/systemctl reboot, /bin/systemctl poweroff, /bin/systemctl reboot
EOF
    # a broken sudoers file locks sudo itself: check it before it goes in place
    if sudo visudo -cf "$tmp_rule" >/dev/null; then
        sudo install -m 0440 -o root -g root "$tmp_rule" "$POWER_SUDOERS"
        ok "web page Shut down / Reboot allowed for $RUN_USER ($POWER_SUDOERS)"
    else
        warn "sudo rule for the web page's Shut down / Reboot not valid - not installed"
    fi
    rm -f "$tmp_rule"
}
power_rule_remove() {
    sudo rm -f "$POWER_SUDOERS"
    ok "no sudo rule for the web page's Shut down / Reboot"
}

UNINSTALL=0
POWER_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --uninstall)  UNINSTALL=1 ;;
        --power-only) POWER_ONLY=1 ;;
        *) die "unknown option $arg (--uninstall, --power-only)" ;;
    esac
done

# --------------------------------------------- only the web page's rule ------
# For a computer that doesn't run the boot service (a desktop PC started with
# run.sh): nothing else is installed or changed. kraken_doa checks once at
# startup, so the buttons follow after the next restart of the software.
if [[ "$POWER_ONLY" == "1" ]]; then
    [[ "$RUN_USER" != "root" ]] || die "run it as the user that runs the KrakenSDR software (it asks for sudo itself)"
    if [[ "$UNINSTALL" == "1" || "$POWER_BUTTONS" != "1" ]]; then
        power_rule_remove
    else
        power_rule_install
        echo "   Restart the software (the page's Restart, or ./run.sh) to get the buttons."
    fi
    exit 0
fi

# ---------------------------------------------------------------- uninstall --
if [[ "$UNINSTALL" == "1" ]]; then
    sudo systemctl disable --now krakensdr.service 2>/dev/null || true
    sudo rm -f "$SVC" /usr/local/bin/kraken-term /usr/local/bin/kraken-wait-usb "$POWER_SUDOERS"
    # Only OUR drop-in: the directory may hold other packages' or the admin's
    # overrides, which an rm -rf of it used to delete too.
    sudo rm -f /etc/systemd/system/NetworkManager-wait-online.service.d/timeout.conf
    sudo rmdir --ignore-fail-on-non-empty /etc/systemd/system/NetworkManager-wait-online.service.d 2>/dev/null || true
    rm -f "$RUN_HOME/.config/autostart/kraken-term.desktop"
    sed -i '/kraken-term/d' "$RUN_HOME/.config/labwc/autostart" 2>/dev/null || true
    sed -i '/kraken-term/d' "$RUN_HOME/.config/wayfire.ini" 2>/dev/null || true
    strip_console_attach
    sudo systemctl daemon-reload
    ok "Uninstalled."
    exit 0
fi

# -------------------------------------------------------------- preflight ----
[[ -f "$RUN_SH" ]] || die "run.sh not found at $RUN_SH (set APP_DIR= to override)"
[[ "$BOOT_MODE" =~ ^(auto|desktop|console|keep)$ ]] || die "BOOT_MODE must be auto, desktop, console or keep"
chmod +x "$RUN_SH"

say "User $RUN_USER, app dir $APP_DIR, session '$SESSION'"
sudo apt-get install -y tmux lxterminal >/dev/null
ok "tmux + lxterminal present"

# ------------------------------------------------------- USB enumeration ------
# multi-user.target can be reached before all dongles enumerate on a cold boot.
sudo tee /usr/local/bin/kraken-wait-usb >/dev/null <<EOF
#!/bin/bash
WANT="\${KRAKEN_TUNERS:-$KRAKEN_TUNERS}"
for i in \$(seq 1 45); do
    n=\$(lsusb | grep -ciE '0bda:(2838|2832)' || true)
    [ "\$n" -ge "\$WANT" ] && exit 0
    sleep 1
done
echo "only \$n of \$WANT tuners enumerated; starting anyway" >&2
exit 0
EOF
sudo chmod +x /usr/local/bin/kraken-wait-usb

# ------------------------------------------- cap the network-online wait ------
# The unit waits for network-online (outbound reporting links), but a Pi booted
# where its configured WiFi doesn't exist must not stall. Cap the wait at 30s.
sudo mkdir -p /etc/systemd/system/NetworkManager-wait-online.service.d
sudo tee /etc/systemd/system/NetworkManager-wait-online.service.d/timeout.conf >/dev/null <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/bin/nm-online -s -q --timeout=30
EOF

# ------------------------------------------------------------ systemd unit ---
sudo tee "$SVC" >/dev/null <<EOF
[Unit]
Description=KrakenSDR Suite
# Wait for network so outbound links (RDF Mapper, MQTT) come up cleanly.
# The wait is capped at 30s by the NetworkManager-wait-online drop-in below,
# so a missing/slow WiFi network cannot stall boot; the stack starts anyway
# and the persistent kraken-term viewer tolerates any start delay.
After=network-online.target
Wants=network-online.target

[Service]
Type=forking
GuessMainPID=yes
User=$RUN_USER
Group=$RUN_USER
WorkingDirectory=${APP_DIR//%/%%}
Environment=$(sd_quote "HOME=$RUN_HOME")
Environment=TMUX_TMPDIR=/tmp
Environment=TMUX_SESSION=$SESSION
Environment=NO_ATTACH=1
Environment=WAIT_TIMEOUT=$WAIT_TIMEOUT
Environment=KRAKEN_TUNERS=$KRAKEN_TUNERS
Environment=PYTHONUNBUFFERED=1
$([[ "$POWER_BUTTONS" == "1" ]] || echo "# POWER_BUTTONS=0: the web page offers only Restart software (power.md)
Environment=KRAKEN_POWER=restart")
ExecStartPre=/usr/local/bin/kraken-wait-usb
ExecStart=$(sd_quote "$RUN_SH") $VARIANT_FLAGS
ExecStop=$(sd_quote "$RUN_SH") stop
# Realtime-priority limit for heimdall's SCHED_RR USB reader / sample-drain
# threads (RT_PRIO_* in heimdall_v2/config.h, <= 30). install.sh's
# limits.conf entry only applies to PAM logins, not to systemd services, so
# without this they silently ran at normal priority in the boot service.
LimitRTPRIO=30
# Locked-memory limit for heimdall's mlockall (keeps the pages those threads
# use in RAM). heimdall locks only with an unlimited limit (a finite one would
# make later allocations fail once reached); without it it logs once and runs
# as before. Same limits.conf caveat as above.
LimitMEMLOCK=infinity
# Covers the tmux server itself; the apps inside it are restarted by run.sh's
# supervisor if they crash (a dead pane never ended the unit's main process).
Restart=on-failure
RestartSec=15
TimeoutStartSec=120
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
EOF
ok "unit written"

# ------------------------------------------ web page: Shut down / Reboot -----
# The sudo rule (power_rule_install, above). POWER_BUTTONS=0 removes it and
# sets KRAKEN_POWER=restart in the unit (above), which hides the two computer
# buttons - also where the user has sudo without a password anyway (the
# Raspberry Pi OS default).
if [[ "$POWER_BUTTONS" == "1" ]]; then
    power_rule_install
else
    power_rule_remove
fi

# ------------------------------------------------------- viewer (one only) ---
# Persistent: retries for up to 10 min so a slow service start still gets a
# window. Logs to /tmp/kraken-term.log for diagnosis. Only defers to clients
# on a pty (visible terminals); a stale tty1 console client hidden under the
# desktop is evicted by attach -d rather than blocking the window.
sudo tee /usr/local/bin/kraken-term >/dev/null <<EOF
#!/bin/bash
exec >>/tmp/kraken-term.log 2>&1
echo "--- \$(date) start (pid \$\$)"
export TMUX_TMPDIR=/tmp
SESSION="\${TMUX_SESSION:-$SESSION}"
DEADLINE=\$(( \$(date +%s) + 600 ))

exec 9>/tmp/kraken-term.lock
flock -n 9 || { echo "another instance holds the lock; exiting"; exit 0; }

while [ "\$(date +%s)" -lt "\$DEADLINE" ]; do
    if ! tmux has-session -t "\$SESSION" 2>/dev/null; then
        sleep 2
        continue
    fi
    if tmux list-clients -t "\$SESSION" -F '#{client_tty}' 2>/dev/null | grep -q '/dev/pts/'; then
        echo "a desktop/ssh client is attached; done"
        exit 0
    fi
    echo "launching terminal"
    lxterminal --title="KrakenSDR" --geometry=150x45 -e "tmux attach -d -t \$SESSION" &
    sleep 5
    if tmux list-clients -t "\$SESSION" -F '#{client_tty}' 2>/dev/null | grep -q '/dev/pts/'; then
        echo "attached ok"
        exit 0
    fi
    echo "terminal failed to attach; retrying"
done
echo "gave up after 600s with no session"
exit 1
EOF
sudo chmod +x /usr/local/bin/kraken-term

# ------------------------------------------------------ autostart: pick ONE ---
# Bookworm labwc reads BOTH ~/.config/labwc/autostart and the XDG dir, so
# installing to every mechanism opens two windows on the same session.
asuser mkdir -p "$RUN_HOME/.config"
HAS_DESKTOP=0
dpkg -s raspberrypi-ui-mods >/dev/null 2>&1 && HAS_DESKTOP=1
[[ -d "$RUN_HOME/.config/labwc" || -f "$RUN_HOME/.config/wayfire.ini" ]] && HAS_DESKTOP=1

rm -f "$RUN_HOME/.config/autostart/kraken-term.desktop"
sed -i '/kraken-term/d' "$RUN_HOME/.config/labwc/autostart" 2>/dev/null || true
sed -i '/kraken-term/d' "$RUN_HOME/.config/wayfire.ini" 2>/dev/null || true

if [[ "$HAS_DESKTOP" == "1" ]]; then
    if [[ -d "$RUN_HOME/.config/labwc" ]] || command -v labwc >/dev/null 2>&1; then
        asuser mkdir -p "$RUN_HOME/.config/labwc"
        L="$RUN_HOME/.config/labwc/autostart"
        asuser touch "$L"
        echo "/usr/local/bin/kraken-term &" | asuser tee -a "$L" >/dev/null
        asuser chmod +x "$L"
        ok "autostart: labwc"
    elif [[ -f "$RUN_HOME/.config/wayfire.ini" ]]; then
        W="$RUN_HOME/.config/wayfire.ini"
        grep -q '^\[autostart\]' "$W" \
            && asuser sed -i '/^\[autostart\]/a kraken = /usr/local/bin/kraken-term' "$W" \
            || printf '\n[autostart]\nkraken = /usr/local/bin/kraken-term\n' | asuser tee -a "$W" >/dev/null
        ok "autostart: wayfire"
    else
        asuser mkdir -p "$RUN_HOME/.config/autostart"
        asuser tee "$RUN_HOME/.config/autostart/kraken-term.desktop" >/dev/null <<'EOF'
[Desktop Entry]
Type=Application
Name=KrakenSDR Terminal
Exec=/usr/local/bin/kraken-term
Terminal=false
EOF
        ok "autostart: XDG"
    fi
fi

# ------------------------------------------------------- console viewer ------
# Installed in both modes. Desktop autologin (B4) also autologins tty1 behind
# the desktop, where an attach -d would steal the session from the desktop
# window, so it only attaches when booted to the console target.
P="$RUN_HOME/.bash_profile"
strip_console_attach
if [[ ! -f "$P" ]]; then
    echo "$PROFILE_CHAIN" | asuser tee "$P" >/dev/null
elif ! grep -q '\.profile' "$P"; then
    warn "$P does not source ~/.profile; login shells will skip PATH/.bashrc setup"
fi
asuser tee -a "$P" >/dev/null <<EOF

# --- KrakenSDR console attach
if [ "\$(tty)" = "/dev/tty1" ] && [ -z "\${TMUX:-}" ] \\
   && [ "\$(systemctl get-default)" != "graphical.target" ]; then
    export TMUX_TMPDIR=/tmp
    for i in \$(seq 1 90); do tmux has-session -t $SESSION 2>/dev/null && break; sleep 1; done
    tmux attach -d -t $SESSION
fi
# --- end KrakenSDR
EOF
ok "console viewer on tty1 (console boot only)"

# ------------------------------------------------------------- tmux config ---
T="$RUN_HOME/.tmux.conf"
asuser touch "$T"
grep -q aggressive-resize "$T" || echo "set -g aggressive-resize on" | asuser tee -a "$T" >/dev/null

# ----------------------------------------------------------------- autologin --
# Only picks the initial mode; both viewers stay installed either way.
if [[ "$BOOT_MODE" == "auto" ]]; then
    [[ "$HAS_DESKTOP" == "1" ]] && BOOT_MODE=desktop || BOOT_MODE=console
fi
case "$BOOT_MODE" in
    desktop)
        [[ "$HAS_DESKTOP" == "1" ]] || warn "BOOT_MODE=desktop but no desktop detected"
        sudo raspi-config nonint do_boot_behaviour B4 2>/dev/null || warn "set desktop autologin manually" ;;
    console)
        sudo raspi-config nonint do_boot_behaviour B2 2>/dev/null || warn "set console autologin manually" ;;
    keep)
        ok "boot mode left as $(systemctl get-default)" ;;
esac

# ------------------------------------------------- force HDMI (headless boot) --
# With no monitor at boot, KMS creates no framebuffer -> the desktop never
# starts -> no terminal for a later-plugged monitor or VNC. Forcing the
# connector fixes all three. Works on Pi 4 and Pi 5 (hdmi_force_hotplug does
# not work under KMS; this kernel parameter is the supported method).
# Override the mode by editing cmdline.txt if your display is not 1080p60.
CMD=/boot/firmware/cmdline.txt
[[ -f "$CMD" ]] || CMD=/boot/cmdline.txt
if [[ -f "$CMD" ]] && ! grep -q 'video=HDMI-A-1' "$CMD"; then
    sudo cp "$CMD" "$CMD.bak"
    # cmdline.txt must stay a single line; strip trailing newlines, append, re-add one.
    sudo sed -i -z 's/\n*$//' "$CMD"
    echo -n " video=HDMI-A-1:1920x1080@60D" | sudo tee -a "$CMD" >/dev/null
    echo | sudo tee -a "$CMD" >/dev/null
    ok "forced HDMI-A-1 1920x1080@60 (backup: $CMD.bak)"
fi

# Enable VNC so headless users can see the terminal (wayvnc on Bookworm).
if [[ "$HAS_DESKTOP" == "1" ]]; then
    sudo raspi-config nonint do_vnc 0 2>/dev/null && ok "VNC enabled" || warn "enable VNC manually if wanted"
fi

sudo systemctl daemon-reload
sudo systemctl enable krakensdr.service >/dev/null
ok "service enabled"

cat <<EOF

  Installed. Reboot to start on boot:

    sudo reboot

  Check after reboot:
    systemctl status krakensdr           # active (running)
    journalctl -u krakensdr -b

  Operate:
    sudo systemctl {start,stop,restart} krakensdr
    kraken-term                          # open a viewer window
    tmux attach -d -t $SESSION      # attach from SSH

  While the service is running, use systemctl / kraken-term rather than
  running run.sh by hand.

  Remove with:  ./install-pi-service.sh --uninstall

EOF
