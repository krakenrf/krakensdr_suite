# Third-party notices - KrakenSDR DoA (kraken_doa_v2)

kraken_doa_v2 is licensed under the KrakenSDR Source Available Licence
(`LICENSE` in this folder). It contains, links or works with the components
below, which belong to their authors and keep their own licences; the
KrakenSDR licence does not apply to them. Binary packages of the suite
(`windows/package.sh`, the Docker image) carry the complete licence text of
every component in their `licenses/` folder.

No component of kraken_doa or its decoder plugins is under the GPL, AGPL or
LGPL (glibc on Linux and the GCC runtime are system libraries, see below).

## 1. Third-party code in this folder

| Component | Where | Licence |
|---|---|---|
| pffft (marton78 fork, commit aa16fd3) | `external/pffft/` - kraken_doa and every plugin (`libkrakendig.a`) | BSD-style (FFTPACK), text A |
| moodycamel ConcurrentQueue | `include/concurrentqueue.h` | Simplified BSD or Boost Software License 1.0, text B |
| uPlot 1.6.30 | inlined at the top of `kraken_doa.html` | MIT, text C |
| opus-decoder (wasm-audio-decoders) incl. simple-yenc | `opus-decoder.min.js` | MIT, text D |
| libopus, compiled to WebAssembly inside `opus-decoder.min.js` | `opus-decoder.min.js` | BSD-3-Clause, text E |
| mbelib - the IMBE decoder ported to C++ and its tables | `plugins/lib/dig_vocoder.cpp`, `plugins/lib/mbe_tables.hpp` | ISC, text F |
| DSD - frame deinterleave tables | `plugins/lib/mbe_tables.hpp` | ISC, text F |
| NTIA/ITS Irregular Terrain Model (ITM) 1.3, ported to JavaScript | `kraken_doa.html` ("📶 ITM" block) | not subject to copyright in the US; NTIA terms, text G |
| OGN tracker protocol: the LDPC(208,160) parity-check matrix (data only, no code) | `plugins/flarm/ogn_ldpc_matrix.hpp` | interoperability data - the code every OGN tracker transmits; taken from the OGN tracker source (github.com/pjalocha/esp32-ogn-tracker, Pawel Jalocha), which states no licence |

