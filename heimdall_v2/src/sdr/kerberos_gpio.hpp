#pragma once

// CKOVAL third-party antenna switch support for --kerberos_sw.
//
// A modified KerberosSDR has external antenna switches driven by two
// Raspberry Pi header GPIOs (KERBEROS_SW_GPIO_ANT1/ANT2 in config.h, default
// 23/24 - the pins the original heimdall_daq_fw V1 rtl_daq.c used). Idle
// state selects antenna input 1 (ANT1=1, ANT2=0). While the calibration
// noise source is on, BOTH lines are driven LOW, disconnecting the antennas
// so the directional-coupler noise is the only thing the tuners see - which
// is what makes fully automatic calibration valid again on this hardware.
//
// Implemented with the linux/gpio.h v2 character-device API on /dev/gpiochipN
// (pigpio does not work on the Pi 5's RP1). The header controller is found by
// its "pinctrl-*" chip label (rp1 on Pi 5, bcm2711 on Pi 4, ...), and the
// whole module only activates on a Raspberry Pi (/proc/device-tree/model).
// Off Linux (Windows) the functions are stubs: init fails, nothing is active.

// Claim the GPIO lines and drive the idle state (ANT1=1, ANT2=0). Returns
// false - with the reason on stderr - when not running on a Raspberry Pi or
// the gpiochip is unavailable; the caller then falls back to plain --kerberos
// (manual calibration), because automatic calibration without switch control
// would calibrate against live antennas.
bool kerberos_gpio_init();

// True once kerberos_gpio_init() succeeded (lines are claimed).
bool kerberos_gpio_active();

// Route the antennas (noise_on=false: restore the saved selection) or the
// noise source (noise_on=true: save the selection, drive both lines low).
// Thread-safe; no-op unless kerberos_gpio_active().
void kerberos_gpio_set_noise_path(bool noise_on);

// Restore the idle antenna selection and release the lines.
void kerberos_gpio_cleanup();
