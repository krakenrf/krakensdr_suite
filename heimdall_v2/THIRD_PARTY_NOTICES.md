# Third-party notices - Heimdall receiver server (heimdall_v2)

Heimdall is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option)
any later version (`LICENSE` in this folder). Copyright (c) 2026 KrakenRF
Inc.

It contains or links the components below, which keep their own licences.
Every one of them is compatible with GPL-3.0-or-later, so heimdall as a
whole - including a statically linked `heimdall` / `heimdall.exe` - is
distributed under GPL-3.0-or-later. (Not GPL-2.0-only: uWebSockets'
Apache-2.0 licence is incompatible with version 2.) Binary packages of the
suite carry the complete licence texts in their `licenses/` folder and say
where the corresponding source is (`SOURCE.txt`).

## In this folder

| Component | Where | Licence |
|---|---|---|
| pffft (marton78 fork, commit aa16fd3) | `external/pffft/` | BSD-style (FFTPACK) - `external/pffft/LICENSE.txt` |
| moodycamel ConcurrentQueue - Copyright (c) 2013-2020 Cameron Desrochers | `external/concurrentqueue/` | Simplified BSD or Boost Software License 1.0 - `external/concurrentqueue/LICENSE.md` |
| Jeff Preshing's semaphore - Copyright (c) 2015 Jeff Preshing | `external/concurrentqueue/lightweightsemaphore.h` | zlib (in the file) |
| uPlot 1.6.30 - Copyright (c) 2022 Leon Sorokin | inlined at the top of `index.html` | MIT (in the file) |
| heimdall_daq_fw - Copyright (C) 2018-2022 Tamás Pető, Carl Laufer | the Wideband board's GPIO switch patterns in `src/sdr/sdr_init.cpp` (ported from its `rtl_daq.c`) | GPL-3.0-or-later |

The RTL-TCP server (`src/net/rtl_tcp_server.cpp`) is compatible with
osmocom's `rtl_tcp` (GPL-2.0-or-later, Steve Markgraf and others) and follows
its protocol and buffering design.

## Linked libraries

| Component | Version | Licence | Source |
|---|---|---|---|
| KrakenSDR librtlsdr fork (static) - Copyright Steve Markgraf, Dimitri Stolnikov, Hoernchen, Kyle Keen and others | krakenrf/librtlsdr 578b43d or later | GPL-2.0-or-later | https://github.com/krakenrf/librtlsdr |
| libusb-1.0 (Linux: dynamic; Windows: static) | the system's / MSYS2's | LGPL-2.1-or-later | https://github.com/libusb/libusb |
| uWebSockets + uSockets | commit fe7da4cb (`UWS_COMMIT`) | Apache-2.0 | https://github.com/uNetworking/uWebSockets |
| uWebSockets `MoveOnlyFunction.h` - Copyright (c) 2020 Oleg Fatkhiev | (part of uWebSockets) | MIT | as above |
| libuv (Windows only) | MSYS2's | MIT | https://github.com/libuv/libuv |
| Eigen (headers, unmodified) | the system's | MPL-2.0 | https://gitlab.com/libeigen/eigen |
| OpenSSL, zlib (linked by the Makefile) | the system's | Apache-2.0, zlib | https://www.openssl.org/, https://zlib.net/ |
| MinGW-w64 runtime + winpthreads (Windows only) | MSYS2's | ZPL-2.1 / MIT / BSD | https://www.mingw-w64.org/ |
| GCC runtime (libstdc++, libgcc) | | GPL-3.0 with the GCC Runtime Library Exception | https://gcc.gnu.org/ |
| GNU C library (Linux, dynamic) | the system's | LGPL-2.1-or-later | https://www.gnu.org/software/libc/ |
