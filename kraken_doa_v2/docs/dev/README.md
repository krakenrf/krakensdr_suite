# kraken_doa developer notes

One file per topic, moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24. Not
loaded into an AI session automatically: `kraken_doa_v2/CLAUDE.md` holds the rules
every session needs and points here.

| File | Topic |
|---|---|
| `ai-assistant.md` | AI assistant |
| `array-calculator.md` | Antenna array calculator |
| `array-calibration.md` | Array calibration from ADS-B aircraft |
| `configuration.md` | Configuration, MUSIC DoA, UCA element ordering |
| `debug-playback.md` | Debug playback |
| `decoder-log.md` | Decoder data log |
| `decoder-plugins.md` | Decoder plugins |
| `digital-decoders.md` | Digital voice / data decoders (engine, protocol receivers, decoder tabs) |
| `digital-squelch.md` | Digital squelch |
| `digital-voice.md` | Digital voice |
| `doa-per-talker.md` | DoA per talker |
| `drive-mode.md` | Drive mode: DF from a vehicle (the map full-window, one big bearing, big buttons, Day / Night) |
| `fft-pffft.md` | FFT (pffft) |
| `fixed-frequency-decoders.md` | Fixed-frequency decoders |
| `incident-map.md` | Incident map |
| `liquid-dsp.md` | liquid-dsp |
| `log-replay.md` | Log replay |
| `map-search.md` | Map search, 🏠 / 🎯 |
| `map.md` | Map (right-hand pane) and decoder map points |
| `mobile-df.md` | Mobile direction finding |
| `noise-floor.md` | Noise floor (the spectrum's 0 dB) |
| `trunk-following.md` | Trunk following: grants of a trunked system's control channel -> temporary voice VFOs (optionally retuning: excursions outside the span, voice tuners; the control-channel search, its memory, click to follow) |
| `bearing-spectrum.md` | Bearing spectrum: the spectrum coloured by direction (type 9) |
| `operating-modes.md` | Operating modes, VFO cards, VFO tuning step, spectrum zoom |
| `osm-masts.md` | OpenStreetMap masts layer |
| `overview.md` | Overview: build, architecture, threads, components |
| `patch-3d-topology.md` | Patch / 3D topology |
| `path-loss.md` | Path loss (the viewshed's Signal mode) |
| `plugin-acars.md` | ACARS (VHF) plugin |
| `plugin-adsb.md` | ADS-B plugin |
| `plugin-ais.md` | AIS plugin |
| `plugin-analog-id.md` | Analog FM IDs plugin |
| `plugin-analogtv.md` | Analog TV plugin (PAL / NTSC luma, the video display) |
| `plugin-beacon406.md` | 406 MHz distress beacons plugin |
| `plugin-beacon121.md` | 121.5 / 243 MHz swept-tone distress beacons plugin |
| `plugin-inmarsat-c.md` | Inmarsat-C (STD-C) TDM / EGC plugin; survey of the other Inmarsat services |
| `plugin-inmarsat-aero.md` | Inmarsat Classic Aero plugin (P channel at every rate, ACARS / ADS-C, C channel voice, aircraft R / T bursts; All channels in the spectrum - every channel of the tuner's 2.4 MHz at once, the VFO locked on the band) |
| `plugin-cells.md` | LTE / 5G NR Cell Decoder plugin |
| `plugin-dab.md` | DAB / DAB+ plugin |
| `plugin-dme.md` | DME / TACAN plugin |
| `plugin-dtv.md` | Digital TV plugin |
| `plugin-flarm.md` | FLARM / OGN plugin |
| `plugin-ils.md` | ILS / VOR / marker plugin |
| `plugin-ism.md` | ISM devices plugin |
| `plugin-lora.md` | LoRa / Meshtastic / LoRaWAN plugin |
| `plugin-pagers.md` | Pagers plugin (POCSAG + FLEX) |
| `plugin-radar.md` | Pulse radar plugin |
| `plugin-radiosonde.md` | Radiosonde plugin |
| `plugin-rds.md` | RDS / RBDS + SCA plugin |
| `plugin-ssr1030.md` | 1030 MHz interrogations plugin |
| `plugin-tempest.md` | TEMPEST screen-reconstruction plugin |
| `plugin-uat.md` | UAT plugin |
| `plugin-vdl2.md` | VDL Mode 2 plugin |
| `plugin-wildlife.md` | Wildlife collars plugin |
| `range.md` | Range from signal strength, a VFO's level on mouseover |
| `recorder.md` | IQ & audio recorder and playback of recordings |
| `reference.md` | Hot paths, development tasks, key files, common issues, testing, dependencies |
| `saved-frequencies.md` | Saved frequencies and map markers |
| `saved-frequency-scan.md` | Saved-frequency scan |
| `signal-classifier.md` | Signal ID: the signal classifier (core, training, runtime, labels on the spectrum); the Bands bar |
| `spectrum-views.md` | Spectrum views (FFT message type 7) |
| `station-on-map.md` | Station on the map |
| `sweep-mode.md` | Sweep mode: the tuners step through a range, one wide spectrum + waterfall (FFT types 10 / 11, the gain by region) |
| `tempest-deep-plan.md` | TEMPEST deep-reconstruction plan |
| `vfo-audio.md` | VFO audio |
| `vfo-bandwidth.md` | Arbitrary VFO bandwidth and the VFO decimator |
| `viewshed.md` | Viewshed |
| `voice-to-text.md` | Voice to Text |
| `web-mapper.md` | Web Mapper output |
| `wideband-variant.md` | KrakenSDR Wideband (downconverter) variant |
