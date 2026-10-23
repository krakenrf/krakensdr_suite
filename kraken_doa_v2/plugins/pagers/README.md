# Pagers: POCSAG + FLEX (example plugin)

Decodes pager transmissions of both widespread paging protocols at once -
POCSAG and Motorola FLEX - on one VFO. One FM front end (8 kHz low-pass,
discriminator at 38.4 kHz) feeds a POCSAG receiver per bit rate and the FLEX
receiver side by side, so neither the protocol nor the speed needs a
setting. The plugin was `pocsag` until 2026-10-23: saved VFOs, options and
the Auto detect list naming `pocsag` load as `pagers` (`dig::renamed_plugin`).

Use a VFO of 12.5 kHz or more on the paging channel (POCSAG channels are
12.5 / 25 kHz, FLEX 25 kHz; every FLEX speed decoded through a 12.5 kHz
VFO filter in the tests below).

Events (one line per page, in the decoder tab and the decoder log):

```
POCSAG 1200 bit/s RIC 0012345 F3 alpha: TEXT
POCSAG 512 bit/s RIC 1234567 F0 numeric: 0211234567
FLEX 1600/2 capcode 1234567 alpha: TEXT
FLEX 3200/4 capcode 0000346 numeric: 0211234567
FLEX 3200/2 capcode 1100003 tone only (source 3)
FLEX 6400/4 capcode 1000016 binary: DEADBEEF0123
FLEX 6400/4 capcode 2029570 (group) (to 2 pagers: 1000008 1000012) alpha: TEXT
FLEX 6400/4 group call set up: capcode 2029570 (group) for 2 pagers in frame 44: 1000008 1000012
```

A page with a lost or failed codeword ends in `[incomplete]` (FLEX: a lost
word shows as `…`). Every alphanumeric page also goes to kraken_doa as a
text message (`host.message`): street addresses in it become incidents (the
decoder tab's 📍 Incidents table, the 🗺 Map with Plot on map).

Facts: `POCSAG` (last bit rate, polarity, deviation), `FLEX` (speed, cycle
/ frame, polarity, deviation), `FLEX time` (when the system sends it), last
address, last message, message counts per protocol.

Raw frames (decoder data log, "Raw frames" ticked):
`POCSAG RIC 1234567 F3 1200 63987 464D9 ...` - RIC, function, bit rate and
the 20 data bits of each message codeword in hex;
`FLEX 6400/4 7.44 A capcode 1234567 V5 0C02D4 1AA0F1 ...` - speed, cycle.frame,
phase, address, vector type, the vector word and the message words (21
information bits in hex, `------` = lost).

## POCSAG

ITU-R M.584: 2-FSK, +-4.5 kHz deviation, 512, 1200 and 2400 bit/s - all
three demodulated in parallel (`pocsag.hpp`).

- Sync codeword 0x7CD215D8, either polarity (inverted transmitters / receivers)
- BCH(31,21) correction of up to 2 bit errors per codeword; the even-parity
  bit rejects 3-error words the BCH decoder would "correct" into a wrong one
  (`bch.hpp`, shared with FLEX)
- Address codewords give the 21-bit RIC (capcode) and the function bits;
  messages are shown as numeric (BCD) or alphanumeric (7-bit ASCII) text -
  function 0 is usually numeric and 3 alphanumeric, with a fallback to
  whichever decodes as clean text
- A message with a lost codeword is marked `[incomplete]` / `(message lost)`

Tested on synthetic transmissions (all three rates, 3-15 dB SNR, inverted,
700 Hz carrier offset): every message decodes down to ~5 dB; below that
messages are cut short rather than garbled. Decode time ~1% of real time on
a Raspberry Pi 5. Since the merge into this plugin its pages are byte for
byte those of the `pocsag` plugin (A/B at 4 - 20 dB, the "POCSAG " prefix
aside). On pure noise it reports a stray `(message lost)` every few minutes
(a false 32-bit sync, then garbage) - as before the merge.

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

## FLEX

Motorola FLEX (`flex.hpp` / `flex.cpp`): 1600 bit/s 2-level, 3200 bit/s
2-level, 3200 bit/s 4-level (1600 symbols/s) and 6400 bit/s 4-level (3200
symbols/s), +-4.8 kHz (4-level inner +-1.6 kHz); each frame says its own
speed, so a channel may change it frame by frame.

- A frame (1.875 s, 128 a cycle of 4 minutes, cycles 0-14): sync 1 at 1600
  bit/s 2-level - 32 bits 1010, the 32-bit A word naming the speed, 16 bits
  B, A inverted - and the frame information word (cycle, frame; BCH + a
  4-bit check), sync 2 at the frame's speed (bit sync, C, both inverted, 25
  ms), then 11 blocks of 160 ms. Per phase (1600: A; 3200: A + C; 6400: A
  + B + C + D) 8 codewords a block, bit-interleaved; 3200/2 alternates the
  phases symbol by symbol, a 4-level symbol carries two (its sign one, inner
  / outer the other), 6400/4 both
