# KrakenSDR IQ recordings (SigMF)

The 📼 IQ & Audio Recorder (sidebar) writes coherent multi-antenna IQ
recordings in the open [SigMF](https://sigmf.org) format (specification
1.2). This file describes the layout and every field it writes, so the
recordings can be read by other software (and other software's recordings
played here).

## Why a collection + an archive

A KrakenSDR records 5 (up to 8) channels that only mean something together:
the phases between them carry the direction. SigMF offers two ways to store
several channels:

- `core:num_channels`: all channels interleaved sample by sample in ONE
  dataset. The spec says it is meant for audio / oscilloscope channels and
  recommends Collections for multi-channel IQ.
- a **Collection** (`.sigmf-collection`): one ordinary SigMF Recording per
  channel, plus a small file naming them in order with the SHA-512 of each
  metadata file - the spec's answer for "multiple channels from a phased
  array". Each antenna's file is a normal single-channel SigMF Recording
  that any SigMF tool (inspectrum, GNU Radio, sigmf-python ...) opens alone.

The recorder uses a **Collection**, kept together in **one folder** per
recording. For **transport** the folder is packed into one **SigMF Archive**
(`<name>.sigmf` = a POSIX.1-2001 tar file, uncompressed, holding the same
folder). The archive is made on the fly by the download - the recording is
never stored twice on the receiver - and is what the receiver accepts as an
upload. Uncompressed tar members are contiguous, so the receiver plays an
archive in place (memory-mapped, nothing extracted); files of 8 GiB and more
get a pax header (POSIX.1-2001), as the spec's tar format allows.

```
iq_20261016_181503_433_920MHz/                    the recording's folder
  iq_20261016_181503_433_920MHz.sigmf-collection    the antennas, in order + SHA-512s
  iq_20261016_181503_433_920MHz_ch0.sigmf-meta      antenna 0 (ANT0)
  iq_20261016_181503_433_920MHz_ch0.sigmf-data
  iq_20261016_181503_433_920MHz_ch1.sigmf-meta      antenna 1 ...
  iq_20261016_181503_433_920MHz_ch1.sigmf-data
  ...

iq_20261016_181503_433_920MHz.sigmf               the download: a tar of that folder
```

## Two kinds of IQ recording

| `krakensdr:format` | What | Datatype | Rate | Data rate (5 antennas) |
|---|---|---|---|---|
| `full` | heimdall's 8091 stream itself: the whole 2.4 MHz, already phase-calibrated (the bytes as they arrive) | `cu8` | 2.4 MHz | 24 MB/s |
| `band` | one VFO's band, decimated by the receiver (the VFO's bandwidth option) | `ci16_le`, `krakensdr:iq_scale` 16384 counts = the receiver's full scale | 2.4 MHz / N | e.g. 1 MB/s at 50 kHz |

