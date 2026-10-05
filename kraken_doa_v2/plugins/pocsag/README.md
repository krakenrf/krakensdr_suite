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
Events: one line per message (`1200 bit/s RIC 1234567 F3 alpha: ...`).

Use a VFO of 12.5 kHz or more on the paging channel.

Tested on synthetic transmissions (all three rates, 3-15 dB SNR, inverted,
700 Hz carrier offset): every message decodes down to ~5 dB; below that
messages are cut short rather than garbled. Decode time ~1% of real time on
a Raspberry Pi 5.
