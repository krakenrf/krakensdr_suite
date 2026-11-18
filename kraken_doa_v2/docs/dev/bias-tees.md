# Antenna bias tees from the page

Added 2026-11-18 at the user's request: "Add buttons to turn the bias tees
on/off in the krakensdr doa program. It should be on the top bar, to the
right of the coherent/wideband etc mode selector." Files: `src/antenna_bias.cpp`,
`include/antenna_bias.hpp`, the dispatcher hook in `src/control_handler.cpp`,
`system_status` in `src/message_builders.cpp`, the control-socket drain in
`src/networking/data_receiver.cpp`, the page's top bar (`#tb-bias`) and its
"BIAS TEE" script block; on heimdall's side `set_antenna_bias_tees` and three
status fields (`heimdall_v2/src/net/tcp_control_server.cpp`,
`request_antenna_bias_tees()` in `heimdall_v2/src/sdr/sdr_init.cpp`; the
protocol: `gr-krakensdr_v2/docs/heimdall_v2_control_protocol.md` §2.17).

"Bias tee" here means the per-port **antenna** bias tees: DC on the antenna
ports for active antennas and LNAs (GPIO N+1 of the channel-0 chip powers
port N). Not the calibration noise source, which older code and the control
protocol also call "bias tee" (`bias_tee_enabled`, `BIAS_TEE_ENABLE`); new
names say "antenna bias tee".

## What the page shows

- **The button** (`#bias-btn`, in its own top-bar group right of Mode): the
  `biastee` icon only (a T whose stem is a bolt, on a coax line), 30 x 28 px,
  on the controls' line (its bottom with the Mode select's; no caption above
  it). Amber (`--warn-*`: attention) while any bias tee is on, with the count
  as a badge on its corner ("2/5": on / open antennas) - a badge, so the
  button never gets wider. Its `title` names them ("Bias tee on: Antenna 0, 2
  (DC on those ports)", or "... - all off"). The count, the colour and the
  title are heimdall's state only, never this page's request.
- **The popover** (`#bias-pop`, the View popover's look and helpers:
  `popDismiss` / `popPlace`, under the button with the left edges aligned):
  "Bias tee", the warning line "DC on the antenna port - only for antennas /
  LNAs that take it" (a safety notice - the one sentence allowed on a
  control), **All** (on when every switchable antenna is on; switching it
  sets them all), then one switch per OPEN antenna, "Antenna 0" ... - the
  wording of the spectrum header's antenna selector. A switch acts at once.
  A channel that can't be switched is shown disabled with the reason in its
  title: Antenna 0 under `--ext_noise` ("Powers the external noise source
  (--ext_noise) - switched by calibration", heimdall's page's wording), an
  8th antenna (no GPIO). Closes on Esc (focus back to the button), a press
  outside, the button, a window resize. Tab goes from the button into the
  switches (the popover follows the button in the DOM).
- **Pending, no optimistic state**: a switch sends `ANT_BIAS:<mask>` and keeps
  its new position with the pending look (dimmed, "…" after the name) until
  heimdall's state shows that mask, an error comes for it (`error_seq` moved:
  shown in the popover's red line, the switch back to heimdall's state) or 5 s
  pass ("no answer from heimdall"). Other pages simply follow the state.
- **Unavailable**: hidden where heimdall's own page hides the bias tees -
  generic RTL-SDRs and the Wideband variant (state `generic` / `wideband`),
  so the two pages agree. Disabled (`aria-disabled`, not `disabled`: a
  disabled button shows no tooltip), reason in the title: no state yet; this
  page not connected; an older kraken_doa (its `system_status` has no
  `antenna_bias`: "this kraken_doa is too old for it - rebuild and restart
  it" - what the live page shows until the binary is rebuilt); heimdall not
  connected; heimdall too old ("update it (rebuild and restart heimdall)");
  a debug playback / recording playing (the state is still shown, the count
  too - they are the real receiver's). Nothing is shown as on while heimdall's
  state is not known.
- The same in all three operating modes (bias tees are hardware state);
  the rows follow heimdall's open channel count (`num_channels`, so an
  element-count change rebuilds them). Phone: beside Mode on the bar's line,
  34 px tall, the popover as wide as the screen allows, 42 px rows. Drive
  mode hides the top bar.
- Room: the bar had 22 px to spare at 1700 px with the sidebar (measured; not
  the ~48 px expected), so the Gain slider went from 120 to 100 px and the
  Audio slider from 80 to 72 px - the bar keeps one row with the button
  (6 px to spare). The top bar's rework later that day (icons for its
  captions, the speaker button, *sidebar.md* "Top bar and rail") gave them
  back 112 / 76 px; the button stayed as it was, now on the bar's centre
  line with the other controls.

## Protocol

- Page -> kraken_doa: `ANT_BIAS:<mask>`, bit N = antenna N, 0-255 (digits
  only). Only the open, switchable antennas' bits.
- kraken_doa -> heimdall (8092): `{"command":"set_antenna_bias_tees","mask":N}`.
  heimdall applies it to the open channels (the others keep their saved
  bit), saves it, and replies with the mask in effect, an error reply too.