An SD card takes roughly 8-20 MB/s: a full-band recording is buffered in
memory and written after it ends (only short ones fit; the recorder refuses
a length the buffer can't cover once the drive's speed is known - "Test
write speed" or an earlier recording). Use the VFO band or a USB SSD for
long recordings.

## Per-antenna metadata (`<name>_ch<k>.sigmf-meta`)

```json
{"global": {
   "core:datatype": "cu8",                  "ci16_le" for a VFO band
   "core:sample_rate": 2400000.0,
   "core:version": "1.2.0",
   "core:description": "KrakenSDR coherent IQ recording, antenna 1 of 5 (ANT1), full band 2.4 MHz - <notes>",
   "core:author": "<station ID>",
   "core:recorder": "KrakenSDR kraken_doa",
   "core:hw": "KrakenSDR, 5 coherent RTL-SDR (R820T) channels, heimdall_v2 phase calibration",
   "core:collection": "<name>",
   "core:sha512": "<the dataset's SHA-512>",  written when the recording ends
   "core:extensions": [{"name": "spatial", "version": "1.1.0", "optional": true},
                       {"name": "krakensdr", "version": "1.0.0", "optional": true}],
   "core:geolocation": {"type": "Point", "coordinates": [lon, lat]},   with "save the station's position"
   "spatial:num_elements": 5,
   "spatial:channel_index": 1,
   "krakensdr:format": "full",
   "krakensdr:iq_scale": 16384                 VFO band only
 },
 "captures": [ { "core:sample_start": 0, "core:global_index": 0,
                 "core:frequency": 433920000,          tuner centre (full) / the VFO's centre (band), Hz
                 "core:datetime": "2026-10-16T05:15:03.124Z",
                 "spatial:aperture_azimuth": 45.0,     the station's heading, when known
                 "krakensdr:gain_db": 49.6,            this antenna's gain
                 "krakensdr:phase_state": 4,           heimdall's phase calibration state (4 = converged)
                 "krakensdr:noise_source": false },
               ... ],
 "annotations": []}
```

A new **capture segment** starts when the frequency or a gain changes, when
heimdall's calibration state changes between converged and not, at every
**noise-source calibration burst** (those segments carry
`"spatial:calibration": {"caltype": "other", "cal_geometry": {"unknown": true}}`
and `krakensdr:noise_source` true - the samples are the calibration noise,
not the antennas; playback raises heimdall's noise flag so the DoA holds
exactly as it did live) and after a **gap**: samples heimdall marked as
retuning (noise) are not recorded, and `core:global_index` counts them
(`global_index - sample_start` = samples missing before that segment).

## The collection (`<name>.sigmf-collection`)

```json
{"collection": {
   "core:version": "1.2.0",
   "core:description": "KrakenSDR coherent IQ recording: 5 antennas, full band 2.4 MHz - <notes>",
   "core:author": "<station ID>",
   "core:extensions": [ ...as above... ],
   "core:streams": [{"name": "<name>_ch0", "hash": "<SHA-512 of <name>_ch0.sigmf-meta>"}, ...],
   "spatial:element_geometry": [{"point": [0.12, 0.0, 0.0]}, ...],     metres, see below
   "krakensdr:format": "full",
   "krakensdr:channels": 5,
   "krakensdr:start": "2026-10-16T05:15:03.127Z",
   "krakensdr:seconds": 10.0,
   "krakensdr:complete": true,                 false while being written (or after a crash)
   "krakensdr:stop_reason": "time limit",      stopped | the drive is full ... | kraken_doa shut down
   "krakensdr:gap_samples": 0,                 only when samples were left out
   "krakensdr:array_commands": ["TOPOLOGY:UCA", "RADIUS:120", "SPACING:30", "ULA_MODE:BOTH", ...],
   "krakensdr:vfos": "DECIMATORS:0|100000.00,7,NBFM,...",   the VFOs at the start
   "krakensdr:band": {"vfo": 1, "offset_hz": -300000.0, "bandwidth_hz": 50000.0,
                      "tuner_hz": 433920000, "decimation": 48},   VFO band only
   "krakensdr:receiver": {"kerberos": false, "wideband_variant": false},
   "krakensdr:station": {"id": "...", "lat": 51.4779, "lon": -0.0015, "heading": 45.0, "source": "gps"},
   "krakensdr:track": [[t_s, lat, lon, heading], ...]   a moving station (GPS), ~1 Hz
 }}
```

`spatial:element_geometry` is in the **receiver's own frame**: x towards
ANT0 (the array's front), y to the left, z up, metres - the frame the
receiver's MUSIC uses (a UCA wired clockwise: ANT1 at -72°; a ULA along x
from ANT0). `krakensdr:array_commands` are the receiver's own array settings
at the start, exactly as its settings store keeps them - the playback puts
them back. `krakensdr:station` / `krakensdr:track` (and `core:geolocation`)
are only written with "Save the station's position with it" ticked
(default on): leave it off before sharing a recording if the position is
private.

## Reading a recording elsewhere

numpy, one antenna (cu8):

```python
import numpy as np
d = np.fromfile('name_ch0.sigmf-data', np.uint8).astype(np.float32)
x = (d[0::2] - 127.5) / 127.5 + 1j * (d[1::2] - 127.5) / 127.5
```

[sigmf-python](https://github.com/sigmf/sigmf-python) (checked with 1.13):

```python
from sigmf import sigmffile
import numpy as np
c = sigmffile.fromfile('name/name.sigmf-collection')   # validates + checks the hashes
X = np.array([c.get_SigMFFile(stream_index=k).read_samples() for k in range(len(c.get_stream_names()))])
```

sigmf-python's `fromarchive()` (1.13) reads only one recording per archive -
unpack the archive first (`tar xf name.sigmf`) and open the collection as
above.

## Recordings from elsewhere

The receiver plays any SigMF recording it can map onto its pipeline (copy it
into the recordings folder, or ⬆ Upload a `.sigmf` archive):

- a folder or archive holding a `.sigmf-collection` (channels in its order,
  or by `spatial:channel_index` when every stream has one), or loose
  `.sigmf-meta` files (by name), or one `.sigmf-meta`
- `core:num_channels` > 1 (interleaved) datasets too; 1 to 8 channels in all
- complex datatypes `cu8`, `ci8`, `cu16`/`ci16`/`cu32`/`ci32` and
  `cf32`/`cf64`, `_le` or `_be` (integers scaled from their full scale,
  data far outside the receiver's range brought into it)
- sample rate 2.4 MHz, or 2.4 MHz divided by a whole number (interpolated)
- non-conforming datasets via `core:dataset` (a header only before the first
  sample, `core:trailing_bytes`)
- array geometry from `krakensdr:array_commands`, else from
  `spatial:element_geometry` (as custom positions, receiver frame)
