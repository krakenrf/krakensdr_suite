# Digital voice / data decoders (engine, protocol receivers, decoder tabs)

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Digital voice/data decoders (one per VFO): engine `src/digital/`, decoders `plugins/`:**
- EVERY decoder is a plugin (out-of-process, see *Decoder plugins* below);
  kraken_doa contains no protocol code. Shipped: p25, dmr, tetra, dstar,
  nxdn, ysf, mpt1327, pagers (see *Pagers*; was pocsag), aprs, adsb (see *ADS-B* below), ais (see *AIS*),
  radiosonde (see *Radiosonde*), dme and ils (see *DME / TACAN* and *ILS /
  VOR / marker*), ism (see *ISM devices*), radar (see *Pulse radar*),
  ssr1030 (see *1030 MHz interrogations*), uat (see *UAT*), flarm (see
  *FLARM / OGN*), analog_id (see *Analog FM IDs*; was ctcss), beacon406 (see *406 MHz
  distress beacons*), beacon121 (see *121.5 MHz beacons*), lora (see *LoRa / Meshtastic / LoRaWAN*), wildlife (see
  *Wildlife collars*), rds (see *RDS / RBDS + SCA*), acars (see *ACARS*), vdl2 (see *VDL Mode 2*), dab (see
  *DAB / DAB+*; multichannel, a talker per transmitter by its TII code), dtv and cells
  (ARRAY plugins - see *Digital TV* and *LTE / 5G NR Cell Decoder*)
- Which plugins run in Auto detect is the USER's choice: the "Auto detect"
  tick per plugin in the plugin list (⚙ Setup Settings → Decoders), all on by
  default. WS `PLUGIN_AUTO:id:0|1` -> `PluginRegistry::set_auto()` (bumps the
  registry generation, so decoders in AUTO rebuild their plugin set) and
  records the setting `AUTO_DETECT_OFF:id,id` (settings_store schema, before
  DECIMATORS; replayed through `set_auto_off()`); the plugin list's `auto`
  field is that state. `kp::Info::auto_detect` is deprecated and ignored
- `DigitalDecoder` (digital_decoder.hpp) hangs off a `DecimatorInstance`
  (`digital`, created on first use, `digital_mode` mirrors the mode). The
  decimation pass pushes the VFO's decimated samples (the beamformer output
  when beamforming ran for it, else the listened-to channel - the same stream
  FM audio gets) with `push()`; a worker thread ("digital-dec") per decoder
  drives the plugin processes, so the pipeline never waits. Queue capped at
  1 s (oldest dropped, `dropped` in the status). In FM/digital-only mode (DoA
  and beamforming off) only the VFOs heard (🔊) and the VFOs with a decoder are
  decimated, on one channel (`fm_only` in data_receiver.cpp)
- Modes (`dig::Mode`): OFF, AUTO, PLUGIN (+ plugin id). Wire / snapshot form
  `OFF`, `AUTO`, `PLUGIN:<id>`; `parse_mode_string()` maps the old protocol
  names (P25, DMR, TETRA, DSTAR, D-STAR, NXDN, MPT1327) to their plugins and
  renamed plugins to their new id (`renamed_plugin()`: `sfn` -> `dtv`,
  `ctcss` -> `analog_id`, `pocsag` -> `pagers`; the snapshot's `sfn.*`,
  `ctcss.*`, `pocsag.*` options too), so saved settings and old clients keep
  working