- heimdall -> kraken_doa: its 2 Hz status carries
  `antenna_bias_tees_available`, `antenna_bias_tee_mask`,
  `antenna_bias_tee_reserved` (and `num_channels`, `generic_rtlsdr`,
  `external_noise`, `downconverter` as before).
- kraken_doa -> pages: `system_status.antenna_bias` (2 Hz), the same object
  as `{"antenna_bias":{...}}` on connect and right after a refusal or an
  error reply: `{state, mask, reserved, channels, external_noise, playback,
  error_seq, error}`. `state`: `ok`, `none` (no status from heimdall for 5 s),
  `generic`, `wideband`, `old` (heimdall's status lacks the fields),
  `unavailable`. `mask` is 0 unless `ok`.
- The client reads heimdall's control socket on its status thread
  (`antbias::ingest`, next to the version and the Wideband board): a line
  with `num_channels` is a status; a reply with `antenna_bias_tee_mask` and
  no `num_channels` is the answer to `set_antenna_bias_tees`. That
  connection used to open only with the first command; the drain now also
  opens it, every 5 s at most while it is down (not during a playback), so
  the state is there before anything was sent.

## Where each refusal is enforced

- **kraken_doa** (`antbias::handle_command`, uWS loop): not from a page's own
  socket (the settings replay, the AI assistant, the web mapper, a test
  transmission, the scanners - like `POWER:`; there is no generic "send a
  command" MCP tool, and the assistant's tools never build `ANT_BIAS:`, so
  this is the only gate it needs); a malformed mask; a debug playback or a
  recording playing ("Playback: the bias tees are the real receiver's - stop
  the playback to switch them"); heimdall not connected / too old / generic
  / Wideband; an antenna not open; a reserved antenna. A refusal for a page is
  broadcast as `{"antenna_bias":...}` with `error_seq` moved; the page that
  asked shows it. Nothing is persisted (not in the settings schema), echoed
  or replayed: heimdall owns the state and saves it - two stores would fight.
- **heimdall** (`request_antenna_bias_tees`): generic RTL-SDRs (never a GPIO
  write there), the Wideband variant (GPIO1-6 are its RF switches), CH0
  under `--ext_noise` (cleared, success with a message), a bad mask, during
  an element-count change, a GPIO write failing.

## Compatibility

- Older page, new receiver: nothing changes (it ignores the new field and
  message).
- New page, older kraken_doa: `system_status` without `antenna_bias` - the
  button is disabled with "rebuild and restart it".
- New kraken_doa, older heimdall: no `antenna_bias_tees_available` in its
  status - state `old`, the button disabled with "update it"; the command is
  never sent to it (it would answer "Unknown command").
- Older client, new heimdall: three more status fields, one more command -
  nothing changes for it.
- Both programs must be rebuilt and restarted for the feature: a running
  heimdall keeps its old control port.

## Safety (why two presses)

DC on a port can damage a DC-shorted antenna (a loop, a folded dipole, some
discones) or equipment behind it (a splitter, a filter, a signal generator).
So there is no single-press "on" in the bar: the button only opens the
popover, a switch in it acts - and the popover's first line says what it
does. heimdall's own page has had the same switches since before this
(Array Elements card); the saved state is re-applied at every start, as
before. The voltage and current the KrakenSDR's bias tees supply are not
stated in heimdall's code or docs, so the page names none.

KerberosSDR (`--kerberos*`): available, as on heimdall's page - the code does
not refuse it, and whether that hardware has per-port bias tees is not known
here.

## Tests (2026-11-18)

`test_bias.py` in the job's scratch directory, against `fake_heimdall.py`
(a Python heimdall on 127.0.0.1:28091 / 28092: a minimal 8091 stream, the
8092 status at 2 Hz and the replies, every command logged; personalities
KrakenSDR, old heimdall, generic RTL-SDRs, Wideband, `--ext_noise`,
KerberosSDR; 4 / 5 / 8 channels; failing GPIO writes, no answers, heimdall
gone), headless Chromium with real mouse, key and touch events (105 checks):
the place right of Mode on the controls' line, one row at 1700 px, the icon,
aria; 0 / 1 / 2 / all on (count, colour, title, width unchanged); the
popover's rows, warning, placement, titles; a click on a switch -> the page's
`ANT_BIAS:` -> heimdall's JSON command -> the state back on this page and a
second page; All on / off; the row's name toggles; pending without an
optimistic button; nothing in `doa_settings.json`; Esc / outside / button /
resize; Enter, Tab, Space; help mode on the button and in the popover;
hover; independent and wideband scan mode; refusals by the client (bad,
not open, reserved, generic, Wideband, old heimdall) with nothing reaching
the fake; heimdall's error reply shown only on the page that asked; no
answer for 5 s; 4 and 8 channels (Antenna 7 disabled); `--ext_noise`;
heimdall gone and back (the client reconnects by itself); this page's
connection lost; a debug playback (refused, nothing at all sent to heimdall);
the phone; no JS errors. heimdall's side ran in a harness
(`tmp/hctl/harness.cpp`: the real `process_command` and status with the
GPIO call wrapped, 34 checks) - not on hardware.
