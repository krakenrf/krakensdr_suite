# Voice to Text helper (`kraken_whisper`)

kraken_doa's **🗣 Voice to Text** writes down what is said on the VFOs whose
"🗣 Transcribe" box is ticked. The speech-to-text work runs in this folder's
helper, a separate process built on [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
(MIT licence) with OpenAI's Whisper models - free, local, nothing is sent
anywhere. kraken_doa starts it while a VFO transcribes (at nice 10, below the
receiver) and stops it 2 minutes after the last one is unticked.

## Set up

```
cd kraken_doa_v2
make whisper               # = transcribe/setup.sh: clone + build + base.en + the VAD model
```

| Command | Does |
|---|---|
| `transcribe/setup.sh` | clone whisper.cpp at the pinned commit, build `build/kraken_whisper` (JOBS, max 3), download base.en (if no model yet) and the Silero VAD model |
| `transcribe/setup.sh small.en tiny.en` | the same + these models |
| `transcribe/setup.sh base.en-q8_0` | base.en with 8-bit weights, made here from base.en (below; `-q4_0` = 4-bit) |
| `transcribe/setup.sh --models-only M..` | only download models |
| `transcribe/setup.sh --build-only` | only build |
| `transcribe/setup.sh --update` | rebuild if set up already (kraken_doa's `make` runs it) |
| `transcribe/setup.sh --list` | the models that can be downloaded |

**Windows**: in MSYS2's UCRT64 shell the same commands build
`build/kraken_whisper.exe` (for the CPU level of `WIN_MARCH`, default
x86-64-v3 like the apps); `set WHISPER=1` before `build_windows.bat` runs it
as part of the build. Models are downloaded with `curl.exe` (Windows 10 1803+
has one).

**8-bit models** (`base.en-q8_0` 78 MB, `small.en-q8_0` 252 MB): 16-27 % less
CPU than base.en / small.en, about as accurate (measured on a PC; 5-bit
q5_1 was slower there and a little less accurate). whisper.cpp publishes no
English-only 8-bit files, so they are made on this machine from the f16
model by the helper itself (`build/kraken_whisper --quantize IN OUT q8_0`,
a second or two on a PC): setup.sh (or the page's "Get") uses an installed
base.en / small.en, else downloads it for this and removes it again
afterwards unless it was asked for too. `--models-only` needs the helper
built already.

**On a Raspberry Pi** (measured on a Pi 4 and a Pi 5, 2026-11-20): use
**`base.en-q8_0`** - half the CPU of base.en, as accurate (CPU-s per
transcript, 2 threads: Pi 4 6.2 against 12.8, Pi 5 0.92 against 1.84; the
old helper took 39.8 / 6.23). On a Pi 5 **`small.en-q8_0`** (2.7 CPU-s, 5x
faster than real time) is the accurate choice. **4-bit models**
(`base.en-q4_0` 44 MB, `small.en-q4_0` 139 MB, made the same way with
`--quantize ... q4_0`) take the least memory (base.en-q4_0 123 MB against
157 MB for q8_0 on a Pi 4) but more CPU than q8_0 on both Pis. On 64-bit
Arm Linux whose CPU has the dot-product instructions (a Pi 5, not a Pi 4),
setup.sh builds the helper with Arm's **KleidiAI** kernels (fetched by CMake
from GitHub; offline it builds without them; `KLEIDIAI=on|off` overrides
the detection; an existing build gets them with `make whisper`): on a Pi 5
15-21 % less CPU for the q8_0 models, nothing for f16, 4 % more for q4_0.
Live text: fine on a Pi 5 with base.en-q8_0 (half to three quarters of a
core while someone talks), too heavy for a Pi 4 (3 of its 4 cores).

Files: `whisper.cpp/` (the pinned checkout), `build/` and `models/`
(`ggml-<name>.bin`) - all gitignored. `KRAKEN_TRANSCRIBE_DIR` /
`KRAKEN_WHISPER_MODELS` move them (Docker: the helper is in the image, the
models in the data volume).

whisper.cpp is pinned (`WHISPER_TAG` / `WHISPER_COMMIT` in `setup.sh`): a
newer one may change the C API this helper uses. To move to another
version, change both, run `setup.sh --build-only` and test as below.

## Offline test

```
build/kraken_whisper --file clip.wav --model models/ggml-base.en.bin \
    --vad models/ggml-silero-v6.2.0.bin [--threads 3] [--prompt "Roger, over"] [--no-vad]
```

prints the speech found, the time taken (and the CPU time), the encoder
windows used ("1 sized", or "1 sized + 1 full (loop guard 1)"), the
confidence and the text (or why nothing was kept). Any 16-bit WAV works
(other rates are resampled crudely). More modes:

| Option | Does |
|---|---|
| `--partial` | one Live text decode of the whole file (what a PARTIAL job does) |
| `--stream 1000` | Live text as kraken_doa runs it: a partial decode of the audio so far every second (as if the file arrived in real time), each line `time, decode time, [confirmed] pending`; then the final transcript and a summary (decode times, CPU cores used while the audio lasted, words confirmed early and how many the final changed) |
| `--quantize IN.bin OUT.bin q8_0` | (no `--model`) f16 / f32 model -> 8-bit (`q4_0`, `q5_0`, `q5_1` too); OUT is written exactly as named (callers use a `.part` name and rename), exit 0 = done, else a message on stderr and no OUT |

The clips kraken_doa keeps (`<decoder log folder>/voice_clips/YYYY-MM-DD/<id>.opus`,
Ogg Opus) are what it sent, compressed: `ffmpeg -i <id>.opus -ar 16000 -ac 1 clip.wav`
turns one back into a WAV for this.

## What the helper does with a transmission

1. Silero VAD finds the speech; only those parts (each padded 200 ms,
   joined by 150 ms of silence) go on. Less than 200 ms of speech = "no
   speech": whisper doesn't run (noise, carriers, data bursts, tones).
2. whisper (greedy decoding, each transmission on its own; the vocabulary
   prompt as "Glossary: ... ." - a bare word list made whisper stop after
   one word; a prompted result far shorter than the speech is decoded again
   without it). Whisper sometimes stops after the first sentence ("Roger the
   register." of 11 s of speech - timestamps don't make it go on): a result
   with fewer than 0.6 words per second of speech (>= 4 s) is decoded again
   as the speech's two halves, split between speech parts near the middle,
   and the longer text wins. Each decode encodes only the speech + 1 s,
   rounded up to 5.1 / 10.2 / 15.4 / 20.5 / 25.6 s, not whisper's fixed 30 s
   (3x less CPU for 5-13 s transmissions, as accurate on the test clips),
   with a cap of 6 tokens per second (multilingual models: 10) + 16 (at most
   219, below whisper's own limit): a decode that reaches
   it (whisper looping) is done again with the full 30 s window and no cap -
   the decode of before -, so the cap never cuts a transcript short.
3. Clean-up: non-speech annotations (`[BLANK_AUDIO]`, `(static)`, ♪) are
   removed, a phrase repeated 4+ times is kept once, subtitle credits
   ("thanks for watching") and lone fillers ("thank you", "you") that
   whisper isn't sure of are dropped.

**Live text** (a PARTIAL job: the transmission so far, sent about once a
second while someone talks, when Live text is on in kraken_doa): the same
VAD, prompt, language and clean-up, but decoded once - greedy, no
temperature fallback, no retries, and the token cap just ends it (on
half-spoken audio whisper otherwise looped or invented words: 7-10 s
decodes; 0.2-0.5 s with these guards); a phrase looped up to the cap is
kept once. kraken_doa shows the words two partials in a row agree on; the
final transcript at the end replaces them. It costs ~4x the CPU of
transcribing at the end (base.en-q8_0, 2 threads: ~0.45 of a PC core while
someone talks; a Pi 5, estimated: ~1.5-2 of its 4 cores) - an option, off by
default.

The protocol with kraken_doa (`include/voice2text_wire.hpp`) uses the
decoder plugins' framing: one job at a time, int16 16 kHz audio in, text +
confidence + timing out (JOB -> RESULT, PARTIAL -> PARTIAL_RESULT; READY
says `live=1` when the helper takes PARTIAL).

## Licences

whisper.cpp: MIT (cloned at build time, not part of this repository);
`quantize.cpp` is ported from its quantize example (MIT, notice in the
file). The Whisper models: MIT (OpenAI), converted by the whisper.cpp
project and downloaded from Hugging Face. Silero VAD: MIT.
