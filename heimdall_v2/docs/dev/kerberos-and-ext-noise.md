# KerberosSDR mode and the external noise source array

Moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every heimdall session needs). Not loaded automatically. *Italic*
names refer to sections of the other files in this folder (index:
`README.md`); "above" / "below" meant elsewhere in the old file.

### KerberosSDR Mode (--kerberos / --kerberos_sw)

KerberosSDR has no noise-source RF switch (noise couples in via a directional
coupler), so calibration is only valid with the antennas manually
disconnected. With `--kerberos`, every automatic noise-on path is suppressed
(`kerberos_manual_cal_only()` in core/config.hpp guards them all):

- Startup, coherence loss and element-count reconfiguration land in
  `kerberos_enter_uncalibrated()` (compensation.cpp): noise off, FFT off
  (idles the lag/phase machines, same trick as the watchdog's 120s abort),
  machines reset, identity compensation, `kerberos_cal_stale` cleared
- Frequency/gain changes keep the old compensation applied and set
  `kerberos_cal_stale` (STALE) instead of entering the cooldown recal
  (web_server.cpp SDR_SETTINGS, tcp_control_server.cpp set_frequency,
  handle_settings_change); the periodic monitor never checks
- The ONLY calibration trigger is FORCE_RECAL (web UI "Full Recalibration" button, confirm
  dialog): the watchdog calls `recover_coherence(manual=true)`, which runs
  the normal full noise-source recalibration; convergence clears the stale
  flag in `complete_phase_calibration_locked()`
- Status: `kerberos_calibration_state()` returns
  uncalibrated/calibrating/calibrated/stale - reported in the 8092 status
  JSON, the web STATE message (banner + greyed periodic-recal controls), and
  as HIGH BITS of the 8091 header's phase-state field (bit 8 = kerberos,
  bit 9 = stale; the DoA client and GR source mask the low byte)
- Element count defaults to 4 in kerberos mode (4-channel hardware; the
  persisted setting and USB auto-detect are skipped) - only `-n` overrides
- `--kerberos_sw` implies kerberos_mode but keeps automatic calibration:
  `src/sdr/kerberos_gpio.cpp` drives the CKOVAL antenna switches from Pi
  header GPIOs 23/24 (`KERBEROS_SW_GPIO_ANT1/ANT2` in config.h) via the
  linux/gpio.h v2 character-device API on the `pinctrl-*` gpiochip (found by
  label - rp1 on Pi 5, bcm2711 on Pi 4; pigpio is NOT used, it's broken on
  RP1). Idle 23=1/24=0 (antenna input 1); `kerberos_gpio_set_noise_path()`
  is called from `set_bias_tee_all_devices()` and the device-open path: noise
  on = save selection + both LOW, noise off = restore. Init in main() before
  device init; falls back to plain --kerberos (manual cal) if the GPIOs are
  unavailable; cleanup at shutdown restores the antenna selection and
  releases the lines. The 8091 kerberos header bit is NOT set in _sw mode
  (clients treat it as a normal KrakenSDR)

### External Noise Source Array (--ext_noise)

Standard KrakenSDR + an add-on antenna array whose noise source is powered from
the CH0 antenna bias tee. `external_noise_mode` (core/config.hpp, set by
`--ext_noise` / `--ext-noise`): `set_bias_tee_all_devices()` holds the internal
noise source (GPIO0, every chip) OFF and switches GPIO1 of the channel-0 chip
(`set_external_noise_power`) instead; `init_rtlsdr_device()` forces GPIO0 off
and `open_active_devices()` powers the external source for the startup
calibration. `apply_antenna_bias_tees()` treats CH0 as reserved (bit 0 refused
and cleared, GPIO1 never written from the mask). Everything else - calibration
state machines, `bias_tee_enabled` semantics, the 8091 noise flag - is
unchanged. Exclusive with `--wideband` and `--kerberos(_sw)` (refused at
startup).

