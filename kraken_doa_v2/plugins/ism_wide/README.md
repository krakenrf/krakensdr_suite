# ISM band devices, full band (`ism_wide`)

The `ism` decoder on the whole 2.4 MHz the tuner receives: weather
stations, tyre pressure sensors, key fobs and remotes, alarm sensors and
energy meters anywhere in the band, each with its own DoA.

Pick "ISM devices, full band (2.4 MHz)" in a VFO card's Digital decoder
list: the VFO moves to the tuner's centre with the 2.4 MHz bandwidth and is
drawn as a single line that can't be moved or resized (like ADS-B). Tune the
receiver to the band - 433.920 MHz covers 432.72-435.12 MHz.

The sources are `plugins/ism/` (this folder's `USES` file compiles them in;
`decoder.cpp` here only declares the plugin). How it works, options and test
results: `plugins/ism/README.md`, section "The whole 2.4 MHz: `ism_wide`".