- `Engine` (digital_decoder.cpp): AFC mixer at the VFO rate -> one msresamp
  per distinct plugin `sample_rate` (shared by the plugins at that rate; a
  plugin declaring 0 gets the VFO's rate unresampled - `Runner::vfo_rate`,
  started `--serve --rate HZ`, restarted when the VFO's rate changes) ->
  each plugin process (`Runner`). AUTO runs one Runner per built plugin the
  user left in Auto detect (rebuilt when the registry generation changes)
  minus `manual_only` plugins (`kp::Info::manual_only`, ADS-B - the plugin
  list shows no Auto detect tick for them, `digAutoPlugins()` leaves them
  out) and those whose `min_vfo_rate` is more than 2x the VFO's rate
  (`auto_ids()`, `AUTO_MAX_RATE_SHORTFALL`: ADS-B's 2.4 MHz upsampled from a
  12.5 kHz VFO only burned CPU) - so AUTO starts its plugins with the first
  block (rate known) and rebuilds when the VFO's rate changes the set;
  detection = majority of `valid()` frames in a 4 s window. Only the "lead"
  plugin (the fixed one, or the detected one) gets VOICE_WANTED and has its
  audio / voice state / carrier offset used. AFC: plugin offset reports are
  averaged and applied at most every 300 ms, ignoring reports from the 300 ms
  after a correction (the pipe latency made an every-report integrator run
  away - TETRA drifted -5 kHz). With no correction (yet) the mixer is a
  plain copy (a sin/cos per sample cost ~5% of a core at 2.4 MHz) and its
  phase goes back to 0; idle, the correction decays to the VFO centre and
  lands on 0 below 0.5 Hz (until 2026-10-16 the phase of the last
  correction was kept for good: once a decoder that reports an offset -
  D-STAR, P25, ... - had run on a VFO, a later multichannel plugin there -
  lora, ism - got one antenna until kraken_doa restarted: a debug playback
  of lora-mesh-five after dstar-two-radios had no separation by code). A VFO
  retune (> 100 Hz) or mode change resets everything (RESET to the plugins,
  facts + map points cleared; `reset_pending_` is consumed by the worker)
- Station location for the plugins: `dig::set_station_location()` (called
  by `build_map_message` every second from `station_info.resolve()`; 0,0 =
  unknown) bumps a generation; each Runner sends wire STATION "lat,lon" (""
  = unknown) when it changes and after a (re)start -> `kp::Host::station()`
- Map points: wire MAP_POINT (id, lat, lon, label, kind, heading, altitude_m,
  speed_kmh, ttl_s, info - '\0'-separated text, "" = unknown; validated in
  `on_map_point`, ttl clamped 10 s..24 h) / MAP_REMOVE -> `Report::map_set /
  map_remove` (key plugin|id, max 2000 per decoder, expired by ttl).
  NaN marks unknown fields: test with `is_finite_value()`
  (utils/parse_num.hpp) - kraken_doa built with -Ofast until 2026-10-23,
  which folded `std::isfinite` to true (a `"h":nan` broke the page's
  JSON.parse; digi_test (-O2) didn't show that); its Makefile adds
  -fno-finite-math-only now, and is_finite_value is right under any flags
- The protocol receivers live in the plugins (`plugins/<id>/<id>.cpp`), on
  the shared library `plugins/lib/` (libkrakendig.a): `dig_common` (the
  RxContext / Report / Options interface they were written against, mapped
  onto `kp::Host` by `dig::Bridge`; `FmFrontEnd` = 48 kHz Kaiser 6.5 kHz +
  discriminator in Hz; `RrcFrontEnd`), `dig_fec` (block codes by nearest
  codeword, RS GF(64) Berlekamp-Massey, soft Viterbi, CRCs), `dig_fsk4`
  (P25/DMR 4FSK sync receiver -> `Fsk4Sink`), `dig_vocoder` (IMBE, mbelib
  AMBE, TETRA ACELP), `mbe_tables.hpp`. Each receiver is sync-driven -
  nothing is reported unless its own FEC/CRC passed:
  - `Fsk4Receiver` (lib/dig_fsk4.cpp): P25 on an integrate-and-dump filtered
    discriminator, DMR on RRC 0.2; normalized sync correlation per sample,
    the sync's own +-3 symbols give timing, level and centre (= carrier
    offset), no symbol-timing loop. DMR voice bursts B..F (no sync) are
    decoded at positions extrapolated from the last sync. DMR data sync =
    voice sync with every symbol negated (`^ 0xAAAA...`, NOT `~`). The p25
    plugin enables only P25, the dmr plugin only DMR. A DMR VOICE sync off
    the 30 ms grid of a CONFIRMED burst sequence (`dmr_confirmed_`: its
    last burst checked out - EMB / slot type + FEC, or a voice sync on the
    grid) is ignored: random voice payload correlates >= 0.8 with a sync
    word now and then (~1 in 50 bursts of random AMBE data), and burst A is
    checked by nothing, so a false one moved the extrapolation of B..F off
    their timing; a sequence a bare voice sync started is not confirmed
    (any sync re-anchors it, as before - a real MS recording has a false BS
    voice sync 10 ms after a radio's real one) (2026-10-16, found with the
    dmr generator). P25 (2026-11-19): a frame goes to the sink as soon as its
    own length is in (the NID, `p25_nid_decode` in dig_fec, gives the DUID:
    HDU 396 dibits, TDU 72, TDULC 216, LDU 864, a TSDU once its own block
    is) instead of after 864 dibits - a lone trunking block ~33 ms after its
    sync instead of 180 ms, terminators 100-165 ms earlier, a recording's
    last 0.2 s decoded; sample positions unchanged. CQPSK read through the
    discriminator: the +-6 kHz per-sample limit (against noise clicks) cut
    its fast phase steps near the envelope nulls (6 % of on-air samples,
    outer levels squeezed, the centre fit biased as the carrier moved), so a
    second boxcar keeps what the limit took (modulo one turn, 4800 Hz on
    the boxcar); sync detection and the fit stay on the limited one, and a
    frame is sliced from the restored one only when its sync scale was
    squeezed below 0.85 of the exact one (CQPSK 0.65-0.78, C4FM / LSM
    0.91-1.0 down to 4 dB SNR; restoring everywhere cost 0.11-0.16 dB on
    weak C4FM / LSM). Real CQPSK voice channel: 90 of 90 frames at any
    carrier offset within +-1 kHz (before 68-90), half-frame SNR 16.9 ->
    14.25 dB; C4FM / LSM unchanged; cost 1.03 -> 1.06 % of real time
    (plugins/p25/README.md)
  - `P25Proto` (plugins/p25): NID BCH(63,16) (`p25_nid_decode`, dig_fec:
    the last NAC first, else all 65536 - one 512 KB table per process,
    shared with the receiver), TSBK (rate 1/2 trellis + CRC; the trellis'
    branch metric = absolute error capped at 4 level units since 2026-11-19,
    `p25_trellis_half` - the soft symbols' noise is heavy-tailed near
    threshold, kurtosis 9-12 on real C4FM / LSM and 14-30 through CQPSK,
    and a squared error let one click steer the path: half of the blocks
    at 0.3-0.6 dB less SNR, false accepts unchanged), LDU1/LDU2
    (Hamming(10,6) + RS(24,12)/(24,16)), HDU (Golay(18,6) + RS(36,20)),
    TDULC (Golay(24,12) + RS(24,12)), PDU header. IDEN_UP tables turn
    channel numbers into MHz. Option `nac`. Link controls with the SF bit
    (LCO 0x02 / 0x22 / 0x23) are the trunking-block bodies of TSBK 0x02 /
    0x3C / 0x3A and go through `tsbk()`; TSBKs also decoded: secondary
    control channel (0x39, explicit 0x29), SNDCP data channel grant /
    announcement (0x14 / 0x16), location registration (0x2B) - layouts
    checked on real control channels (README). A call (2026-11-19): played
    only once known to be clear (ALGID 0x80 or an unprotected voice-user
    LC), up to 2 LDUs held until then; one "Voice:" / "Call ended" event per
    transmission (the talker logic's), no dedup, nothing from hang-time
    terminators
  - P25 Phase 2 (plugins/p25, 3.0, 2026-11-18; details, measurements and
    sources: plugins/p25/README.md): `P25p2Phy` (p25p2_phy.cpp) runs on the
    same 48 kHz complex input as the Phase 1 4FSK path (decoder.cpp feeds
    both; no phase setting - a traffic channel can switch between Phase 1
    and Phase 2 calls). 6000 symbols/s, 8 sps, one 41-tap low-pass (-6 dB at
    5 kHz). Outbound H-DQPSK: differential detection at the symbol centres,
    acquired on two S-ISCH syncs 180 / 540 symbols apart (either
    polarity), timing / carrier / scale tracked per slot, grid dropped after
    34 slots without a sync, re-anchored after sample slips. Inbound H-CPM
    bursts: the discriminator integrated between symbol centres (a
    duobinary-like response, 489 Hz x (I[n] + I[n+1])) into a 16-state
    sequence detector, voice bursts (no sync) on each radio's own timing /
    carrier / deviation track. It hands 30 ms slots (`P25p2Slot`,
    p25p2_slot.hpp) to `P25p2Proto` (p25p2.cpp): I-ISCH (40,9,16) / DUID
    (8,4,4) by nearest codeword, PN44 descrambling (seed = WACN | system ID
    | NAC from the options, a control channel's network status or BLIND:
    solved from one clean scrambled FACCH / SACCH burst by GF(2) linear
    algebra on its RS parity relations), RS over GF(64) from the (63,35,29)
    mother code with the punctured symbols as erasures
    (`rs64_decode_erasures`, `crc_bits` added to lib/dig_fec), CRC-12 /
    CRC-16, MAC PDUs + messages (partition-01 TSBK bodies through
    `P25Proto::tsbk()`, which shares its channel identifiers and network
    identity), voice + ESS. Options `wacn`, `sysid`, `lch` (Timeslot) next
    to `nac`; they take effect at once while it runs (a complete identity
    replaces the seed in use, a NAC alone drops a contradicting one). Facts
    both phases write (Encryption, Channel type, WACN / System ID / NAC):
    the phase that decoded last shows its value (`P25p2Proto::phase1_frame`
    after every valid Phase 1 frame - Phase 2's cached values went stale
    before). The receiver never hands on a non-finite symbol (non-finite /
    absurd input samples are zeroed); the MAC treats one as no reliability
    (`p2_finite`, a bit test, p25p2_slot.hpp). `rs64_decode_erasures`
    returns errors + erasures also for a word that is a codeword already.
    Call events (2026-11-19) follow the talkers, as Phase 1's: one "voice:"
    when a transmission's unit is named, one "call ended:" when that
    talker ends (`announce` / `announce_end` from `talker_set` /
    `talker_end`, inbound `in_named` / `unit_end`), no dedup; before they
    followed the call with 5 s / 2 s dedup and lost starts / ends on 6 of
    14 real traffic channels
  - P25 trunk following (3.1, 2026-11-20; plugins/p25/README.md *Trunk
    following*): `P25Trunk` (p25_trunk.cpp), one for both phases, fed by
    `P25Proto::tsbk()` (so the TSBK, link-control and Phase 2 abbreviated
    forms share it) and the Phase 2 MAC: `trunk_system` (WACN.SYSID, RFSS /
    site, control = trunking blocks / LCCH in the last 10 s; sent at most
    once per input block from `tick()`, before any grant / channel / call),
    `trunk_channel` (site / neighbour / secondary control channels, data
    channels; again when the identifier arrives), `control_channel` (the
    RFSS status names a control channel > 4 kHz from the VFO's RF), `grant`
    for every grant form (TSBK 0x00 0x02 0x03 0x04 0x06 0x08 0x09 0x14, MFID
    0x90 0x02 / 0x03, Phase 2 0x05 0x25 0xC0 0xC3 0xC4 0xC6 and the 0x4x
    abbreviated ones; `phase1_parses` now also 0x06 0x14 0x39) with options
    nac / wacn / sysid / lch / phase (new hidden option: which receiver runs),
    channel -> downlink / uplink / slot through the identifier tables (TDMA:
    number / slots, slot = number % slots), and `call` per slot (Phase 1 -1,
    Phase 2 LCH; ACTIVE / ENDED / RELEASED - README). Generator: `p25_gen.py
    --control` (TSDUs, its TSBK coding reproduces 361 real blocks of three
    sites bit for bit), scene `p25-trunk-three-calls`
  - `DmrProto` (plugins/dmr): CACH TACT (timeslot), slot type Golay(20,8)
    (max 2 corrections - a third of random words are within 3), BPTC(196,96)
    voice LC header / terminator (RS(12,9)), CSBK, PI and data headers, EMB
    QR(16,7) + embedded LC, GPS and talker alias LCs. Control-channel chatter
    (Aloha, broadcasts, acks) only with the verbose option. Option `slot`.
    The kind + slot that extrapolated bursts are read with (`ext_bs_`,
    `ext_slot_`) come only from a burst that checked out (`set_ext`): a
    false direct-mode data sync inside a repeater's burst made the next
    burst B..F skip its CACH, on the wrong slot (2026-10-16). Real
    recordings: dmr_1 263 -> 269 valid bursts, dmr_it_8 604 -> 605, dmr_2
    identical, talkers unchanged.
    Trunking (2026-11-20, plugin 2.1, `dmr_trunk.cpp`): Tier III TSCC ->
    `trunk_system` (C_SYScode: model + NET = key, SITE), every grant form
    (0x30-0x38; MBC with CG_AP = absolute downlink / uplink + colour code),
    C_BCAST (Chan_Freq / adjacent site / vote now -> `trunk_channel`), C_MOVE
    mask 0 -> `control_channel`; payload channels -> `call()` per slot
    (LC -> ACTIVE, terminator -> ENDED, P_CLEAR -> RELEASED), the CACH short
    LC (C_ / P_SYS_Parms) for the system on either; Capacity Plus rest
    channel only (README). Opcode names corrected: 0x2E is P_CLEAR, 0x2F
    P_PROTECT, 0x39 C_MOVE (were C_MOVE / P_CLEAR / -). An extrapolated
    burst takes its slot from the 30 ms alternation (by sample position;
    was a wall-clock window) when its TACT disagrees: a miscorrected TACT on
    an empty or weak slot named the other slot and cut its superframe short.
    A terminator's hang-time repeats (every 60 ms until P_CLEAR on a Tier
    III payload channel) no longer make a 27 ms talker each. Review fixes
    (2.2): a CACH short LC counts only from a burst whose slot type / EMB
    passed (with the `cc` filter: of that colour code) - a co-channel TSCC's
    C_SYS_Parms made a payload VFO claim `control`; a `traffic` instance
    (grant option) and a single-slot instance on a composite TSCC never do;
    an MBC continuation must be the header slot's next burst (60 ms +-3
    symbols); a Capacity Plus rest-channel move needs 3 status CSBKs in a
    row, at most one per 5 s. 2.3 (sanitizer run): a DMR sync less than 89
    symbols after the stream start / a reset() is skipped in
    `Fsk4Receiver::on_sync` (its burst was read in front of the buffer -
    ASan heap-buffer-overflow in 509 of 1023 runs of a start-offset / RESET
    sweep, 0 after); `SymSrc::sym` returns 0 outside the history (all
    users of the lib receivers). P25 reads at most 23 symbols back - the
    sync itself, which the correlator only finds inside the buffer; NXDN
    has its own receiver and checks its FSW start. A call without an LC
    needs the colour code known + two good EMBs (a false voice sync + one
    lucky EMB made calls from noise: 2 per hour, now 0)
    Option `net_tag` (2.4): two unrelated Tier III networks on air here send
    the same network code (Huge / NET 1, at 455 and 414 MHz) and nothing
    else network-wide is on air - a tag the user sets on one network's
    control VFOs is added to the key (`Huge.1.tait`; Capacity Plus too) and
    passed on by every grant. Gateway / special addresses (annex A.4) are
    reported by their alias (`PSTNI`) in grants and calls
  - `DstarReceiver` (plugins/dstar): RF header (scrambler, 24-column
    interleaver, K=3 Viterbi, CRC-16/X.25), slow data (text message, GPS /
    DPRS, header copy), end pattern
  - `NxdnReceiver` (plugins/nxdn): NXDN96 (4800 Bd, 10 sps) and NXDN48
    (2400 Bd, 20 sps) searched at once, each on its own RRC 0.2 filter;
    10-symbol FSW (0.88 threshold), whole-frame descrambler (negates
    symbols), LICH (8 bits on the first bit of 8 symbols, second bits 1,
    even parity of bits 7..4), SACCH (5x12 interleave, puncture every 6th
    from 5, K=5 G 0x19/0x17, CRC-6 0x27) and FACCH1 (9x16, every 4th from 1,
    CRC-12 0x80F). A SACCH counts only when it follows the previous good one
    exactly one frame later (CRC-6 alone passes 1 random word in 64);
    FACCH1 counts alone. Layer 3: VCALL (unit/TG, cipher), TX_REL, DCALL,
    trunking CAC (site / service info, grants). Voice: 4 (or 2) AMBE+2
    frames per frame -> mbelib (nW..nZ). Trunking (2.1, 2026-11-20, from
    the NXDN Forum's TS 1-A / TS 1-C 1.3; plugins/nxdn/README.md): the
    outbound CAC only (300 bits, 25 x 12, puncture 1111111 / 1011101, CRC-16
    preset ones over SR + 144 + 3 null), single or dual message format (SR
    structure bit 0: two 9-octet messages); UDCH / FACCH2 (348 bits, 29 x
    12, CRC-15) on traffic channels - a false sync whose LICH read as UDCH
    no longer resets the SACCH lock. Reports through kp::Host directly
    (`NxdnReceiver(ctx, &host)`): trunk_system (key = Location ID category
    + system code, `global.123`), trunk_channel (SITE_INFO / CCH_INFO /
    ADJ_SITE_INFO control channels), control_channel (CCH_INFO New, this
    site), grant (VCALL_ASSGN / _DUP, DCALL_ASSGN / _DUP; channel `CH n`,
    no frequency - Type-C channel plans live in the radios; options `ran`,
    `rate`), call (VCALL ACTIVE, TX_REL / TX_REL_EX / 1 s gap ENDED, DISC
    RELEASED only for a call this instance reported - a DISC heard first
    may be the previous call's; RTCH / RTCH_C only). Any SACCH superframe
    message needs its 4 fragments from 4 consecutive frames in order (a
    failed head fragment once let the previous superframe's complete it:
    message type, CC Option, call type). The system is sent every 5 s and
    ahead of a grant (the engine can drop it without a RESET: Auto
    detect's lead changing). Hidden
    options `ran` (a frame's SR RAN, inherited by frames that name none)
    and `rate` (one branch only). VCALL Transmission Mode 011 (EFR) is not
    played. Not decoded: direct frequency assignment (not in 1.3), Type-D.
    Control channel not seen on air
  - `YsfReceiver` (plugins/ysf, 2026-11): Yaesu System Fusion, written
    from Yaesu's "Amateur Radio Digital Standards" 1.02 alone (section
    numbers are that document's). 4800 Bd C4FM, RRC 0.2 (span 4, 10 sps);
    20-symbol FS D471C9634D (0.75 normalized threshold, either polarity,
    level / centre fitted on the FS). After a good frame the next one is
    tried on the 100 ms grid (+-3 samples, the last good level when the FS
    fit is off) even without a sync peak; 4 misses = out of sync (4.7).
    FICH: 5 x 20 dibit deinterleave, K=5 G 0x19/0x17 soft Viterbi, 4 x
    Golay(24,12) (Appendix A = `golay24()`, checked row by row), CRC-16
    (init 0, inverted, S15 sent first) - nothing is reported unless it
    passes (random input passes ~1 in 300000; ~3 sync candidates a second
    in noise). DCH units: 9 (160 bits) or 5 (80 bits, V/D mode 2) x 20
    deinterleave, Viterbi, CRC over the WHITENED data, PN(9,5) (init
    111001001, out S00, S08 <- S00 ^ S04). Layout by FI / DT: HC / TC and
    Data FR two 160-bit units in alternating 72-bit pieces (Data FR unit =
    2 FN + slot), V/D 1 DCH + 5 x 72-bit AMBE+2 frames (rW..rZ, like DMR),
    V/D 2 (DN) 40-bit DCH pieces + 5 x 104 voice bits (26 x 4 deinterleave,
    de-whitened, 27 bits x 3 by soft majority + 22) -> 49 bits ->
    mbelib's `mbe_processAmbe2450Dataf` (`MbeLib::decode49`, optional),
    Voice FR (VW) 5 x 144 (sub header FT 1 / FN 0: CSD3 + VCH 3, 4) ->
    `ImbeDecoder` in the YSF order (`vw_plain_index`: the plain IMBE
    codewords c0..c7 as a 24 x 6 block, read two rows per 12 air bits; at
    first P25's iW..iZ order, wrong - corrected 2026-11-16 after the dsdcc
    check). A call = valid frames < 1 s apart
    (two headers in a row = one; a repeated TC is no new call); its event
    waits for source + destination + CSD2 (late entry: at most 8 frames;
    an all-asterisk destination - an off-air call to everyone sent `*****`
    in Radio ID mode - is shown as "ALL", the fact keeps `***** (all)`);
    a lone FICH with no DCH and no second frame gives no call events.
    Ambiguities resolved: the FICH interleave (text "M = 5, N = 10",
    Figure 4-26: 20 rows and the air order - followed the figure); the CRC
    bit order (not given - S15 first); whitening before the CRC (Figure
    4-27; 4-30 / 4-31 omit it); "divided into 3 bits each" = each bit sent
    three times in a row (Figure 4-32's arrows), its output order S0 S26
    S52 S78 S1 ... taken over its contradictory direction labels; DG-ID =
    the 7-bit SQL code; CS = 10 not enforced. NOT in the standard: the bit
    order inside the voice frames (DN's 49 bits passed as received =
    mbelib's u0 u1 u2 u3 order, the 27 triplicated bits then being u0, u1
    and u2's top 3; V/D 1 as DMR's 72-bit frame; VW in the order the
    reference decoders document). The plugin's generator
    (`tools/ysf_gen.py`, written independently from the same document;
    VW = real IMBE frames) agrees bit for bit; dsdcc (GPL, run in scratch
    as a reference only, 2026-11-16) reads the generator's frames, CSD and
    voice bits the same way (DN 174 / 175, V/D 1 100 / 100, VW 161 / 169
    frames bit-exact - the rest its own symbol errors; plugin README).
    Off-air IQ recordings of a DN and a VW call (2026-11-17): every
    frame decoded, VW's IMBE codewords all valid as received (plugin
    README); DN (mbelib) and VW audio of those recordings is speech,
    V/D mode 1 voice not heard from a real radio yet. DN silence frames
    (b0 124 / 125) are muted, the leading ones of a call also reset the
    codec (`dn_lead_`) - they made a full-scale burst at the call's start
    (*Digital voice*).
    `KRAKEN_YSF_DUMP=1`: frames, DCH units, voice bits to stderr
  - `Mpt1327Receiver` (plugins/mpt1327): MPT1327 / MPT1343 analogue
    trunking signalling, 1200 bit/s FFSK (1 = 1200 Hz, 0 = 1800 Hz) on the
    FM discriminator. Two sliding one-bit (40-sample) tone correlators; each
    tone is normalized by its own running level (radio pre-emphasis makes
    1800 Hz louder - without it the sync never correlated). Sync = 4
    preamble bits + SYNC 0xC4D7 (control) / SYNT 0x3B28 (traffic), hard
    match <= 2 bit errors. The SYNC is the CHECK FIELD of the control
    channel system codeword (CCSC = 0, SYS 15 bits, CCS, 0xAAAA), so the
    CCSC starts 44 bits before the pattern; the address codeword (message)
    follows it. Codewords: 48 bits + BCH(63,48) g 0xE815 with the last check
    bit inverted + even parity; on the control channel single-bit errors
    corrected (facts, events, valid slots), after a SYNT and for every trunk
    report error-free only. A CCSC counts only on the 128-bit slot grid of
    another with the same SYS (a lone one passed 7 times in 1 h of noise, 8
    in 1 h of FM speech; with the check none). A GTC after a SYNT counts
    only when the same codeword comes twice within 2 s (3.1: one codeword
    after a false SYNT has 18 bits of protection - ~400 false grants per
    VFO-year in noise at 11 000 SYNT candidates an hour); a CLEAR has 38
    (REVS, type). The system goes out at once after a reset, before any
    grant. 3.2 (after real calls, NZ 2026-11-20): a granted traffic channel's
    carrier = the call ACTIVE (the radios' MAINT hardly ever reached the
    downlink: 2 Pressel On in 24 calls; the carrier was up from before the
    GTC to the CLEAR); control-channel slots on a traffic VFO count only with
    that channel's CHAN4 (the site's control channel came onto 5 of its
    traffic channels by intermodulation); ACKV QUAL 0 marks a unit; option
    `undecided` (group / unit) for calls nothing shows to be unit calls; a
    release forgets the call's parties. The discriminator's DC is
    removed before the correlators (an offset of 2 kHz lost most slots: the
    1.5 cycles of 1800 Hz don't reject DC) and reported as freq_error.
    Messages (field positions checked against MPT 1327 section 5,
    2026-11-20): GTC (bit 22 = 0: PFIX, IDENT1, D, CHAN, IDENT2), Aloha
    (CHAN4), ACK*, AHY (POINT, E), AHYX / AHYP / AHYQ / AHYC, MARK, MAINT
    (CHAN bits 31-40, OPER), CLEAR (CHAN, CONT, REVS), MOVE (CONT, (M)),
    BCAST SYSDEF 0-5 (CHAN, SYS, ADJSITE; PER / IVAL / PON / ID), data
    headers; idents shown prefix-ident. Before 3.0 MAINT / MOVE read their
    channel from bits 16-25 (wrong). Trunk following (version 3.0): the
    system (key = SYS hex, or MPT 1343's OPID / NET with the System key
    option), grants per GTC (target IDENT1, source IDENT2, D = data,
    emergency from ACKE / AHY E, unit vs group only from an AHY or an
    earlier call by that ident - not in the GTC -, update = the same GTC
    within 5 s, demod nfm, options chan / sys / target / source / group),
    BCAST channels as trunk channels, MOVE (M) = 0 as control channel. Channel
    numbers -> Hz only with the Channel plan option (MPT 1343 Band III;
    else the receiver's table). On a traffic channel: MAINT Pressel On /
    Periodic -> call ACTIVE, Pressel Off / Disconnect -> ENDED, CLEAR (REVS,
    CHAN) -> RELEASED (one is enough), a CCSC or a replacement GTC releases
    too; no Pressel Off: ENDED 1 s after the carrier goes (noise squelch on
    the discriminator's first difference); a talker per speech item named
    by the MAINT ident ("via site" on the downlink). Validated on live NZ
    control channels (SYS 0x42E1 ~96 % of slots; 2026-11-19 capture: 0x42E1
    / 0x42C1 / 0x4221 / 0x4201, channel plan 414.0000 MHz + n x 12.5 kHz
    from their own CHAN4 and each other's vote-now channel numbers); details
    and tests in the plugin README
  - `TetraReceiver` (plugins/tetra, 72 kHz RRC 0.35 input): differential
    detection, 4th-power frequency estimate, slot sync from the training
    sequences (both spectral orientations tried), BSCH (MCC/MNC/colour code
    -> scrambling code), BNCH SYSINFO, AACH (Reed-Muller), SCH/F, SCH/HD
    and STCH MAC PDUs (2026-11-20, version 2.2): MAC-RESOURCE with every
    header field and the channel allocation element (`parse_alloc`,
    EN 300 392-2 table 21.87, the augmented fields too), fragmented TM-SDUs
    reassembled per timeslot (`frag_`: MAC-RESOURCE LI 0x3F + MAC-FRAG +
    MAC-END; a failed CRC on that timeslot or 8 slots without a fragment
    drop it), the basic link (FCS = CRC-32 of annex C checked), MLE, the
    CMCE call control PDUs with their type-1 / 2 / 3 elements
    (`parse_cmce`, annex E rules; a PDU counts only when its elements end
    exactly at the TM-SDU's end). Trunking: allocation + call PDU ->
    `grant()` (carrier -> Hz with SYSINFO's or the extended band / offset,
    uplink by TS 100 392-15's duplex table), a call table by call
    identifier (64) for target / source / encryption across PDUs,
    D-RELEASE -> `grant_end()`; a TRAFFIC instance (hidden option
    `timeslot`, set by the grant with `mcc` / `mnc` / `cc` / `call` /
    `group`) plays, reports valid frames / cell packets and `call()` /
    per-transmission talkers for that timeslot only, valid frames only
    while its AACH says assigned (`tr_`: ACTIVE at TCH
    or D-TX GRANTED, ENDED at D-TX CEASED / hang time / 1 s without
    traffic, RELEASED at D-RELEASE / the AACH unallocated twice); with the
    identity options it accepts that cell only (another cell's BSCH is
    ignored, the AACH counts only within 1.5 s of a CRC-valid block of the
    wanted cell). Call records start anew at each D-SETUP and go 60 s
    after last heard. Search (review fixes 2026-11-20): a training
    sequence counts anywhere in the window (2.1 missed carriers that kept
    an unlucky place in the slot grid - after a RESET, after silence),
    one slot dropped per failed search, repeated in `process()` until
    less than a window is left (bounded history; a half-slot drop per
    10 ms block grew it without limit); a RESET clears the statistics.
    After a loss of slot sync the TDMA numbers are unknown (nothing
    timeslot-dependent reported, tracked or played) until the cell's BSCH
    or until the carrier is back on the old slot grid (`resume_numbering`:
    a whole number of slots after the last good one within 2 samples, at
    most 10 s later; a 250 ms fade costs a call ~0.33 s of talker / audio;
    before, the old count ran on and a fade put another timeslot's grant /
    target on a traffic instance - the sanitize run's damage sweep). Real
    sigidwiki recording: 8 allocations, all parsed, all pointing to the
    timeslot the AACH marks as traffic. Details and tests:
    plugins/tetra/README.md
- Report (`dig_report.hpp`): facts table per plugin id (key, value, age),
  event log (400 entries, identical texts de-duplicated per window; events
  carry `src` = plugin id and `p` = its name, "" = the decoder itself).
  `MessageBuilders::build_digital_message()` pushes `{"digital":[...]}` on
  TOPIC_CTL at 4 Hz (only while a decoder is on), with only the events since
  the previous push; `DIGITAL_HISTORY:id` replies with the whole log,
  `DIGITAL_CLEAR:id` empties it. Status: `mode`, `detected` (plugin id),
  `detected_name`, `frames` {id: [4 s, total]}, `plugins` [{id, name, state
  running|starting|missing|not built|crashed|error, error, dropped, log}],
  `info` {id: facts}, `voice`, `opts`, `map_capable` (a running plugin
  declares `kp::Info::map`), `map_points`, `tables` (below; only in some
  pushes), `choices` (below; in every push, `{}` = none)
- Tables (`kp::Host::table_columns / table_row / table_remove / table_keep`,
  wire 25-27, 32; `table_detail`, wire TABLE_DETAIL 38): one per plugin in
  `Report` (headings + rows by key, max 1000, a row not updated for the
  table's keep time - 10 min, or what the plugin set with `table_keep`, 60 s
  .. 7 days - left out; `table_clear()` empties the rows on a retune / mode
  change, headings stay; a row's details - label / value pairs, <= 200 -
  live in its `TableRow::detail`, kept across row updates, dropped with the
  row, a detail for a missing row ignored). `status_json(.., tables)`:
  `"tables":{"<id>":{"cols":[..],"rows":[[key, age_s, cells..]],
  "det":{"<key>":[[label, value]..]}}}` -
  `build_digital_message` adds it at most once a second per VFO (+ every
  history reply); the page keeps the last tables in between
  (`digOnMessage`, same mode). UI `digTablesHtml(tab)` merges the plugin's
  table of every VFO in the tab with leading "VFO" / "MHz" columns (row key
  `vfo|plugin|key` = its map point) + the tab's incidents -> `#info-dec-table`
  above the facts / log (`#info-body-dec.has-table`; the panel grows to
  420 px the first time): header click sorts (numeric when both cells are
  numbers, empty cells last; per plugin in localStorage
  `kraken_dig_table_sort`), "Seen" column from the row age, a row whose
  `vfo|plugin|key` is a map point is clickable (`digTableRowClick`: opens
  the map, selects + centres the marker); one whose key is a talker of that
  VFO selects it in the 📡 DF panel; one whose key is a value of the
  run-time choices of one of the plugin's options (below; 2026-11-17, dab:
  an audio service's SId) selects that choice (`digRowChoice` /
  `digPickChoice`: the dropdown's `DIGITAL_OPT:<vfo>:<id>.<key>:<value>`,
  not sent when it is the value set; for a `voice` plugin also the tab's
  Listen - `digListenFor` - when the VFO isn't heard, so a listener just
  hears the new choice). The first that applies counts: map point, talker,
  choice (the map / talker rules as before; a plugin wanting a selectable
  row keeps that key out of its map points and talkers). A choice row:
  `tr.choice` (pointer, green VFO cell, tooltip "Listen to this
  (Programme: ...)" / "Select this (...)"), and `tr.sel` when its value is
  the option's value as the receiver last reported it (no optimistic
  mark; a second page follows the broadcast). A setting that is no row's
  key - dab's "" (first audio service) or a part of a name - marks no row:
  the page can't know which service it resolves to, and a wrong mark is
  worse than none; after a row click or a list pick it is a SId (the dab
  plugin marks the programme it decodes in a ▶ column of its own - a plain
  cell, *plugin-dab*). Rows
  without choices (older receiver, data services) as before. Test:
  scratchpad rowclick/t_rowclick.py. A row with details gets a ▸
  toggle (`.dig-det-tog`, `digDetToggle` - stopPropagation, so the row's
  own click isn't fired; open rows in `digDetOpen`, this page only): the
  details render as a row of their own under it (`tr.dig-det`, a
  label / value table in `.dig-det-box` - sticky left, `100cqw` of the
  `.dig-table-wrap` container, so a table wider than the panel doesn't
  push them out of view)
- Options (`dig::Options`): verbose ("Show all messages" - "Log all"
  until 2026-11-19), log (the logging mode, 0 Off / 1 Log / 2 Log All,
  2026-11-19 - *decoder-log.md*; a new VFO / decoder starts at Off), map
  ("Plot on map", default OFF - the
  user ticks it in the decoder tab; persisted as `&m=1`;
  `DIGITAL_OPT:id:map:0|1` also asks for a full map push) + `plugin`
  {"<id>.<key>": value} - the plugins' declared options,
  sent to the plugin as OPTION key=value ("" = back to its default). An
  option declared `hidden` (plugin list `"hidden":true`, 2026-11-17 - dab's
  Programme) gets no control in `digVfoRowHtml` at any stage; it is still
  saved and set by `DIGITAL_OPT` (a table row click, `digRowChoice`).
  `DIGITAL_OPT:id:verbose|log|pos_s|map|<id>.<key>:value` (log: 0 / 1 / 2 or
  off / log / all; old keys dmr_slot / p25_nac map to dmr.slot / p25.nac).
  Persisted as the snapshot field `v=1&l=1&ps=10&dmr.slot=1` (values
  percent-encoded; the old `v/slot/nac/inv` form still loads; without `l` -
  stored by an older version - the VFO gets Log, or Log All where the old
  log types had `raw`). The Invert option (a conj before the mixer, for an
  I/Q-swapped source; no use with the KrakenSDR) was removed 2026-11-19:
  a stored `i=` / the old form's 4th field is ignored, `DIGITAL_OPT:id:invert`
  is refused, the plugins' `kp::Host::rf_inverted()` is always false; the
  plugins' own video options (`analogtv.invert`, `tempest.invert`: black /
  white) are a different thing and stay. Both go to the page in `digital_opts` (decimator
  info) and the status' `opts`; a page that finds no `log` there talks to
  an older receiver. **What the plugin gets as `verbose`**
  (`kp::Host::verbose()`, the OPTION verbose=0|1, `Engine::send_settings`):
  the VFO's verbose OR (log = Log All AND "Allow logging to disk" on,
  `dig::set_log_allowed`) - so Log All writes the repeated messages, and
  they are on screen then too. The host can't keep "verbose-only" reports
  apart from the others (the wire EVENT carries a de-duplication time and
  the text, nothing else; plugins just don't send them without verbose), so
  writing them without showing them would need a plugin-API change. Told
  that Log All would otherwise be the only way to see them, the user asked
  for the separate tick: "Yes add a seperate show all messages checkbox
  too" (2026-11-19). Raw frames (OPTION log_raw=1,
  W_RAW accepted) only under Log All with logging allowed. The panel shows a plugin's options while it is selected, or
  in AUTO once AUTO detected it. An option whose two choices are exactly
  "0" / "1" is an on / off button (`digVfoRowHtml`, `.dig-opt-tog`: "● Label:
  On" highlighted / "○ Label: Off", the choice's text without a trailing
  "(...)", the help + "click to turn on / off" as its tooltip; it sends
  DIGITAL_OPT with the other value and shows the echoed state) - dtv's Path
  separation, adsb's Array combining / Overlap separation (its Bit repair
  is a three-way list since v2.0), dme's DoA per beacon, cells' Cell
  information; other choice lists stay `<select>`s. The settings rows
  (`#info-dec-top`) are rebuilt only while no input / list in them has the
  focus or a mode list is open (`ddOpenSel` - wherever the focus is: the
  list's filter field is in body, not in the rows); a focused BUTTON doesn't hold
  the rebuild back (a clicked button keeps the focus - the on / off button
  showed its new state only after a click elsewhere) and gets the focus
  back afterwards (the same button by position). Test: scratchpad
  e2e/t_dtv_toggle.py clicks with real mouse events (`.click()` doesn't
  focus, so the first test missed it)
- Run-time choices (`kp::Host::option_choices`, wire OPTION_CHOICES 42,
  2026-11-16 - the dab plugin's Programme list): key + value / label pairs
  for one of the plugin's DECLARED options (others ignored). The engine
  (`on_option_choices`) checks them again - it is a child process's text and
  a DAB label is from the air: at most 100 pairs read, a value the page
  couldn't set (`DIGITAL_OPT`: > 100 bytes, a control character) or a
  repeated one dropped, a label's control characters made spaces and cut to
  80 bytes at a character boundary ("" = the value) - and `Report` keeps
  them per `"<id>.<key>"` (32 KB for all lists of a decoder; an empty list
  removes them). Run-time state only, never persisted: cleared by
  `clear_all()` (retune, mode change), for a plugin when its process
  (re)starts or crashes and when the runner set is rebuilt. Status:
  `"choices":{"<id>.<key>":[[value, label],..]}` in every push (the facts
  are too; a list is small), keyed like `opts.plugin`. Page: `digDynChoices`
  -> `digVfoRowHtml` draws such an option as a `<select class="dig-opt-dyn">`
  of them (also when the declaration is a text field; no on / off button),
  every value / label through `digEsc`; a set value that is none of them
  gets an entry of its own first (`[v, v || 'Default']`), so the list
  never silently shows its first choice. Without `choices` (older receiver,
  no list yet) the declared list / text field as before. The focus rule
  above keeps an open / focused list from being rebuilt; the next 4 Hz push
  after it loses the focus redraws it. An older page ignores the key; an
  older receiver ignores the message type (an unknown type has always been
  dropped by `on_message`'s `default:` - only a payload > 1 MB is a
  protocol error); an older plugin sends none. The AI assistant's
  `get_decoder` returns the status, `choices` included (its note says so)
- UI (kraken_doa.html, "Digital decoders" block): any number of VFOs decode
  at once. Each VFO card (Decimators box, `updateDecimatorList`) has a
  "Digital decoder" tick (`digToggle` -> `digOnMode`: DIGITAL_MODE with the
  VFO's last mode other than Off as decimator_info reported it (`digLastOn`,
  this page, dropped when the VFO goes), else the mode picked in this
  browser (`digWanted`, per VFO in localStorage), else AUTO. Before
  2026-11 it was only the latter two, so a mode set elsewhere - a debug
  playback's entry, a saved frequency, another page, the AI assistant -
  came back as AUTO after an untick / tick, and a manual-only decoder (dab,
  adsb ...) silently stopped) and a mode list
  (`digSetModeFor`: picking a decoder also switches it on) plus a state line
  (`digRenderCards`, also run at the end of updateDecimatorList, which
  rebuilds the cards). The mode lists (VFO card + the tab's Mode) are
  two-line dropdowns: a native `<select>` draws options on one line, and
  the then "SFN Separation (DVB-T/T2, ISDB-T, ATSC 1.0/3.0, DTMB, DAB)" ran past the
  card. The `<select>` stays as the hidden model (value, options from
  `digModeOptions`, its onchange) and `ddEnhance(sel)` puts a button
  (`.dd-btn`) after it + a pop-up list (`.dd-pop`, in body, zoomed like the
  sidebar when the button is in it); an option "Name (details)"
  (`ddSplit`) shows the name with the details on a smaller second line, in
  the button and the list. The plugins are listed in GROUPS (the user's
  request 2026-10-23; `digGroups`, also the saved-frequency editor's
  decoder list): one `<optgroup>` - a heading in the pop-up - per
  `kp::Info::category` (`--info` / plugin list `category`, case ignored),
  the shipped groups in a fixed order (Two-way radio, Aviation, Marine &
  distress, Paging & packet, ISM & IoT, Telemetry & tracking, Broadcast &
  cellular), other categories after them by name, none = Other last, each
  group by name; only plugins without one (built before categories) = the
  old single "Decoders" group. Code that sets `sel.value` calls `ddSync(sel)`
  (`digFillSelect` and the tab rebuild do); `digFillSelect` leaves the value
  alone while that list is open (`ddOpenSel`). Keyboard: arrows / Home /
  End / Enter / Escape / Tab; a press elsewhere, a scroll or a resize closes
  it. The pop-up has a filter field on top (`.dd-q`, the user's request
  2026-11-16; `ddFilter`): the query is split into words and an option stays
  when every word is a case-insensitive substring of its name + details,
  group, description (`title`) and plugin id (`data-k`); groups left empty
  hide, nothing left = a muted "No matching decoder" row; options are hidden
  in place, not rebuilt. With a fine pointer (`(pointer: fine)`) the field
  takes the focus at open; touch-only devices don't (it would raise the
  on-screen keyboard at every open) - tap the field, and a character typed
  while the button has the focus goes into it. Arrows / Enter work on the
  visible options (highlight: the current value while the query is empty,
  else the first visible option whose name or id starts with the query,
  else the first with every word in its name / details / id (`data-n`),
  else the first visible - "dm" is DMR, not D-STAR above it through its
  description); Space types a space (on the closed button it still
  opens); Home / End jump only while the field is empty; keys typed in the
  field stop at the capture handler, so no page shortcut sees them. Width
  and side (below / above the button) are fixed at open; one above is
  placed by its `bottom`, so it stays on the button as the filter shrinks
  it (`ddPlace`). A resize that changes only the height while the field has
  the focus (a phone's on-screen keyboard) re-fits the list instead of
  closing it. Tab labels and state lines use `digShort(name)` (without the
  "(details)"; the tab's tooltip has the whole name). Test (scratchpad
  e2e/t_dd_ui.py, headless Chromium, the real plugin list; the filter:
  t_dd_filter.py - headless Chromium reports `(pointer: none)`, so it
  overrides matchMedia for the fine-pointer run and uses touch emulation
  for the touch run). The bottom panel has one tab per decoder MODE
  (plugin), not per VFO: `infoTab` = `dig:<plugin>` (the AI tab is gone -
  the assistant has its own dock; Voice to Text is no tab here either - it
  has the panel to itself, *voice-to-text.md*)
  (`infoPanelOpen('dec:<vfo>')` still works: the tab that VFO is in,
  `digTabOfVfo`). A VFO is in tab P while it runs P fixed or Auto detect
  locked onto P (`digVfoPlugin`, `digTabVfos`). There is NO Auto detect tab:
  a VFO in AUTO appears in the tab of the decoder it detected (its mode list
  there says Auto detect), and in none while it is still searching (its VFO
  card shows "Searching…"). Tabs only for the decoders in use, a list
  for the rest (2026-11-18; the user accepted the recommendation to
  "tame the decoder tabs" - the strip had a tab for every installed
  decoder, 28 on the test build, in a sideways-scrolling strip - and
  asked for it "with a list for the inactive ones"):
  - **Which decoders get a tab** (`digTabsInUse`): a VFO runs it now
    (fixed, or Auto detect locked onto it), or it has something to show -
    events not hidden by Clear (live VFOs, VFOs since switched, removed
    VFOs' `digArchive`) or incident rows. Those a VFO runs first, then the
    rest, each group by name; the tabs' look, state dot and "· D0 D2" are
    as before.
  - **All decoders** (`#dec-all-btn`, "All decoders (28)" - the count of
    installed plugins) after the tabs opens a popover in the View
    popover's pattern (`decAllOpen` / `decAllClose` / `decAllPlace`): under
    or above its button (the panel is low on the page, so usually above),
    inside the window, not modal; closed by Esc (focus back to the
    button), a press outside, the button again, a pick, a scroll of what
    holds the button, a window resize, the panel closing or switching to
    Transcription (the closing ways are the shared `popDismiss` since
    2026-11-18 - *sidebar.md*, *View button and popover*; the placement
    stays its own: it fits the list's height). A press on a help-mode `?` is no press outside, and in
    help mode a tap on a row shows its tip instead of picking it. On top a
    filter box (`#dec-all-q`, a combobox; name or id, case-insensitive),
    focused when the list is opened with a mouse or the keyboard, not on
    touch (no on-screen keyboard popping up); then every installed
    decoder (`digPlugins`, plus a decoder in use this receiver lacks)
    under "In use" (the tabs' decoders, in their order) and "Not running"
    (by name): a row (`role="option"`) with the tabs' state dot, the short
    name, the VFOs running it, and a plugin problem - "Not built",
    "Error", "Out of date" (`built` / `error` / `stale`, as in Setup
    Settings → Decoders), "Not installed"; the row's `title` has the whole
    name, the id, the state, the reason and the plugin's description.
    Arrow keys move the highlight (`aria-activedescendant`), Enter picks,
    typing filters. While filtering the popover keeps the height it opened
    with, so the filter box does not move. On a phone it is as wide as the
    screen allows, rows 42 px.
  - **A picked decoder that is not in use** is a tab while it is the shown
    one (`digTabList` = the decoders in use + `digTabShown`, at the end, in
    the idle look), so the strip always shows where you are; it leaves the
    strip when another tab is picked. Nothing is stored for it beyond the
    selected tab in `kraken_info_panel` (a reload restores it as the
    shown tab; an id no longer installed falls back). Its body is the idle
    one: "Not running" + "Start on D0 D1 ..." (`digSetModeFor`). The same
    holds for a shown decoder whose VFO stops elsewhere (another page,
    the assistant): the panel never leaves a shown decoder by itself; but unticking a VFO's
    decoder here (`digToggle` off) moves the panel off a tab left with
    nothing in it, to the first decoder in use or the empty state.
  - **Nothing in use**: the strip shows only All decoders, `infoTab` is
    `'dig:'` and the body says "No decoder is running" ("Auto detect:
    searching · D0" while a VFO in Auto detect has found nothing) + a `?`;
    as soon as a decoder is in use the panel shows it (`digRenderAll`).
  - The strip (`#info-tab-list`) still scrolls sideways when many are in
    use; All decoders stays visible right of it, the ✕ after it.
  - Test: `test_dectabs.py` in the job's scratch directory (headless
    Chromium: the empty strip, the list against `digPlugins`, groups,
    filter by name and id, keys, Esc / outside / pick, help mode, an idle
    pick as a tab, a VFO switched on / off, data keeping a tab - the ADS-B
    debug playback -, `dec:0` and reloads, Transcription, 1700 / 1300 /
    560-px-high windows, a 400 px phone with touch).
  The panel follows a VFO whose mode is picked
  (`digSetModeFor` / `digToggle` -> `infoPanelOpen('dec:<id>')`). A tab
  (`infoRenderDecoder`) shows: a settings row per VFO in it
  (`digVfoRowHtml`: coloured VFO + MHz + state, mode, Listen - only for
  voice decoders, `kp::Info::voice` (p25 dmr dstar nxdn ysf tetra; `digHasVoice`:
  in AUTO the detected plugin, else any ticked one) -, the plugin's options -
  in AUTO those of the detected plugin -, then `digLogCtlHtml`: **Logging**
  (a select: Off / Log / Log All - the user's words, 2026-11-19, in place
  of the "Log all" tick; greyed while logging to disk is not allowed: Log /
  Log All disabled options, the select showing Off whatever is stored, a
  small Decoder Logging button beside it, the tooltip naming the stored
  mode), **Positions every [N] s** (2026-11-19, only for a decoder whose
  plugin reports positions - `kp::Info::map`, `digReportsPositions`; the
  per-VFO option `pos_s`, `DIGITAL_OPT:<vfo>:pos_s:N`, 1-3600, default 10:
  the decoder log keeps at most one position per object in that time -
  greyed while the VFO's Logging is Off or logging is not allowed;
  *decoder-log.md*) and **Show all messages** (the verbose tick, "Log all" until then;
  ticked and disabled while Log All is in effect, its tooltip saying why;
  the stored value comes back after), then Plot on map,
  bandwidth / plugin problems; an older receiver - no `log` in the options
  - gets the tick only), a decoder log problem (`digLogWarnHtml(ids)`,
  one line under the rows while logging is on and not writing / dropping /
  the disk is nearly full and one of the tab's VFOs logs, with a button to
  Setup Settings' Decoder Logging - *decoder-log.md*), the incident row (messages plugins), the
  tables (below), per VFO a status + facts block (`digFactsHtml(id, tab)`:
  only that plugin's facts / frames), the first
  plugin stderr, and ONE event log merged from every VFO (`digTabEvents`:
  events whose `src` is the plugin - from VFOs in it, VFOs since switched
  to another mode, and removed VFOs (`digArchive`, this page only, marked
  "D3†") - plus the decoder's own lines (no src) of the VFOs in the tab since
  their last "Decoder set to / on"; each line tagged with its VFO in the
  VFO's colour). Clear (`digClearTab`) hides the tab's lines up to now
  (`kraken_dig_cleared`) - the decoders keep their logs (another mode's tab
  still needs them) ⚙ Setup Settings → Decoders
  (a sidebar box, "🔐 Digital Decoders", until 2026-11-17 - *sidebar.md*,
  "Setup Settings dialog") shows the plugin list (`aiRenderPlugins` -> `#ai-plugins`:
  Auto detect tick, a "Trunk following" tag on the plugins whose list entry
  has `trunking` (2026-11-20; *trunk-following.md*), ↻ = `PLUGINS_RESCAN` after a manual copy + make; no
  export / import / rebuild in the web UI - plugins move as folders + `make`;
  a plugin is put on a VFO with the VFO card's decoder list; the
  heading shows the count, the Decoders tab a ⚠ count of plugins with an
  error / stale / unbuilt - `#set-tab-dec-n`) and the
  voice codecs (`digOnCodecs`): IMBE / AMBE
  (mbelib) / ACELP (ETSI TETRA) installed or not, with the README's install
  steps for a missing one. Source: `plugins/lib/build/codecs` (lib/tools/
  codecs.cpp, built by plugins/Makefile) - the SAME MbeLib / TetraCodec
  detection the plugins use, run by kraken_doa with its environment
  (KRAKEN_MBELIB, KRAKEN_TETRA_CODEC_DIR, PATH) via
  `dig::codec_status_message()` on a worker thread, on AI_STATE (page
  connect) and `PLUGIN_CODECS` ("↻ Check codecs") -> `{"codecs":{checked,
  imbe_ok, imbe, ambe_ok, ambe, acelp_ok, acelp}}`. Running plugins dlopen
  mbelib once, so a newly installed codec needs the decoder switched off and
  on
  `DIGITAL_HISTORY:id` is asked once per VFO / mode. Demod "Digital" keeps the
  VFO's bandwidth (widened only below the decoder's need - it used to force
  NBFM's 16 kHz, too narrow for TETRA / AUTO)
- Bandwidth: each plugin's `min_vfo_rate` (12 kHz for the 4FSK ones, 24 kHz
  for TETRA; AUTO = the largest of the ticked set) - the panel warns and
  offers the narrowest wide-enough option
- CPU (Pi 5, 24 kHz VFO): one plugin ~3% of a core, AUTO ~11% with the six
  protocol plugins and ~15% with all nine shipped/user plugins ticked (each
  process has its own front end; was ~4.5% in-process) - unticking unused
  decoders saves it
- Offline test harness: `tools/digi_test.cpp` (runs the plugins like a VFO,
  synchronously: waits for slow plugins instead of dropping, SYNC-drains at
  the end; .dis files are FM-modulated again; `--opt dmr.slot=1`; run from
  kraken_doa_v2): `g++ -std=c++20 -O2 -Iinclude -isystem
  liquid-dsp/build-kraken/include tools/digi_test.cpp src/digital/*.cpp
  src/utils/process_posix.cpp src/utils/fs_compat.cpp src/ai_sandbox.cpp
  src/geo_address.cpp liquid-dsp/build-kraken/libliquid.a` (liquid built by `make` first - see
  *liquid-dsp*; the plugin processes are kproc's, `utils/process.hpp`; the
  Windows command line is in the file's header)
- Validated on live P25 (VFO on a trunked control channel + its voice
  channel) and DMR Tier III here, and on recordings for TETRA, D-STAR, DMR,
  NXDN, MPT1327 and P25 (regression of 33 checks incl. voice passes through
  the plugins)

