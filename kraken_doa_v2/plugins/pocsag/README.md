# POCSAG (example plugin)

Decodes POCSAG pager transmissions (ITU-R M.584): 2-FSK, +-4.5 kHz deviation,
512, 1200 and 2400 bit/s - all three are demodulated in parallel, so the rate
needs no setting.

- Sync codeword 0x7CD215D8, either polarity (inverted transmitters / receivers)
- BCH(31,21) correction of up to 2 bit errors per codeword; the even-parity
  bit rejects 3-error words the BCH decoder would "correct" into a wrong one
- Address codewords give the 21-bit RIC (capcode) and the function bits;
  messages are shown as numeric (BCD) or alphanumeric (7-bit ASCII) text -
  function 0 is usually numeric and 3 alphanumeric, with a fallback to
  whichever decodes as clean text
- A message with a lost codeword is marked `[incomplete]` / `(message lost)`

Facts: bit rate, polarity, deviation, last RIC, last message, message count.
Every alphanumeric page also goes to kraken_doa as a text message
(`host.message`): street addresses in it become incidents (the decoder
tab's 📍 Incidents table, the 🗺 Map with Plot on map).
Events: one line per message (`1200 bit/s RIC 1234567 F3 alpha: ...`).
Raw frames (decoder data log, "Raw frames" ticked): `RIC 1234567 F3 1200
63987 464D9 ...` - RIC, function, bit rate and the 20 data bits of each
message codeword in hex.

Use a VFO of 12.5 kHz or more on the paging channel.

Tested on synthetic transmissions (all three rates, 3-15 dB SNR, inverted,
700 Hz carrier offset): every message decodes down to ~5 dB; below that
messages are cut short rather than garbled. Decode time ~1% of real time on
a Raspberry Pi 5.

Real recordings (2026-10-16; verified against them, not in this repo -
multimon-ng built in scratch as the reference, GPL):
- sigidwiki "POCSAG" IQ (128 kS/s, a transmitter sending 512 and 1200
  bit/s, inverted): 6 pages - "THIS IS A TEST PERIODIC PAGE ..." at 512 and
  1200, two dispatch pages ("Unit: WEST2 Job # 1046-1-2015/0 ...") and two
  scrambled-looking alpha pages - text identical to multimon-ng's.
  multimon-ng also printed a message-less 512 bit/s "RIC 1860544": its
  512 decoder read past the transmission's last batch into the 1200 bit/s
  preamble (the word where the next sync would be is 1C71C71C, 13 bits off
  it) - nothing was sent there; the plugin stops at the missing sync.
- multimon-ng's own test samples (FM discriminator audio, FM-modulated to
  IQ at 38.4 kHz): 512 bit/s "512 B SIDE ZZZZZZ" (RIC 273040), 1200
  bit/s "+++TIME=0008300324..." (273040), 2400 bit/s (1022869 F1) - all
  as multimon-ng's test expects; its extra message-less 1200 bit/s page
  (671968) is again past the last batch (next word FFFFFFFE, no sync).
- The generator (`tools/pocsag_gen.py`, the playback's six pages) through
  multimon-ng: 6 / 6 RICs, functions and texts identical, with multimon-ng
  set to NORMAL polarity (inverted: none) - "1 = the lower frequency" as
  M.584. The real recording above is inverted (its receiver / transmitter).
- `POCSAG_CW=1` (offline) prints every codeword and the word checked for
  the next sync.

## Test transmission (debug playback)

`tools/pocsag_gen.py`: real framing - 576-bit preamble, batches of sync +
8 frames x 2 codewords, BCH(31,21) + parity, the address in its RIC's
frame, numeric / alphanumeric messages, idle fill; Gaussian-filtered NRZ
2-FSK, 4.5 kHz deviation; `--fm` writes an analog FM interferer. Playback
`pocsag-three-rates` (157.95 MHz, 16 s): one paging transmitter at 60° -
a numeric (F0) and an alphanumeric (F3) page at each of 512, 1200 and 2400
bit/s - and an FM transmitter at 200° in the gaps. POCSAG names no
transmitter, so the entry checks the six pages and the VFO's own DoA with
the Digital squelch: 60.0° throughout (with the squelch off it swings
17.7-352.7° between the pages). Offline the generator's file gives the six
pages, 6 valid batches, nothing else.

## ID separation
Not applicable: the RIC is the pager being called (the recipient), not the
transmitter, and paging networks send the same batch from several
transmitters at once (simulcast) - nothing in the signal tells them apart.
The VFO's DoA (with the Digital squelch: from the decoded batches only) is
the paging site's.