| ra-firmware (DF9DQ's "Ra" radiosonde receiver firmware): the RS41 and M10 / M20 sensor computations follow its metrology code | `plugins/radiosonde/rs41.cpp`, `plugins/radiosonde/modem.cpp` | BSD-3-Clause, text H |

The decoder plugins are written from the published standards and protocol
descriptions; some protocol facts (frame layouts, constants, calibration
fits) were first published by other projects' reverse engineering and are
used as facts, without their code. Each plugin's README lists its sources.

## 2. Libraries built with or linked into the programs

| Component | Version | Used by | Licence | Source |
|---|---|---|---|---|
| uWebSockets + uSockets | commit fe7da4cb (`UWS_COMMIT`) | kraken_doa | Apache-2.0 | https://github.com/uNetworking/uWebSockets |
| uWebSockets `MoveOnlyFunction.h` | (part of uWebSockets) | kraken_doa | MIT - Copyright (c) 2020 Oleg Fatkhiev | as above |
| liquid-dsp | v1.7.0 (4dda702, `liquid.mk`), built without FFTW / libfec | kraken_doa, plugins | MIT - Copyright (c) 2007 - 2016 Joseph Gaeddert | https://github.com/jgaeddert/liquid-dsp |
| Eigen | the system's (Debian 3.4, MSYS2 5.x), headers only, unmodified | kraken_doa | MPL-2.0 | https://gitlab.com/libeigen/eigen |
| OpenSSL | 3.x | kraken_doa | Apache-2.0 | https://www.openssl.org/source/ |
| Opus (libopus) | the system's | kraken_doa | BSD-3-Clause (text E) | https://opus-codec.org/ |
| zlib | the system's | kraken_doa | zlib | https://zlib.net/ |
| SQLite | the system's | kraken_doa | public domain | https://sqlite.org/ |
| libuv | MSYS2's (Windows only) | kraken_doa.exe | MIT | https://github.com/libuv/libuv |
| dlfcn-win32 | MSYS2's (Windows only) | plugin .exe files | MIT | https://github.com/dlfcn-win32/dlfcn-win32 |
| MinGW-w64 runtime + winpthreads | MSYS2's (Windows only) | every .exe | ZPL-2.1 / MIT / BSD (`COPYING.MinGW-w64-runtime.txt`) | https://www.mingw-w64.org/ |
| GCC runtime (libstdc++, libgcc) | | every program | GPL-3.0 with the GCC Runtime Library Exception | https://gcc.gnu.org/ |
| GNU C library (glibc) | the system's (Linux, linked dynamically) | every program | LGPL-2.1-or-later | https://www.gnu.org/software/libc/ |
| whisper.cpp + ggml (optional Voice to Text helper `transcribe/build/kraken_whisper`) | v1.9.4 (`transcribe/setup.sh`) | kraken_whisper | MIT - Copyright (c) 2023-2026 The ggml authors | https://github.com/ggml-org/whisper.cpp |

## 3. Software you install separately

These are not part of the KrakenSDR software and are not distributed with
it. You obtain them yourself, under their own terms, and are responsible for
having the rights (including any patent rights) to use them.

- **Speech models** for Voice to Text: OpenAI Whisper models (MIT, ggml
  conversions from huggingface.co/ggerganov/whisper.cpp) and Silero VAD (MIT).
- **Voice codecs**: mbelib for AMBE / AMBE+2 (ISC; loaded at run time - the
  AMBE+2 algorithm may be covered by DVSI patents in some countries), the ETSI
  TETRA codec (ETSI's terms, built by you).
- **AI assistant**: Claude Code (Anthropic's terms), Python with numpy / scipy
  (BSD-3-Clause), bubblewrap, socat, curl.

## 4. Online services and map data

The web UI and some features fetch data from online services, directly from
your browser or from the receiver. Their terms apply to that use, and some
restrict heavy or commercial use - check them before using the software in a
commercial product or service.

- **OpenStreetMap** map tiles (tile.openstreetmap.org), the Overpass API
  (masts layer) and Nominatim (incident address search): data © OpenStreetMap
  contributors, available under the Open Database License (ODbL) -
  https://www.openstreetmap.org/copyright. Usage policies:
  https://operations.osmfoundation.org/policies/
- **Esri World Imagery** (satellite layer): © Esri and its data providers;
  Esri's terms of use apply.
- **AWS Terrain Tiles** (viewshed / path loss terrain, Mapzen terrarium):
  attributions per data source -
  https://github.com/tilezen/joerd/blob/master/docs/attribution.md
- **KrakenSDR web mapper** (map.krakenrf.com): KrakenRF's service.

## 5. Licence texts of the components in this folder

### A. pffft

```
Copyright (c) 2020  Dario Mambro ( dario.mambro@gmail.com )
Copyright (c) 2019  Hayati Ayguen ( h_ayguen@web.de )
Copyright (c) 2013  Julien Pommier ( pommier@modartt.com )

Copyright (c) 2004 the University Corporation for Atmospheric
Research ("UCAR"). All rights reserved. Developed by NCAR's
Computational and Information Systems Laboratory, UCAR,
www.cisl.ucar.edu.

Redistribution and use of the Software in source and binary forms,
with or without modification, is permitted provided that the
following conditions are met:

- Neither the names of NCAR's Computational and Information Systems
Laboratory, the University Corporation for Atmospheric Research,
nor the names of its sponsors or contributors may be used to
endorse or promote products derived from this Software without
specific prior written permission.

- Redistributions of source code must retain the above copyright
notices, this list of conditions, and the disclaimer below.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions, and the disclaimer below in the
documentation and/or other materials provided with the
distribution.

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE
SOFTWARE.
```

### B. moodycamel ConcurrentQueue

```
Simplified BSD License:

Copyright (c) 2013-2020, Cameron Desrochers.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this list of
conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or other materials
provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

(Also offered by its author under the Boost Software License 1.0 - see
`heimdall_v2/external/concurrentqueue/LICENSE.md`, the upstream licence file.)

### C. uPlot

```
The MIT License (MIT)

Copyright (c) 2022 Leon Sorokin

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### D. opus-decoder / wasm-audio-decoders, simple-yenc

```
Copyright 2021-2025 Ethan Halsall
https://github.com/eshaz/wasm-audio-decoders

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### E. Opus (libopus)

```
Copyright 2001-2023 Xiph.Org, Skype Limited, Octasic,
                    Jean-Marc Valin, Timothy B. Terriberry,
                    CSIRO, Gregory Maxwell, Mark Borgerding,
                    Erik de Castro Lopo, Mozilla, Amazon

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of Internet Society, IETF or IETF Trust, nor the
names of specific contributors, may be used to endorse or promote
products derived from this software without specific prior written
permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Opus is subject to the royalty-free patent licenses which are
specified at:

Xiph.Org Foundation:
https://datatracker.ietf.org/ipr/1524/

Microsoft Corporation:
https://datatracker.ietf.org/ipr/1914/

Broadcom Corporation:
https://datatracker.ietf.org/ipr/1526/
```

### F. mbelib and DSD (ISC licence)

```
mbelib:
Copyright (C) 2010 mbelib Author
GPG Key ID: 0xEA5EFE2C (9E7A 5527 9CDC EBF7 BF1B  D772 4F98 E863 EA5E FE2C)

DSD:
Copyright (C) 2010 DSD Author
GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL ISC BE LIABLE FOR ANY
SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

### G. NTIA/ITS Irregular Terrain Model

The path-loss tool's Longley-Rice model (`kraken_doa.html`, "📶 ITM" block)
is a JavaScript port of the Irregular Terrain Model (ITM) 1.3, created by the
Institute for Telecommunication Sciences of the National Telecommunications
and Information Administration (NTIA/ITS), https://github.com/NTIA/itm.
Changes (2026-10): translated from C++ to JavaScript (complex numbers as
`[re, im]` pairs), ITM's area mode left out, results returned as an object.
NTIA's software disclaimer / release:

```
SOFTWARE DISCLAIMER / RELEASE

This software was developed by employees of the National Telecommunications
and Information Administration (NTIA), an agency of the Federal Government and
is provided to you as a public service. Pursuant to Title 15 United States
Code Section 105, works of NTIA employees are not subject to copyright
protection within the United States.

The software is provided by NTIA "AS IS." NTIA MAKES NO WARRANTY OF ANY KIND,
EXPRESS, IMPLIED OR STATUTORY, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
WARRANTY OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, NON-INFRINGEMENT
AND DATA ACCURACY. NTIA does not warrant or make any representations regarding
the use of the software or the results thereof, including but not limited to
the correctness, accuracy, reliability or usefulness of the software.

To the extent that NTIA holds rights in countries other than the United
States, you are hereby granted the non-exclusive irrevocable and unconditional
right to print, publish, prepare derivative works and distribute the NTIA
software, in any medium, or authorize others to do so on your behalf, on a
royalty-free basis throughout the World.

You may improve, modify, and create derivative works of the software or any
portion of the software, and you may copy and distribute such modifications or
works. Modified works should carry a notice stating that you changed the
software and should note the date and nature of any such change.

You are solely responsible for determining the appropriateness of using and
distributing the software and you assume all risks associated with its use,
including but not limited to the risks and costs of program errors,
compliance with applicable laws, damage to or loss of data, programs or
equipment, and the unavailability or interruption of operation. This software
is not intended to be used in any situation where a failure could cause risk
of injury or damage to property.

Please provide appropriate acknowledgments of NTIA's creation of the software
in any copies or derivative works of this software.
```

### H. ra-firmware

```
BSD 3-Clause License

Copyright (c) 2017, einergehtnochrein
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
