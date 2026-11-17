# Third-party notices - KrakenSDR DoA (kraken_doa_v2)

kraken_doa_v2 is licensed under the KrakenSDR Source Available Licence
(`LICENSE` in this folder). It contains, links or works with the components
below, which belong to their authors and keep their own licences; the
KrakenSDR licence does not apply to them. Binary packages of the suite
(`windows/package.sh`, the Docker image) carry the complete licence text of
every component in their `licenses/` folder.

No component of kraken_doa, its plugin library (`plugins/lib`) or the plugin
SDK is under the GPL, AGPL or LGPL (glibc on Linux and the GCC runtime are
system libraries, see below). One decoder plugin is itself a GPL program,
built separately from them: `plugins/ism` and `plugins/flarm` (section 1a).

## 1. Third-party code in this folder

| Component | Where | Licence |
|---|---|---|
| pffft (marton78 fork, commit aa16fd3) | `external/pffft/` - kraken_doa and every plugin (`libkrakendig.a`; a plugin with its own licence: pffft's objects) | BSD-style (FFTPACK), text A |
| moodycamel ConcurrentQueue | `include/concurrentqueue.h` | Simplified BSD or Boost Software License 1.0, text B |
| uPlot 1.6.30 | inlined at the top of `kraken_doa.html` | MIT, text C |
| opus-decoder (wasm-audio-decoders) incl. simple-yenc | `opus-decoder.min.js` | MIT, text D |
| libopus, compiled to WebAssembly inside `opus-decoder.min.js` | `opus-decoder.min.js` | BSD-3-Clause, text E |
| mbelib - the IMBE decoder ported to C++ and its tables | `plugins/lib/dig_vocoder.cpp`, `plugins/lib/mbe_tables.hpp` | ISC, text F |
| DSD - frame deinterleave tables | `plugins/lib/mbe_tables.hpp` | ISC, text F |
| NTIA/ITS Irregular Terrain Model (ITM) 1.3, ported to JavaScript | `kraken_doa.html` ("📶 ITM" block) | not subject to copyright in the US; NTIA terms, text G |
| ra-firmware (DF9DQ's "Ra" radiosonde receiver firmware): the RS41 and M10 / M20 sensor computations follow its metrology code | `plugins/radiosonde/rs41.cpp`, `plugins/radiosonde/modem.cpp` | BSD-3-Clause, text H |
| redsea (Oona Räisänen's RDS decoder, github.com/windytan/redsea, commit 7555c9f): the RDS / RBDS lookup tables (character set, PTY names, countries, languages, ODA names, RT+ types, call signs) follow its tables.cc / rdsstring.cc, `rbds_callsign()` its getCallsignFromPI(); two design ideas credited in place (block-sync conditions, biphase pairing) | `plugins/rds/rds_tables.hpp` (+ credits in `rds_block.hpp`, `rds_demod.hpp`) | MIT (the project), ISC (those files' headers), text I |
| minimp3 (lieff, github.com/lieff/minimp3, commit ea99364): the MPEG audio decoder that turns DAB's MPEG-1 / MPEG-2 Layer II frames into PCM, vendored unchanged | `plugins/dab/minimp3.h` - compiled into the dab plugin | CC0-1.0 (public domain dedication), text J |
| VisualOn vo-aacenc (`aacenc/src/aac_rom.c`): AAC spectrum code book 11, the scale-factor code book and the 48 kHz long-window scale-factor band offsets, values unchanged | `plugins/dab/tools/dab_gen_aac_tables.hpp` - only in the dab plugin's test-signal generator (`tools/dab_gen.cpp`; the Windows package carries it prebuilt), not in kraken_doa or the plugin | Apache-2.0, text K |

The decoder plugins are written from the published standards and protocol
descriptions; some protocol facts (frame layouts, constants, calibration
fits) were first published by other projects' reverse engineering and are
used as facts, without their code - except where a row above names the
project. Each plugin's README lists its sources.

## 1a. Parts of this folder under other licences

These are not under the KrakenSDR Source Available Licence (its section 6):

| Part | Where | Licence |
|---|---|---|
| The plugin SDK and the Windows process helpers (KrakenRF) - in every plugin | `plugins/sdk/`, `plugins/SDK.md`, `win/` (= `heimdall_v2/win/`) | MIT, `plugins/sdk/LICENSE` |
| ISM device decoder plugin (KrakenRF), following the device decoders of rtl_433 (github.com/merbanan/rtl_433, Copyright (C) 2012 Benjamin Larsson and the rtl_433 contributors - every file names the decoders it follows and their authors) | `plugins/ism/` | GPL-2.0-or-later, `plugins/ism/LICENSE`. A separate program: kraken_doa runs it as a process of its own; it is built only from its own files, the plugin SDK, pffft and liquid-dsp (`plugins/Makefile` refuses anything else for a plugin with its own `LICENSE`) |
| FLARM / OGN tracker decoder plugin (KrakenRF), with the OGN protocol's LDPC(208,160) parity-check matrix from the OGN project's glidernet/diy-tracker (`ldpc.cpp`, `LDPC_ParityCheck_n208k160`, by Pawel Jalocha and the OGN project - https://github.com/glidernet/diy-tracker) | `plugins/flarm/` (the matrix: `ogn_ldpc_matrix.hpp`, GPL-3.0-only) | GPL-3.0, `plugins/flarm/LICENSE` (KrakenRF's files GPL-3.0-or-later). A separate program, built like `plugins/ism` |

## 2. Libraries built with or linked into the programs

| Component | Version | Used by | Licence | Source |
|---|---|---|---|---|
| uWebSockets + uSockets | commit fe7da4cb (`UWS_COMMIT`) | kraken_doa | Apache-2.0 | https://github.com/uNetworking/uWebSockets |
| uWebSockets `MoveOnlyFunction.h` | (part of uWebSockets) | kraken_doa | MIT - Copyright (c) 2020 Oleg Fatkhiev | as above |
| liquid-dsp | v1.7.0 (4dda702, `liquid.mk`), built without FFTW / libfec | kraken_doa, plugins | MIT - Copyright (c) 2007 - 2016 Joseph Gaeddert | https://github.com/jgaeddert/liquid-dsp |
| Eigen | the system's (Debian 3.4, MSYS2 5.x), headers only, unmodified | kraken_doa | MPL-2.0 | https://gitlab.com/libeigen/eigen |
| OpenSSL | 3.x | kraken_doa | Apache-2.0 | https://www.openssl.org/source/ |
| Opus (libopus) | the system's | kraken_doa | BSD-3-Clause (text E) | https://opus-codec.org/ |
| libjpeg-turbo | the system's (Debian/MSYS2; linked only when present, else the plugins send raw video) | decoder plugins (compresses `host.video_frame` pictures to JPEG; the main app does not link it) | IJG / BSD-3-Clause / zlib - Copyright (c) 2009-2024 D. R. Commander and contributors | https://libjpeg-turbo.org/ |
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
- **DAB+ audio**: Fraunhofer FDK AAC (libfdk-aac, the FDK AAC licence - not
  the GPL), installed by you from your system's packages (Debian `non-free`,
  Ubuntu multiverse, MSYS2, Homebrew); the dab plugin loads it at run time
  and never contains or ships it. AAC / HE-AAC may be covered by patents in
  some countries.
- **AI assistant**: Claude Code (Anthropic's terms), Python with numpy / scipy
  (BSD-3-Clause), bubblewrap, socat, curl.

## 4. Online services and map data

The web UI and some features fetch data from online services, directly from
your browser or from the receiver. Their terms apply to that use, and some
restrict heavy or commercial use - check them before using the software in a
commercial product or service.

- **OpenStreetMap** map tiles (tile.openstreetmap.org), the Overpass API
  (masts layer) and Nominatim (incident address search, the map's search
  box - one request a second, sent on Enter only): data © OpenStreetMap
  contributors, available under the Open Database License (ODbL) -
  https://www.openstreetmap.org/copyright. Usage policies:
  https://operations.osmfoundation.org/policies/
- **Esri World Imagery** (satellite layer): © Esri and its data providers;
  Esri's terms of use apply.
- **Optional map providers** (⚙ Settings, only with the user's own API
  key): Mapbox, Google Maps Platform (Map Tiles API), Azure Maps, MapTiler,
  Thunderforest, Stadia Maps, HERE, or any tile server the user enters. The
  browser fetches their tiles under the user's own account and terms; the
  map shows each one's attribution (Google's and Azure's as their APIs send
  it for the area shown).
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

### I. redsea

The project's licence (LICENSE, MIT):

```
Copyright (c) 2007-2016 Oona Räisänen OH2EIQ

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

The header of src/tables.cc and src/text/rdsstring.cc (ISC):

```
Copyright (c) Oona Räisänen

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

### J. minimp3

The dedication at the top of `plugins/dab/minimp3.h`
(https://github.com/lieff/minimp3); the full text of CC0 1.0 is at
https://creativecommons.org/publicdomain/zero/1.0/legalcode:

```
https://github.com/lieff/minimp3
To the extent possible under law, the author(s) have dedicated all copyright and related and neighboring rights to this software to the public domain worldwide.
This software is distributed without any warranty.
See <http://creativecommons.org/publicdomain/zero/1.0/>.
```

### K. VisualOn vo-aacenc (Apache License 2.0)

The notice of `plugins/dab/tools/dab_gen_aac_tables.hpp` (the tables of
vo-aacenc's `aacenc/src/aac_rom.c`, Copyright 2003-2010, VisualOn, Inc.),
then the licence:

```
AAC tables taken from VisualOn vo-aacenc (aacenc/src/aac_rom.c),
Copyright 2003-2010, VisualOn, Inc., licensed under the Apache License,
Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0). Extracted by
make_aac_tables.py; values unchanged, layout reformatted. Distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
```

```
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright [yyyy] [name of copyright owner]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
```