- Codeword = POCSAG's BCH(31,21) + even parity with every field reversed:
  21 information bits LSB first, the check bits, the parity bit
- Polarity: binary 1 = the LOWER frequency (as POCSAG; a real 6400/4
  transmitter); sync 1 is looked for in both, so an inverted receiver chain
  is read too (fact "inverted")
- A phase: block information word (where the address and vector fields
  start, priority addresses; up to 3 more words with the date / time / time
  zone - fact `FLEX time`, the local time at frame 0 of the cycle), the
  addresses - short (1 word, capcode = word - 32768, incl. the info service
  / network / temporary / operator ranges) and long (2 words: sets 1-2, 1-3,
  1-4, 2-3 with the data sheet's conversions; 2-4 has none - shown as its
  two words) -, one vector word per address word, the message words
- Messages: alphanumeric (7-bit ASCII, ETX / NUL fill), numeric (standard,
  special format, numbered - 4-bit BCD `0-9 * U space - ] [`), short
  message (3 digits in the vector, 8 with a long address), tone only (its
  source), hex / binary (shown as hex; 7 / 8-bit characters as text),
  secure (shown as its text), short instruction = a group call: pagers told
  to listen to a temporary address in a frame - announced once, and the
  message to that address names them
- Checks: the 4-bit check of every frame information / block information /
  vector word, the 10-bit fragment check (alphanumeric, secure), 12-bit
  (binary), 6-bit numeric check, the alphanumeric signature over the whole
  message - a failed one marks the page `[incomplete]`
- Fragments: long messages come in fragments over several frames (fragment
  numbers 3, 0, 1, 2 ..., continued flag), put together per address +
  message number and shown once complete; a missing fragment shows as `…`,
  one that never ends is shown after 4.5 minutes
- Valid frames (`Host::valid`, the Digital squelch): every block with at
  most one lost codeword per phase - block 0 from the start of sync 1. A
  FLEX channel transmits frames back to back, so the squelch stays open
  while it is on the air

Receiver design (each point from a test that failed without it):

- Sync 1 is looked for every quarter bit straight on the smoothed
  discriminator - the 80 bits sliced against their own mean, matched on A,
  B and the first half of A inverted (<= 5 of those 64 bits off). A clock
  loop running in front lost the first frame after a key-up (noise before
  it) and frames after a quiet frame (FLEX idle words are all 0: one
  frequency for most of the frame - a running centre drifted onto it)
- Frames follow each other with no gap: after a frame, the next sync 1 is
  checked exactly where this frame's grid says it begins (+-0.5 bit), so a
  sync the hunt alone would miss after a 3200 symbols/s frame is caught
- The data are sampled on the symbol grid sync 1 gives (one transmitter
  clock for the whole frame), whole symbols fixed by sync 2's C words. The
  fraction is followed block by block by a timing detector on the level
  changes only (at each change the sample half way between two symbols
  should be the levels' midpoint) - idle words carry no timing, and a loop
  following every symbol, or the eye opening of a mostly idle block, let
  the timing wander up to half a symbol: whole blocks lost after a fade
- A block with lost codewords is also tried at nearby fractions; a fraction
  is only followed from a block that decoded
- 4-level levels from sync 2's bit syncs, then decision-directed
- Addresses have no check of their own, and 4 bit errors turn a codeword
  into another one undetected (parity even again): the words that place a
  page (block information, addresses, vectors) are taken with fewer
  corrections the noisier their block is (estimated bit errors 4+: at most
  1 corrected bit, 12+: none) - at 2 dB a real page landed on a capcode
  that wasn't called before. Message words keep both corrections (their
  own checks catch a wrong one)
- A word that needed no correction is taken without its 4-bit check
  (multimon-ng's gen-ng leaves the block information word's at 0)

