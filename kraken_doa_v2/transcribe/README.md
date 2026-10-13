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
| `transcribe/setup.sh --models-only M..` | only download models |
| `transcribe/setup.sh --build-only` | only build |
| `transcribe/setup.sh --update` | rebuild if set up already (kraken_doa's `make` runs it) |
| `transcribe/setup.sh --list` | the models that can be downloaded |

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

prints the speech found, the time taken, the confidence and the text (or why
nothing was kept). Any 16-bit WAV works (other rates are resampled crudely);
the clips kraken_doa keeps (`voice_clips/<id>.wav`) are exactly what it sent.

## What the helper does with a transmission

1. Silero VAD finds the speech; only those parts (each padded 200 ms,
   joined by 150 ms of silence) go on. Less than 200 ms of speech = "no
   speech": whisper doesn't run (noise, carriers, data bursts, tones).
2. whisper (greedy decoding, each transmission on its own; the vocabulary
   prompt as "Glossary: ... ." - a bare word list made whisper stop after
   one word; a prompted result far shorter than the speech is decoded again
   without it).
3. Clean-up: non-speech annotations (`[BLANK_AUDIO]`, `(static)`, ♪) are
   removed, a phrase repeated 4+ times is kept once, subtitle credits
   ("thanks for watching") and lone fillers ("thank you", "you") that
   whisper isn't sure of are dropped.

The protocol with kraken_doa (`include/voice2text_wire.hpp`) uses the
decoder plugins' framing: one job at a time, int16 16 kHz audio in, text +
confidence + timing out.

## Licences

whisper.cpp: MIT (cloned at build time, not part of this repository). The
Whisper models: MIT (OpenAI), converted by the whisper.cpp project and
downloaded from Hugging Face. Silero VAD: MIT.