Sources (no code of any other decoder): the frame and message formats, the
address ranges, capcode conversions and check definitions of Motorola's
MC68175 FLEX decoder data sheet (appendix A / B); the sync 1 / sync 2
patterns of the FLEX patent's drawing as published by Jelmer Bruijn
("Motorola FLEX / P2000 decoding: sync and header"); the bit order, symbol
mapping, polarity and the checks confirmed on a real recording (below).

Tests (job scratch, 2026-10-23):

- A real recording - sigidwiki "FLEX_Pager_IQ_20150816_929613kHz_IQ"
  (929.613 MHz, US hospital / dispatch traffic; not in this repo, its
  content isn't quoted here): 6400/4, frames 39-53 of cycle 7 - 13 syncs
  (the other two frames are too weak for any), 101 valid blocks, 39 pages
  incl. long addresses (set 1-2), numeric pages with their 6-bit check,
  two-fragment messages put together with their signature right, short
  messages; 3 marked `[incomplete]` (fades). Every check definition above
  holds on it (a corrupted fragment failed its 10-bit check)
- The generator (`tools/flex_gen.py`, written from the same sources)
  through multimon-ng (GPL, built in scratch as an outside reference only):
  1600/2, 3200/2, 3200/4, 6400/4 identified, short-address alphanumeric,
  numeric and tone pages identical, in the same phases (multimon-ng names
  the 3200/4 second phase B; the data sheet's figure C - the letter only
  shows in raw frames). multimon-ng gives other capcodes for long
  addresses (and drops a long-address alphanumeric page's first two
  characters); the data sheet's conversion and message layout are the ones
  the real recording's pages check out with
- multimon-ng's own encoder (gen-ng) through this plugin: its alphanumeric
  pages and its group call (set up, then the message to the temporary
  address naming the 3 pagers) exact, either polarity. gen-ng's `-e 1/2`
  test frames aren't read: it corrupts a block information word whose
  4-bit check it never set
- Noise (CNR in 25 kHz, 6 frames x 6 pages per speed, generated): every
  page exact down to 6 dB at all four speeds; at 4 dB 1600/2 36 / 36,
  3200/2 35, 3200/4 32, 6400/4 29; at 2 dB 1600/2 still 36 / 36. No page to
  a wrong capcode or with wrong text at any level (the rest marked
  `[incomplete]` or dropped)
- Every speed exact with a +-1.5 kHz offset, inverted, a sample clock
  +-100 / +300 ppm off, through 12.5 / 16 / 25 kHz VFO filters, clean and at
  16 dB
- 10 minutes of noise: no FLEX event
- kraken_doa's decoder engine (`tools/digi_test.cpp`): `PLUGIN:pagers`,
  the old `PLUGIN:pocsag` (loads as pagers) and Auto detect - every page of
  the playback scene, 66 / 66 valid blocks, its AFC taking the carrier
  offset FLEX reports
- `FLEX_CW=1` (offline) prints every sync found and every codeword (raw,
  corrected, the timing fraction)

## Test transmissions (debug playback)

`tools/pocsag_gen.py`: real framing - 576-bit preamble, batches of sync +
8 frames x 2 codewords, BCH(31,21) + parity, the address in its RIC's
frame, numeric / alphanumeric messages, idle fill; Gaussian-filtered NRZ
2-FSK, 4.5 kHz deviation; `--fm` writes an analog FM interferer. Playback
`pocsag-three-rates` (157.95 MHz, 16 s): one paging transmitter at 60° -
a numeric (F0) and an alphanumeric (F3) page at each of 512, 1200 and 2400
bit/s - and an FM transmitter at 200° in the gaps. The entry checks the six
pages and the VFO's own DoA with the Digital squelch: 60.0° throughout
(with the squelch off it swings 17.7-352.7° between the pages).

`tools/flex_gen.py`: FLEX frames back to back at any of the four speeds -
short and long addresses, alphanumeric (`|` = fragments over the next
frames), numeric, tone only, short message, binary, secure, group calls,
the date / time block information words. Playback `flex-four-speeds`
(929.6625 MHz, 12 s): one transmitter at 120° sending six frames -
1600/2, 3200/2, 3200/4 and three at 6400/4 - with every kind of page; the
entry checks six of them and the VFO's DoA with the Digital squelch.

## ID separation
Not applicable: the RIC / capcode is the pager being called (the
recipient), not the transmitter, and paging networks send the same batch /
frame from several transmitters at once (simulcast) - nothing in the
signal tells them apart. The VFO's DoA (with the Digital squelch: from the
decoded batches / blocks only) is the paging site's.
