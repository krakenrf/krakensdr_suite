KrakenSDR V2 software. The new V2 software moves away from Python and implements everything in C++, which has resulted in massive speed improvements on slow hardware like the Pi 4 and Pi 5.

We are now able to have a real time spectrum and audio demodulation at the same time that the MUSIC DoA is run.

The **Mode** selector in the top bar of the KrakenSDR DOA page picks how the receiver's tuners are used:

- **Coherent** (default): all tuners work as one phase-calibrated antenna array - MUSIC DoA, beamforming, the web mapper and DoA logging.
- **Wideband**: the tuners sit side by side for one wide stitched spectrum (about 5 x 1.9 MHz on a KrakenSDR); the discrete scanner runs in this mode. No DoA.
- **Independent**: every tuner is its own receiver with its own frequency and gain - the page shows one spectrum + waterfall per tuner in a grid. Each pane has its own frequency digits (click the top / bottom half of a digit, or use the mouse wheel on it) and gain slider - the top bar's frequency and gain are hidden in this mode, and put VFOs (demodulation, digital decoders, the AI Signal Lab) on any pane: drag a pane's spectrum to tune that tuner (like the main display), drag a VFO bar to move it, click a pane to move the selected VFO there, or pick the tuner on the VFO's card. No DoA, and no calibration runs (the noise source stays off).

In every mode the spectrum zooms with the mouse wheel (or the Zoom slider under the waterfall, up to 16x; in Independent mode the pane under the cursor / the selected pane), and the bar between the spectrum and the waterfall can be dragged to change the split (in Independent mode it moves every pane at once; double-click for the default).

The DoA panel and the DoA / coherent-only sidebar sections are hidden outside Coherent mode. The mode and Independent mode's per-tuner tuning are remembered across restarts. Switching back to Coherent retunes every tuner to the common frequency and runs a phase recalibration. (The KrakenSDR Wideband hardware variant only has Coherent mode: its tuners are fixed at the IF.)

Any number of VFOs can decode digital radio signals at once - P25 Phase 1, DMR, TETRA, D-STAR, NXDN, MPT1327 (analogue trunking control channels), POCSAG pagers, APRS, ADS-B aircraft (1090 MHz), AIS ships (161.975 / 162.025 MHz), ISM band devices (315 / 345 / 433 / 868 / 915 MHz weather stations, tyre pressure sensors, key fobs, alarm sensors, energy meters), radiosondes (weather balloons, 400-406 MHz), DME / TACAN beacons (962-1213 MHz: Morse ident, channel and its VOR / ILS pairing, the beacon's exact frequency, TACAN bearing, aircraft interrogations - and a DoA per beacon on the map), pulse radars (airport / en-route surveillance radars on 1215-1400 MHz and other pulsed radars: pulse width, chirp, PRF and PRI pattern, antenna rotation, beam width - and a DoA per radar) and the VHF / UHF navigation aids (ILS localizer and glide slope with the course / glide path deviation, VOR radial, marker beacons, their Morse idents) - with automatic mode detection: tick "Digital decoder" on a VFO in the Decimators box and pick "Auto detect" or a decoder. Each decoder mode gets its own tab in the panel under the waterfall ("🔐 Decoders" button, or ⤢ on the VFO), showing every VFO that runs it: a settings row per VFO, the decoded data (tables with VFO and frequency columns), and one event log of all of them. There is always a tab for every installed decoder (an unused one offers to start it on a VFO); when a VFO is switched to another mode or removed, what it received stays in the old mode's tab; the sidebar "Digital Decoders" box lists the installed voice codecs and decoder plugins. It shows network/site identities, talkgroups, radio IDs, callsigns, messages and call activity, and can play the voice (P25 built in; DMR, D-STAR, NXDN and TETRA with codecs you install yourself - see "Digital voice codecs" below). Every decoder is a plugin, so new ones can be added.

**Radiosondes**: the Radiosonde decoder follows weather balloons on 400-406 MHz - Vaisala RS41 (-SG, -SGP; -SGM is encrypted), Graw DFM-06/09/17, Meteomodem M10 and M20, InterMet iMet-4 and iMet-54, Lockheed Martin LMS6-403 and Meteo-Radiy MRZ, all at once (whichever type is on the frequency is shown). Put a 24-50 kHz VFO on the sonde (most stations launch around 23:15 and 11:15 UTC; an RS41 also tells you the frequency it is set to) and pick "Radiosonde" or Auto detect. Its tab shows everything the sonde reports: position, altitude, climb rate, temperature, humidity, dew point, pressure (measured, or estimated from the altitude), wind (from the balloon's drift), battery, burst / kill timers and more, a table of all sondes heard, and an event log that turns the ascent into a sounding - each standard pressure level (850, 700, 500 hPa ...) with its height, temperature, humidity, dew point and wind - plus the burst and the landing. With "🗺 Plot on map" the sonde is a balloon on the map with its whole flight track; follow it to where it lands. Not decoded: RS92, the 1680 MHz sondes, Meisei, MTS01.

The **🗺 Map** button (under the waterfall) opens a street / satellite map in the right-hand pane - in Coherent mode as a tab next to MUSIC DoA, in the other modes on its own. It shows the positions the decoders report - ADS-B aircraft, AIS ships (and base stations, aids to navigation), APRS stations and objects, DMR radios sending GPS, D-STAR GPS / DPRS - with their tracks; click a marker for its details (callsign, altitude, speed, squawk, distance and bearing from the station). A decoder's positions appear once you tick "🗺 Plot on map" in its tab under the waterfall (off by default; remembered per VFO). The ADS-B tab also lists every aircraft in a table with all its decoded data (sort by any column, click a row to show the aircraft on the map). The Listen button only appears for decoders that carry voice.

**Your own markers**: right-click the map (on a phone or tablet: press and hold) and choose "📍 Add Marker" to pin a place - a repeater site, a transmitter you found, a fox-hunt start. Give it a name, frequencies (each with an optional label; "+ Selected VFO" adds the frequency the selected VFO is on) and notes, then Save. Click a marker to see its details at the bottom right of the map (position, distance and bearing from the station, frequencies, notes); click one of its frequencies to tune the selected VFO there - inside the band shown the VFO just moves, otherwise the receiver retunes with the VFO in the middle. Right-click a marker to edit or delete it, or use the buttons in its panel. Markers are saved on the receiver (`kraken_doa_v2/map_markers.json`), so every browser sees the same ones and they survive a restart.

**Masts from OpenStreetMap**: the map's "Masts" button shows the radio masts, towers and antennas mapped in OpenStreetMap. They are loaded online, by your browser, from OpenStreetMap's Overpass server: first everything within about 150 km of the station (Station Information) in one go, then other areas you zoom in on. Your browser keeps them for a week, so they show at any zoom and without internet; the server is often busy and limits how often one connection may ask, so new areas can take a few seconds or a retry to appear (the status line at the bottom left says so). The colour says what OpenStreetMap lists the site for - broadcast (radio / TV), mobile phone, microwave link, amateur radio, or not stated. Click one for everything OpenStreetMap knows about it: type and construction, what it's used for (e.g. "Mobile phone (LTE, UMTS)"), operator, height, and its other tags, with a link to it on openstreetmap.org. "📍 Add as marker" turns it into one of your own markers, so you can add its frequencies and notes. OpenStreetMap is mapped by volunteers, so coverage varies and it has no frequencies: for those, see your country's radio licence register.

**Viewshed (line of sight)**: the map's "Viewshed" button shows which ground is in line of sight of the station's antenna, over the terrain, anywhere in the world. Line of sight is always worked out from the station's position (other antennas are placed in Signal mode). It has its own antenna height above the ground and radius (up to 1000 km - aircraft at 10 km drop below the horizon at about 450 km, weather balloons at 30-35 km at about 750-800 km). The target height says how high the other antenna is - above the ground (2 m for a handheld or a car), or above sea level for aircraft (e.g. 3000 m) - and changes the picture at once. Point at the map (tap on a touch screen) to see whether a place is in sight from each viewshed, and if not, how high an antenna there would need to be. The station's viewshed follows the station when it moves; a viewshed's point can be moved by clicking it (a white ring shows it is selected) and dragging it. Earth curvature is included, with the usual radio refraction (k = 4/3; light or none can be picked for the line of sight - the Signal mode always uses radio refraction, as its propagation models are for radio waves), and every line of sight follows the great circle, as the real one does. The terrain comes from the AWS Terrain Tiles (SRTM and national elevation models), loaded online by your browser: in the finest detail around the antenna (about 8 m), where a nearby hill decides most of what is hidden, and coarser further out (about 60 m at 30 km, 1 km at 300 km, 4 km at 1000 km), so every distance is seen at about the same angular detail. A 30 km viewshed downloads about 8 MB the first time, 300 km about 15 MB, 1000 km about 17 MB, then it works from the browser's cache; working it out takes a few seconds. Moving a viewshed's point (or changing its antenna height) reuses the terrain already loaded and cleaned, and only loads the tiles it hasn't got yet, so an update takes a couple of seconds even at 1000 km. Known errors in that data (e.g. a strip of impossible "mountains" in the sea north of the Coromandel) are removed. It is the bare terrain: buildings and trees are not included, and the sea counts as 0 m. Radio signals also reach somewhat past the line of sight, so treat it as a guide to where signals are clearly blocked, not a coverage prediction - for that, switch the viewshed to its Signal mode (below).

**Viewshed Signal mode (path loss: coverage and link budget, also without line of sight)**: the viewshed panel switches between "👁 Line of sight" (the viewshed above, unchanged) and "📶 Signal", which works out how often the signal gets through between each viewshed point (site) and a radio at the target height - also where there is no line of sight, as radio bends over hills (diffraction) and scatters off the atmosphere far beyond the horizon. The map is coloured by that reliability (a smooth gradient: dark green at 99 % or better, through light green at 90 %, yellow at 75 % and orange at 50 % to red at 0 % - every place within the radius gets a colour; below the reliability you want it fades out), over the whole radius - not only where the line of sight reaches -, and a black line can mark the edge of the line of sight (tick "Line of sight edge", next to the wanted reliability; off by default). The map shows where a transmitter's signal gets through, for receivers at the other antenna's height. The station isn't part of Signal mode: right-click the map and pick "📶 Add TX antenna here" to place each transmitter (TX 1, TX 2, ...); each one's coverage is coloured. Select an antenna and press Delete to remove it. The target height can be above the ground or above sea level (an aircraft, a weather balloon, a drone). In both modes the panel calls each site's antenna and the other end's "TX antenna" or "RX antenna" by these roles (line of sight works both ways, so there the role only names the ends). **Polarization**: set the TX and RX antennas' polarization (vertical, horizontal, circular, or a custom angle from horizontal); a mismatch costs signal - two linear antennas lose cos² of the angle between them (crossed: ~20 dB with a direct path), linear to circular 3 dB - but less where an antenna stands low among clutter, since scattering off buildings and trees mixes the polarizations (crossed antennas lose ~9 dB with a receiver at 2 m in the suburbs, ~6 dB in a city, the full ~20 dB over open ground or sea or above the clutter). The path itself keeps the polarization, so the mismatch is the same everywhere on the map - it doesn't change at the horizon. The panel shows the mismatch, the map and the link budget include it, and the propagation models use the TX antenna's horizontal or vertical (circular counts as vertical). With several transmitters shown, every place takes the best of them. The panel says "% covered" for each TX site (the area that reaches the reliability you want; an RX site's row shows its link's transmitter), and pointing at the map shows, per transmitter, the reliability there, the received level and the path loss. Power, antennas, receiver, fading and the wanted reliability repaint at once; the frequency, clutter, the TX antenna's polarization, a site's position or height, or the target height work it out again (a 30 km site takes some seconds to half a minute in the browser, 300 km longer). Once a site's signal is worked out, switching between the two modes needs no new calculation (the line of sight is the same in both). The calculation runs in your browser, spread over its CPU cores (one less than it has, up to 8 in all), so a desktop or laptop is much faster than a phone or the Pi's own browser. The map uses the model picked under the transmitter power: ITU-R P.1812 (the default) or Longley-Rice / ITM, both worked out along the terrain to every place, or free space (no terrain) - Longley-Rice is much slower: it takes several times as long as P.1812. **Path loss analysis**: right-click the map (long press on a touch screen) and pick "📶 Path loss analysis from TX 1 to here" - a receiver there, at the other antenna's height, linked to the selected TX antenna (else TX 1; the item appears once a TX antenna is placed): the line is coloured by how good the link is, and a strip at the bottom of the map shows the terrain profile between them (with the Earth's bulge, the direct line, the first Fresnel zone and - beyond the horizon - the ridge that decides it), the loss by four models side by side - ITU-R P.1812 (the ITU's terrain method: diffraction by "delta-Bullington" plus troposcatter), Longley-Rice / ITM (the NTIA model most coverage tools use) and free space - and the link budget: the received level (dBm and µV), the signal-to-noise ratio, the margin over what the receiver needs, and how often (%) the link works once fading is counted in. Markers and OpenStreetMap masts have the same buttons ("📶 Path loss from TX 1", "📶 Add TX antenna" / "👁 Line of sight"). The far end can be clicked (a white ring shows it is selected) and dragged; click a model's row to base the budget and the map on it; ✕ removes the link. Enter the frequency and transmitter power in the Signal settings; "Antennas, receiver, channel" opens the rest: antenna gains and cable losses, the receiver's bandwidth, noise figure and the signal-to-noise ratio it needs (this gives its sensitivity), the clutter around the transmitter and the receiver (none, water, open, suburban, urban / forest, dense urban - ITU-R P.2108: a low antenna among buildings or trees loses up to ~20 dB; with the other end in the air it depends on how high that is in the sky), the fading (auto: Rician, its K-factor the one you set where the path is clear, falling smoothly where terrain blocks the path and towards Rayleigh far beyond the horizon where troposcatter carries the signal - the Earth's bulge over open sea doesn't lower it; or a fixed Rician K, Rayleigh, or none for a fixed link between masts), and the location variability; "Wanted reliability" (e.g. 90 % of times and places) sets what counts as covered. All of it is the median path loss (50 % of the time; ducting and other rare propagation are left out) over the bare terrain - buildings and trees count only through the clutter setting -, so expect real signals to scatter several dB around it. The models were checked against their published references: Longley-Rice against NTIA's own test cases and C++ code, P.1812 against the ITU's validation data, P.2108 against its validation table (`kraken_doa_v2/tools/pathloss_validate.mjs`).

**Incident map (POCSAG)**: street addresses in POCSAG text pages ("STRUC1 SMOKE ISSUING 12 QUEEN ST AUCKLAND CENTRAL...") become incidents: the address is found in the text (a house number, street name and type such as ST / RD / AVE; a suburb or town and a cross street named in the message help pick the right one) and looked up online in OpenStreetMap (Nominatim) within a distance of the station - set it in the POCSAG tab (default 300 km; the station location comes from Station Information). Incidents are listed in the POCSAG tab (VFO and frequency, time, address, area, how exactly it was located, the message; the same address paged again - also on another VFO, e.g. fire and ambulance pagers - counts as one incident) and shown as red markers on the 🗺 Map when "Plot on map" is ticked; they are kept 24 hours. The lookup needs internet: without it the pages wait and are tried again every minute for half an hour. It reads addresses in the usual forms of Latin-script countries: "12 Queen St", "123 N Main St", "350 5th Ave" (English-speaking countries), "55 rue du Faubourg Saint-Honoré", "Calle Mayor 1", "Via del Corso 100", "Rua Augusta 100", "ul. Marszałkowska 10" (type before the name), "Friedrichstr. 43", "Prinsengracht 263", "Drottninggatan 50", "Karl Johans gate 22", "Váci utca 1" (German, Dutch, Nordic, Hungarian), with the house number before or after. Not found: other scripts (Cyrillic, Greek, Arabic, Asian), route numbers (SH1, I-95), landmarks, and names the page abbreviates differently from OpenStreetMap. The station (Station Information in the sidebar) is drawn with range rings. To move it on the map, click it (it turns cyan, in a ring) and drag it; the line from it shows its heading - drag the round handle at its end to turn it. Or right-click (long press) anywhere and pick "📡 Set station here". This sets a fixed (static) location; if the station was following the GPS, you are asked first. Map tiles come from OpenStreetMap (street) and Esri World Imagery (satellite) and need an internet connection; without one the map still works on a plain latitude / longitude grid, with a notice. Drag to pan, mouse wheel or +/− to zoom; drag the pane's left edge to make it wider.

**Decoder logging**: sidebar → 🗂 Decoder Logging saves what the digital decoders report to disk: events (each decoder's log lines), text messages (pager texts), positions (aircraft, APRS stations, DMR / D-STAR GPS - each object at most every N seconds), raw frames (ADS-B Mode S messages, APRS packets, POCSAG codewords) and incidents - tick the types you want. One file per day, `decoders-YYYY-MM-DD.jsonl` (one JSON record per line: time, VFO, frequency, decoder, type and the data); at midnight the finished day is gzipped, and days older than "Keep" (default 7, 0 = forever) are deleted - only these files, nothing else in the folder. To spare the SD card the records are collected in memory and written every 5 seconds; writing pauses when less than 100 MB is free. The box shows the free space on the drive the logs go to, today's size and an estimate per day, and lets you pick the folder: the default is `decoder_logs` in kraken_doa's folder, or pick a USB drive / other mounted disk from the list. Raw ADS-B frames near a busy airport are roughly 100-300 MB a day before gzip. Positions are logged with the decoder's details (for aircraft: squawk, vertical rate, speeds, category, emergency status). Anything unusual is logged as an event starting with ⚠ - for ADS-B: emergencies (7500 / 7600 / 7700) and when they end, squawk changes, the special squawks 7400 / 7777 / 0000, unusual aircraft types (UAV, balloon, parachutist, ultralight, high performance, space vehicle), climbs or descents of 6000 ft/min or more, and 400 kt or faster below 10000 ft; pilot IDENTs are logged too.

**Voice to Text**: tick "🗣 Transcribe" on a VFO card and what is said on that VFO is written down - by a free speech-to-text model (OpenAI's Whisper, via whisper.cpp) running on the receiver itself: no account, no internet once it is set up, nothing sent anywhere (set it up once, see "Voice to Text" below). Any number of VFOs at once, whichever one you listen to. Analog voice is demodulated with the VFO's demod (NBFM, AM - e.g. airband -, WBFM); a VFO with its digital decoder on gives the decoded voice instead (P25, DMR, D-STAR, NXDN, TETRA - the voice codec must be installed, see "Digital voice codecs"), with the radio's ID and talkgroup. Each transmission is cut out by the VFO's squelch (with the squelch off: while a signal stands 10 dB over the noise) or by the digital decoder's calls and talker changes, and appears in the **🗣 Voice** tab under the waterfall a few seconds after it ends (one longer than 25 s in parts, cut where the speech pauses; broadcast FM - or anything on air longer than 30 s - in pieces of about 10 s, so its text follows the station): time, VFO, frequency, mode, who (digital radios), the bearing during the transmission (coherent mode with DoA on - click it to see the transmission's DoA lobe on the map), its length, the text, and ▶ to play the audio and check it (compressed, about 7 MB per hour of speech, and kept as long as Decoder Logging keeps its files). Search the transcripts, show one VFO's, or download them as a CSV file; with Decoder Logging on (type "Voice transcripts") they also go to the daily log files. Noise, data bursts and tones are skipped (voice activity detection), so an open squelch doesn't fill the log with invented words. The 🗣 Voice to Text sidebar box picks the model (base.en by default: on a Pi 5 it transcribes about 2-3 times faster than real time on 2-3 CPU cores; small.en is more accurate but slower than real time; models without ".en" do other languages, and can translate them to English), the CPU cores it may use (it runs at a lower priority than the receiver), and "Words to expect" - callsigns, unit and place names spelled as you want them, which helps the model with names it would otherwise mishear. Expect radio-quality transcripts: clear speech comes out well, weak or clipped signals and unusual names less so - the audio is kept so you can check.

**Direction finding on the map (coherent mode)**: the 🗺 Map shows each VFO's live DoA lobe around the station, turned to true north with the station's heading, and the bearing line. While you drive, it builds a heat map of where the transmitter is: bearings are taken every 15-60 m of driving (none while stopped or turning), each is drawn as a faint line from where it was taken, and together they give the most likely transmitter position with its 95 % uncertainty ellipse ("TX ±120 m"). The right pane's "⊞ Both" tab shows the MUSIC DoA plots and the map at once (drag the divider between them). Use the "📡 DF" panel at the bottom left of the map to pick the VFO (or All), switch Lobe / Lines / Heat, set the grid range (km around where you started; the panel warns when the transmitter seems to be outside it), centre on the estimate, or reset. It needs the station position and heading: Station Information → GPS (gpsd; the GPS course is used above 7 km/h, or a compass if gpsd has one); the array's ANT0 must face the front of the vehicle. The receiver also measures whether all bearings are rotated (array mounted a few degrees off) and says so. Retuning a VFO starts a new heat map; the bearings are saved and picked up again after a restart on the same frequency (24 h).

**Direction finding per radio (P25, DMR, NXDN, D-STAR)**: with a VFO's digital decoder on P25, DMR, NXDN or D-STAR, the receiver knows which radio (unit / radio ID, or the D-STAR callsign) is transmitting and when, and cuts the VFO's signal at those points: each radio's bearing is computed only from its own transmissions, so several radios sharing one channel no longer blur into one bearing. In the map's "📡 DF" panel, pick a radio from the list (or the ID selector): you see its latest bearing (plus earlier transmissions as faint lines) and, while driving, its own heat map and position estimate. "Every radio" shows all their bearings at once, labelled with their IDs; "Whole signal" is the plain VFO view. Needs the coherent mode with DoA on; works on encrypted calls too, since the IDs are sent in the clear. On a repeater's output every radio's bearing is the repeater's own (DMR and NXDN labels say "via repeater") - listen on the repeater's input frequency, or to direct (simplex) traffic, to find the radios. **Every decoder whose signal names its transmitter does this ("ID separation")** - besides the radios above: **APRS** stations (each frame by the station that actually transmitted it - the sender, or the digipeater that repeated it), **radiosondes** (each sonde by its serial number, so two sondes on one frequency - a new launch and yesterday's still sending from the ground - keep their own bearings), **TETRA** base stations (each cell by its network and colour code, so cells sharing a frequency elsewhere in the network are told apart), **MPT1327** sites (the control channel's system identity), **VOR / ILS** navaids (their Morse ident), **pulse radars** (each by its signature - its pulses and their timing - with the bearing taken only from its main beam pointing at you), DME beacons, ISM devices, and **ADS-B and AIS**: with the ADS-B or AIS decoder on a VFO, every aircraft / ship (also AIS base stations and aids to navigation) gets its own lobe from its own packets ("Every aircraft" / "Every ship", or pick one in the list); with "🗺 Plot on map" ticked in the decoder's tab the panel also shows how far each DF bearing is from the bearing to its reported position, and the median over all of them ("Position check") - a handy test of the array and its heading/mounting. AIS: picking the decoder tunes a 100 kHz VFO to 162.000 MHz, which holds both AIS channels; a ship's packets are filtered to their own channel before the DoA, so two ships transmitting at once on the two channels don't mix. Each of these shows in the DF panel's list with its own icon (📻 APRS station, 🎈 sonde, 🗼 site, 🧭 navaid, 📡 beacon / radar, 📟 device, ✈, ⛴). Plugins provide it through the plugin API (`host.talker`; `kraken_doa_v2/plugins/SDK.md`, section "ID separation" - required for every new decoder of a signal with transmitter IDs, with a test at known bearings: `plugins/sdk/tools/uca_scene.py` and the decoders' own test-signal generators in `plugins/<id>/tools/`).

**Array calibration from aircraft (experimental - sidebar → 🧪 Experimental → ✈ Array Calibration, coherent mode)**: the noise source calibrates the receivers inside the KrakenSDR, but not what is outside - cables of slightly different length, antennas that aren't exactly where the settings say, an array turned a little on its mast. ADS-B aircraft report where they are, so each of their messages is a transmitter at a known direction. With the ADS-B decoder running on a VFO (DoA on) and the station location and heading set (a static location, or GPS with a compass; the array standing still), press **Collect** and leave it while aircraft come from many directions. Every 20 s the receiver fits each antenna's cable delay and position error and the array's rotation, and checks the result on aircraft it wasn't fitted on (their bearing error without and with the calibration). **Apply** uses it for MUSIC and beamforming on every frequency: delays and positions are physical, so they carry over. Differences between the antennas themselves (and multipath) don't - they show as "Unexpl." per antenna. The calibration is saved (`array_cal.json`) and only used while the array settings (elements, radius / positions) are the ones it was made for. Packets from aircraft nearer than 5 km, below 2° elevation (aircraft on the ground or near the horizon, whose signal often only arrives reflected) or above 60° are left out, and so is any aircraft whose bearing disagrees with its reported position by more than 25° (a blocked or reflected path - the result lists them).

**Digital squelch**: on a VFO with its digital decoder on, the squelch can be set to **Digital** (VFO card → Squelch → "Digital (decoded frames)"): it opens only while the decoder receives frames that pass their checks (and for 1.5 s after the last), and the VFO's bearing is computed from exactly those frames' samples - so another transmitter, noise or interference on the same channel between the frames never gets into the bearing, the map lobe, the logs or the web mapper. No level to set. It works with every shipped decoder; in Independent / Wideband mode it gates the audio. (The eigenvalue squelch methods were removed; such saved settings use FFT Peak.)

**Wideband signals (DVB-T, LTE) on the spectrum**: the spectrum, the FFT squelch and the scanner measure levels from the receiver's noise floor. A signal wider than the 2.4 MHz the receiver sees fills the whole band, so there is no noise left to measure - it used to be drawn at almost 0 dB (only its edge showed its real height) and a squelch inside it never opened. The receiver now remembers its own noise floor for each tuner and gain (in `noise_floors.json`, kept across restarts) and uses that while a signal fills the band, so DVB-T or LTE shows at its real height above the noise. In Coherent mode it tells such a signal from a genuine rise of the noise (an active antenna, interference) by comparing two antennas - noise differs between them, a signal doesn't. The receiver has to have seen ordinary noise once at that gain (any normal frequency will do); in Independent / Wideband mode, where it can't compare antennas, a band-filling signal slowly sinks by about 1 dB per minute.

**ADS-B**: pick "ADS-B (1090 MHz)" as a VFO's digital decoder - that sets the VFO to 2.4 MHz bandwidth and tunes it and its tuner to 1090.000 MHz (in Coherent mode the whole array, in Independent mode just that VFO's tuner); the VFO is then shown as a single line at the centre of the spectrum. ADS-B is never part of Auto detect. Set the station location for distances and range checks. It needs a 1090 MHz antenna on the VFO's channel - in Coherent mode the CH selector's antenna; keep beamforming off for ADS-B (the beamformer follows one direction, the aircraft are all around). Independent mode lets one tuner sit on 1090 MHz while the others do something else.

**ISM band devices (433 MHz etc.)**: pick "ISM devices (433 MHz etc.)" as a VFO's digital decoder and tune the receiver to the band - 433.92, 868.3, 915, 315 or 345 MHz. The VFO moves to the tuner's centre with the full 2.4 MHz bandwidth (drawn as a single line that can't be moved or resized, like ADS-B) and the decoder covers the whole 2.4 MHz - 433.92 MHz covers 432.72-435.12 MHz - decoding the licence-free devices in it: weather stations and thermometers (Acurite, LaCrosse, Oregon Scientific, Fine Offset / Ecowitt / Ambient Weather, Bresser, Hideki / TFA, Nexus, Rubicson, Prologue, inFactory, ThermoPro...), tyre pressure sensors (Toyota, Ford, Citroen / Peugeot, Renault, Hyundai / Kia, Schrader, BMW, Nissan, Steelmate, Jansite...), key fobs and remotes (KeeLoq garage / car remotes, Ford, Somfy RTS, KlikAanKlikUit / Nexa, EV1527 / PT2262 fixed-code fobs and sensors), alarm sensors (Honeywell 5800 / 2GIG, Interlogix, DSC, Kerui, Chuango, smoke and motion sensors) and energy meters (Efergy, CurrentCost) - about 140 device models (69 protocol decoders), OOK and FSK. Its tab shows a table of the devices heard (model, id, reading - temperature, humidity, wind, rain, tyre pressure, button / door state -, battery, the device's own frequency, signal) and logs new devices, button presses, alarms and low batteries. **Every device gets its own DoA**: each transmission is cut out of the band at the device's own frequency and its bearing computed from just that, so in the map's "📡 DF" panel you can pick one sensor or key fob and see its bearing - one per transmission, or averaged over 1 / 10 minutes for a fixed sensor (decoder option "DoA per device"). Devices transmitting at the same moment on different frequencies are kept apart. Signals no protocol knows show up as "Unknown" devices when they repeat a code, with a DoA of their own - handy for hunting an unidentified transmitter.

**Pulse radars (L-band ATC / surveillance)**: pick "Pulse radar (L-band ATC / surveillance)" as a VFO's digital decoder and tune the receiver to the radar (airport and en-route surveillance radars use 1215-1400 MHz; wind profilers and other pulsed radars work too). Like the ISM decoder it takes the whole 2.4 MHz the tuner receives. It measures what every radar in the band sends, like a radar intercept receiver: its pulses (width, frequency, a chirp's sweep - modern solid-state radars send long chirped pulses, often with a short pulse on another frequency), how often (PRF) and in which pattern (constant, staggered, switching between PRIs in blocks, jittered, alternating PRI sets), how fast its antenna turns and how wide its beam is. Its tab lists the radars (open a row for all the details) and plots the strongest one's pulse level over one turn of its antenna - the main beam pointing at you, its sidelobes, and the reflections it lights. **Every radar gets its own DoA** in the map's "📡 DF" panel, taken only from its main beam passing over you (the strongest, direct signal - not the reflections). A nearby radar's main beam is strong enough to overload the receiver at normal gain: the decoder says so, and still takes the bearing from the edges of the beam; a lower gain also shows the beam's width. See `kraken_doa_v2/plugins/radar/README.md`.

**SSR / Mode S / TCAS interrogations (1030 MHz)**: pick "SSR / Mode S / TCAS interrogations (1030 MHz)" as a VFO's digital decoder - like ADS-B it sets the VFO to 2.4 MHz and tunes to 1030.000 MHz. This is the other half of ADS-B / Mode S: the questions that secondary surveillance radars, airport multilateration systems and aircraft collision-avoidance systems (TCAS) send, which the 1090 MHz replies answer. It reads every interrogation - Mode A / C with their side-lobe suppression and all-call pulses, TCAS whisper-shout sequences, and the Mode S messages themselves (even at 2.4 MHz, where each bit is shorter than a sample: they are read as a whole sequence against the receiver's measured response): whom each one asks (the aircraft's address), the radar's interrogator code (II / SI), a TCAS aircraft's own address. It tells the interrogators apart - a rotating radar by its pulse train and code, a fixed multilateration station by its steady level, a TCAS by its whisper-shout and broadcasts - and shows for each its PRF and pattern, how fast its antenna turns and how wide its beam is. **Every interrogator gets its own DoA** in the map's "📡 DF" panel (🛰 ground, ✈ aircraft) - a radar's only from its main beam passing over you. See `kraken_doa_v2/plugins/ssr1030/README.md`.

**UAT (978 MHz, United States)**: pick "UAT (978 MHz)" as a VFO's digital decoder - like ADS-B it sets the VFO to 2.4 MHz and tunes to 978.000 MHz. UAT is the US's second ADS-B link, used by general aviation below 18 000 ft: it decodes each aircraft's position, altitudes, speed, track, vertical rate, callsign or squawk, category and emergency status, the traffic the FAA's ground stations rebroadcast (TIS-B radar targets, ADS-R) and what those ground stations uplink - their position and the FIS-B weather / NOTAM products they send, with the text ones (METAR, TAF, PIREP, winds aloft) readable in the station's row. Aircraft and ground stations appear on the 🗺 Map, and **every aircraft and every ground station gets its own DoA** in the map's "📡 DF" panel (a TIS-B message is the ground station's signal, so it counts for the station, not the aircraft it describes). See `kraken_doa_v2/plugins/uat/README.md`.

**FLARM / OGN trackers (gliders, light aircraft)**: pick "FLARM / OGN trackers (868 / 915 MHz)" as a VFO's digital decoder. It takes the whole 2.4 MHz the tuner receives and listens to every FLARM channel inside it; if there is none it tunes the receiver to the band of your region (Europe and New Zealand: 868.8 MHz covers 868.2 / 868.4 / 869.25 MHz; Australia, the Americas: 921.6 / 915.2 MHz, where FLARM hops over many channels and 5-6 of them are heard). **FLARM** aircraft are shown by their radio ID and their bearing only: the rest of a FLARM packet is encrypted with keys FLARM hasn't published, and it is not decoded. **OGN trackers** (the Open Glider Network's open protocol) also give their position, altitude, climb, speed, aircraft type and registration, and appear on the 🗺 Map - except trackers that ask not to be tracked (stealth): those keep their bearing, but their position is never shown, mapped or logged. **Every aircraft gets its own DoA** in the map's "📡 DF" panel (🪂 / ✈), also when several transmit at the same moment on different channels. See `kraken_doa_v2/plugins/flarm/README.md`.

**CTCSS / DCS tones (analog FM)**: pick "CTCSS / DCS tones (analog FM)" as the digital decoder of a 12-24 kHz VFO on an FM channel - a repeater, a simplex frequency, PMR446, a business channel. It finds the sub-audible tone (CTCSS, 67-254 Hz, every standard and non-standard tone radios offer, its frequency measured to about 0.01 Hz) or digital code (DCS, e.g. 023N - also shown as the code it equals when read inverted, 047I) under each transmission, and how the transmission ended (the transmitter's reverse burst, the DCS turn-off code, tone or carrier off). Since repeaters and users sharing a channel pick different tones, the tone is the transmitter's ID: **each tone / code gets its own DoA** in the map's "📡 DF" panel (🔔, with its number of transmissions, last heard, the last transmission's length and the air time). With the VFO's squelch set to "Digital" the squelch opens only while a tone or code is there; the decoder's "Tone squelch" option (e.g. 88.5 or 023N) restricts that to one transmitter - its audio and its bearing, like a radio's tone squelch. Listening to the FM audio keeps working, and Voice to Text writes down the analog speech, naming each transcript after its tone. See `kraken_doa_v2/plugins/ctcss/README.md`.
**406 MHz distress beacons (EPIRB / ELT / PLB)**: pick "406 MHz distress beacons (EPIRB / ELT / PLB)" as a VFO's digital decoder - it tunes the VFO to 406.050 MHz at 100 kHz and listens to every Cospas-Sarsat channel (406.0-406.1 MHz) at once. Each burst of a first-generation beacon is checked (its BCH codes, up to 3 + 2 bit errors corrected) and decoded: the 15-hex beacon ID, the country, the type (EPIRB for ships, ELT for aircraft, PLB for people, SSAS ship security alerts), what identifies it (MMSI, aircraft address or registration, serial number, call sign) and the position it sends, which appears on the 🗺 Map. A SELF-TEST burst is shown as a test, never as an alert; any other burst is a **⚠ DISTRESS** alert (event, highlighted row, ⚠ on the map and in the DF list). **If you hear a distress beacon, report it to the rescue coordination centre** (New Zealand: RCCNZ, 0508 472 269) with its ID, time, frequency and bearing - the receiver reports nothing by itself. Each beacon gets **its own DoA** in the map's "📡 DF" panel (🆘, every burst its own bearing by default), kept for 24 hours - direction finding is how beacons are found. Second-generation beacons (spread spectrum on 406.050 MHz) are detected and get a bearing, but are not decoded yet; the 121.5 MHz homing signal isn't covered. See `kraken_doa_v2/plugins/beacon406/README.md`.

**LoRa / Meshtastic / LoRaWAN**: pick "LoRa / Meshtastic / LoRaWAN" as a VFO's digital decoder - the VFO sits on its tuner's full 2.4 MHz, so tune the receiver to the band. It works **in any country**: by default (Region / LoRaWAN plan "Auto") it listens to every Meshtastic region's and every LoRaWAN regional plan's channels that fall inside the 2.4 MHz you tuned to (e.g. 902-928 MHz: Meshtastic US + ANZ, LoRaWAN US915 + AU915 + AS923; 863-870 MHz: EU_868 + EU868 ...), and a **plan-free search** finds any other LoRa signal anywhere in the 2.4 MHz - private links, unknown networks - at any spreading factor (SF5-12) and bandwidth (62.5-500 kHz; down to 7.8 kHz as an option), and keeps listening on its channel once it has decoded it. A specific region / plan can be chosen instead. **Meshtastic**: the header is always shown (which node, to whom, hop count); the message is decoded **only on the public default channel** - its key is published, so anything sent there is readable by anyone - giving text messages, node names, and positions (shown on the 🗺 Map). A packet on a private channel shows only its header; its key is never guessed or broken. **LoRaWAN**: the clear header (device address, frame counter, port, join device IDs) and class-B beacons (with the gateway's position when it sends one) are shown - the data itself is encrypted with keys you don't have; a downlink or beacon is sent by a gateway, so its bearing is the gateway's ("GW 927.5"), not the device's. Other LoRa is shown as raw frames, named by frequency, SF and sync word. Each transmitter (and the relaying node when a Meshtastic packet was repeated) gets **its own DoA** in the map's "📡 DF" panel (📶). 2.4 GHz LoRa is outside the RTL-SDR's range. See `kraken_doa_v2/plugins/lora/README.md`.

**Wildlife tracking collars / tags**: pick "Wildlife tracking collars / tags (pulsed VHF / UHF)" as a VFO's digital decoder to find the pulsing transmitters on deer, wolves, livestock, birds, bats and other animals - in any country - and get **a bearing for each one** in the map's "📡 DF" panel (🐾). It works at any VFO width: a 2.4 MHz VFO on the tuner's centre surveys a whole band, a narrow VFO follows one collar. Each collar is listed by its frequency in MHz, pulse rate, pulse length, signal strength and mode, read from the collar's own behaviour: **baseline** (the rate it was first heard at), **⚠ possible mortality** (its rate rose - most mortality sensors double it after hours without movement - and stayed for 2 minutes), back to baseline ("moving again"), or **activity** (it switches back and forth between rates). The "Region / vendor profile" option adds what is specific to a country or vendor: North America / Europe / Australia (a doubling = mortality, their wildlife bands named), Motus (the 166.380 / 150.100 / 151.500 MHz tag frequencies) and New Zealand (DOC channel numbers, the 30 / 48 / 60 ppm rates, and the data Wildtech kiwi / kakapo transmitters send every 10 minutes - time since death, Chick Timer / Egg Timer numbers). Other transmitters' data streams are shown as their pulse groups. Lotek / Motus-style coded tags (4-pulse bursts) are told apart by their pulse pattern, also when several share one frequency. Name your collars in the decoder's options ("150.512=Elk 7, 160.734=Kaka 3"); the decoder tab plots the strength of the last collar heard, pulse by pulse - handy while walking. See `kraken_doa_v2/plugins/wildlife/README.md`.

**Digital TV** (DVB-T, DVB-T2, ISDB-T, ATSC 1.0 / 3.0, DTMB): on a 2.4 MHz VFO anywhere inside a TV channel (Decimators box → Digital decoder → "Digital TV") the decoder shows what the signal says about itself - the standard and its mode, the channel (measured centre and channel number), DVB-T's transmission parameters (constellation, code rate, guard interval, cell ID, bit rate - read from the TPS carriers and error-checked), ISDB-T's TMCC (the hierarchical layers, one-seg, and the emergency warning flag, logged as a ⚠ event) - and an echo profile of the paths the signal arrives on. Service names and the programme guide are not available: they need the whole 6-8 MHz channel decoded.

**Path separation** (coherent mode, off by default - the "Path separation" button in the Digital TV tab): a TV signal reaches the antennas on several paths - reflections off hills and buildings and, in a single frequency network (SFN), the other transmitters sending the same signal - which looks like one signal arriving from several directions at once, so a VFO's bearing is a mix. With path separation on, the decoder tells the paths apart by their delays: with DoA on, it measures every antenna's channel impulse response against the signal's known pilots / training and gives each path its own DoA lobe - a talker "Path-1", "Path-2" ... on the 🗺 Map's 📡 DF panel (lobe, bearing, heat map while driving), the strongest marked "main" and the others' delays counted from it. The tab shows the delay profile with a marker per path and a table (delay from the main path and from the first to arrive, extra path in km, power); click a marker or a row to show that path alone on the map. Whether a path is another transmitter or a reflection can't be told from the signal - both are identical copies; a path whose bearing points at a broadcast mast (the map's Masts layer) is likely a transmitter. It analyses every antenna, about three times the CPU of the decoder alone. (It was called "SFN separation" before; DAB was part of that earlier decoder and will return as a decoder of its own.) See `kraken_doa_v2/plugins/dtv/README.md`.

**LTE / 5G NR Cell Decoder** (coherent mode for bearings): reads the public information every LTE / 5G NR cell on a carrier broadcasts to all phones, and gives each cell its own bearing. Pick it for a VFO (Decimators box → Digital decoder → "LTE / 5G NR Cell Decoder"), with DoA on: the VFO becomes the whole 2.4 MHz band, drawn as one locked line at the centre like ADS-B, and you tune the receiver into an LTE / NR carrier. Near the carrier's centre it finds it on the LTE / NR raster itself; further away it retunes the receiver step by step (up to 10 MHz each way by default - every retune makes the receiver recalibrate for a few seconds) and centres it on the carrier it finds. It finds the cells (LTE FDD and TDD, NR with 15 / 30 kHz SS blocks) by their physical cell ID and lists them in its tab: operator (MCC-MNC and country), tracking area code, cell identity (eNB and sector), band, EARFCN, bandwidth, antenna ports, power and timing - open a row (▸) for everything else the cell broadcasts (MIB, SIB1, SIB2: access rules, uplink frequency, random access, power settings...; SIB3 - SIB5 when they come inside the VFO). On a carrier wider than the 2.4 MHz VFO it reads the part it sees and combines repeated transmissions until each message's checksum passes (SIB1 / SIB2 of a 10 MHz carrier in seconds). Each cell also becomes a talker "LTE-123" / "NR-456" on the 🗺 Map's 📡 DF panel (lobe, bearing, heat map while driving) - a cell's signals are scrambled with its cell ID, so correlating with them separates the cells even when they arrive at the same time. The "Frequency offset" it reports is the network's carrier against your receiver: base stations are accurate to 0.05 ppm, so it calibrates the KrakenSDR. Only broadcast system information is decoded - nothing addressed to a phone (calls, data, paging). See `kraken_doa_v2/plugins/cells/README.md`.

New decoders can be added as plugins, and the **AI Signal Lab** (sidebar "🤖 AI Signal Lab") can write them for you: it points an LLM coding agent (Claude Code, or another LLM CLI) at the signal in a VFO, tells you what the signal is, and on request writes, builds and tests a decoder plugin for it - see "AI Signal Lab and decoder plugins" below.

The KrakenSDR Suite V2 software is currently in beta.

Install:

```
git clone https://github.com/krakenrf/krakensdr_suite
cd krakensdr_suite
./install.sh
```

You may need to reboot after running ./install.

Docker install (instead of install.sh / run.sh):

```
# Docker + Compose (Raspberry Pi OS / Debian trixie; on Bookworm or Ubuntu use
# "curl -fsSL https://get.docker.com | sudo sh" instead)
sudo apt-get install -y docker.io docker-compose
sudo usermod -aG docker $USER          # then log out and back in

# Keep the DVB-T kernel driver off the dongles
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-dvb_usb_rtl28xxu.conf
sudo modprobe -r dvb_usb_rtl28xxu

git clone https://github.com/krakenrf/krakensdr_suite
cd krakensdr_suite
docker compose up -d                   # first run builds the image (~5 min on a Pi 5)
```

The containers then start automatically at every boot. Don't run the native
stack (run.sh or its boot service) at the same time. See [DOCKER.md](DOCKER.md)
for settings, hardware variants and troubleshooting.

To Run:

Simply use the command:

```
./run.sh
```

In any terminal window, with your KrakenSDR connected and powered up.

The Web UI can then be accessed at:

**Heimdall:** http://krakensdr.local:8070 or http://PI_IP_ADDR:8070

**KrakenSDR DOA:** https://krakensdr.local:8080, or https://PI_IP_ADDR:8080 (note, remember the 's' in  https)

## Digital voice codecs

A VFO's digital decoder can play the voice of P25, DMR, D-STAR, NXDN and TETRA
calls. Click "🔊 Listen" in the VFO's decoder tab (panel under the waterfall),
or pick Demod "Digital" on the VFO that is the Audio Src. Encrypted calls are shown and muted - they can't be decoded.

| System | Voice codec | Needs |
|---|---|---|
| P25 Phase 1 | IMBE | nothing - built in |
| DMR | AMBE+2 | mbelib, installed by you (below) |
| D-STAR | AMBE | mbelib, installed by you (below) |
| NXDN (NXDN48 and NXDN96) | AMBE+2 | mbelib, installed by you (below) |
| MPT1327 | analogue FM | nothing - listen to the granted traffic channel with NBFM |
| TETRA | ACELP | the ETSI reference codec, built by you (below) |

The AMBE / AMBE+2 and TETRA codecs are not shipped with this software:
AMBE+2 is a commercially licensed codec (DVSI), and the ETSI codec may not be
redistributed. You can build them yourself for personal use; whether that is
allowed where you live is your responsibility. The sidebar's "🔐 Digital
Decoders" box shows which codecs are installed and the install steps for the
missing ones (press "↻ Check again" after installing).

### DMR, D-STAR and NXDN voice: mbelib

```
sudo apt-get install -y git cmake build-essential
git clone https://github.com/szechyjs/mbelib.git
cd mbelib && mkdir build && cd build
cmake .. -DDISABLE_TEST=ON
make -j3
sudo make install
sudo ldconfig
```

Then restart the KrakenSDR software (`./run.sh`, or reboot when it runs as a
service). kraken_doa finds `libmbe.so.1` in the system library path; set
`KRAKEN_MBELIB=/path/to/libmbe.so.1` to use one elsewhere. API-compatible
mbelib forks work too.

### TETRA voice: ETSI codec (via osmo-tetra's patches)

```
sudo apt-get install -y git build-essential unzip patch wget
git clone https://github.com/osmocom/osmo-tetra.git
cd osmo-tetra/etsi_codec-patches
wget -O etsi_tetra_codec.zip https://www.etsi.org/deliver/etsi_en/300300_300399/30039502/01.03.01_60/en_30039502v010301p0.zip
md5sum etsi_tetra_codec.zip     # must be a8115fe68ef8f8cc466f4192572a1e3e
sh ./download_and_patch.sh      # unpacks and patches the zip downloaded above
cd ../codec/c-code
make
sudo install -m 755 cdecoder /usr/local/bin/tetra-cdecoder
sudo install -m 755 sdecoder /usr/local/bin/tetra-sdecoder
```

Then restart the KrakenSDR software. kraken_doa runs `tetra-cdecoder` and
`tetra-sdecoder` from the PATH; or set `KRAKEN_TETRA_CODEC_DIR` to a directory
holding `cdecoder` and `sdecoder`. TETRA voice plays about half a second late
(the codec programs buffer their output), and only on cells without air
interface encryption.

### Docker

Build the codecs on the host as above (the `sudo make install` / `install`
steps can be skipped), then copy them into the container's data directory and
restart the client:

```
sudo mkdir -p docker-data/codecs
sudo cp -L mbelib/build/libmbe.so.1 docker-data/codecs/
sudo cp osmo-tetra/codec/c-code/cdecoder osmo-tetra/codec/c-code/sdecoder docker-data/codecs/
docker compose restart kraken_doa
```

(paths relative to where you cloned mbelib and osmo-tetra; copy only the ones
you built)

## Voice to Text

Speech on the VFOs written down by a local, free speech-to-text model
([whisper.cpp](https://github.com/ggml-org/whisper.cpp), MIT licence - OpenAI's
Whisper models). Set it up once on the receiver (about 10 minutes on a Pi 5):

```
cd krakensdr_suite/kraken_doa_v2
make whisper
```

This builds the speech-to-text helper (`transcribe/build/kraken_whisper`,
whisper.cpp at a tested version) and downloads the default model (base.en,
142 MB) plus a small voice detection model into `transcribe/models/`. No
restart is needed: tick "🗣 Transcribe" on a VFO. `./install.sh` does it too
when run as `WHISPER=1 ./install.sh`; after that, `make` keeps the helper up
to date.

Other models: pick one in the sidebar box's download list and press "⬇ Get",
or on the receiver `transcribe/setup.sh --models-only small.en`
(`transcribe/setup.sh --list` shows them all):

| Model | Download | On a Pi 5 | |
|---|---|---|---|
| tiny.en | 75 MB | ~5x faster than real time | least accurate |
| base.en | 142 MB | ~2-3x faster than real time | the default |
| base.en-q5_1 | 57 MB | about the same | less memory |
| small.en / small.en-q5_1 | 466 / 181 MB | ~2x slower than real time | most accurate on a Pi; for quiet channels |
| tiny, base, small (+ -q5_1) | as above | as above | any language, or translated to English |

The receiver keeps working while the model is busy: transmissions wait in a
queue (up to 40 / 10 minutes of audio; the oldest are dropped beyond that -
the box shows it). If they pile up, give it more CPU cores, use a smaller model
or transcribe fewer VFOs. The model process only runs while a VFO transcribes
and stops 2 minutes after the last one is unticked (base.en uses ~280 MB of
RAM). Transcripts are kept in `kraken_doa_v2/transcripts.jsonl` (the latest
1000 are shown).

Direction of each transmission: in coherent mode with DoA on, every
transcript keeps its DoA lobe - the MUSIC spectrum averaged over the
transmission - and where the station was. Click the 🧭 bearing in its row
and the 🗺 map opens with that lobe drawn from the station, dashed, in a pale
colour of its own (white, then violet, blue, ...), so it never looks like a
VFO's live lobe; its tag at the tip names the VFO, time and bearing, and its
✕ (or clicking the bearing again) removes it. Show several to compare
transmissions. Without a station heading at the time (GPS before the vehicle
moves, no compass) the bearing reads "arr" and the lobe is drawn relative to
the array, its front towards north.

Long transmissions and broadcasts: a transmission is one transcript up to 25 s;
a longer one is cut into parts where the speech pauses (part 1, 2, ... in the
tab). Broadcast FM (a WBFM VFO), and anything on air longer than 30 s (AM
broadcast, a weather channel), comes in pieces of about 8-15 s, each cut at a
pause, so its text follows the station live, 10-20 s behind. With no pause
(music), the cut falls on the quietest moment and the next piece repeats the
0.3 s before it, so a word cut there is whole in one of them.

The audio ("Keep the audio to play back", on by default) is saved as Ogg Opus
(16 kbit/s, about 7 MB per hour of speech; any audio player plays it - the
text, VFO, frequency and bearing are in its tags) in the Decoder Logging
folder: `voice_clips/YYYY-MM-DD/<id>.opus` (by default
`kraken_doa_v2/decoder_logs/voice_clips/`, also when Decoder Logging itself is
off). It follows Decoder Logging's settings: a day's clips are deleted after
its "Keep" days (0 = kept forever), and with less than 100 MB free on that
drive no clips are written (the transcripts go on; the box says so). At most
2 GB of clips are kept in all - beyond that the oldest go first. Pick another
drive in Decoder Logging to keep more; clips already saved stay in the old
folder (the 🗣 Voice tab plays only those in the current one).

Docker: the image includes the helper (`WHISPER=0` in `.env` leaves it out);
get the models with the sidebar's "⬇ Get" - they are kept in
`docker-data/whisper_models/`.

## AI Signal Lab and decoder plugins

### Decoder plugins

Every decoder in a VFO's decoder list is a plugin: a folder of C++
source in `kraken_doa_v2/plugins/<id>/`, which `./install.sh` / `make` builds.
The suite ships P25, DMR, TETRA, D-STAR, NXDN, MPT1327, POCSAG (pagers),
APRS (AX.25 1200 bit/s packet; written by the AI Signal Lab) and ADS-B
(1090 MHz aircraft, plotted on the 🗺 Map). "Auto detect"
runs every decoder ticked "Auto detect" in the sidebar's plugin list (Digital
Decoders box, "Decoder plugins") - all of them by default; untick the ones you never need, since each
costs CPU on every VFO in Auto detect. ADS-B has no Auto detect tick: it is
only used when picked. Decoders that need a much wider VFO than the one Auto
detect runs on are left out there. A decoder's own settings - the DMR timeslot,
the P25 NAC - appear in the panel while that decoder is selected, or once
Auto detect has locked onto it. Plugins run as separate processes, so a
faulty one cannot crash the receiver. To write one by hand, see
[kraken_doa_v2/plugins/SDK.md](kraken_doa_v2/plugins/SDK.md).

To add a plugin, or move one to another KrakenSDR, copy its folder (without
`build/`) into the receiver's `kraken_doa_v2/plugins/`, then build it and
press ↻ next to "Decoder plugins" in the Digital Decoders box (or restart the
software):

```bash
cd ~/krakensdr_suite/kraken_doa_v2      # wherever the suite is installed
make -j3                                # builds kraken_doa + every plugin
# or just the one plugin:
make -C plugins PLUGIN=<id>
```

Run the same `make` after editing a plugin's source; decoders that are using
it restart on the new build by themselves. The plugin compiles against the
receiver's own `plugins/sdk/` and `plugins/lib/`, so the receiving KrakenSDR
should run the same (or a newer) version of this software. A plugin is
native code that runs on your receiver: only install plugins from people you
trust.

### Debug playback (test transmissions)

The sidebar box **🧪 Debug Playback** plays a decoder's synthetic test
transmission - several transmitters with known IDs at known bearings,
received on a 5-element circular array - through the receiver as if the
antennas had picked it up: spectrum, waterfall, the decoder, the DoA and
the 🗺 Map's 📡 DF panel. Use it to see what a decoder shows, or to check
it still finds every transmitter at the right bearing (**Check** compares
the DF panel with the bearings the test was made with). Press **⚙
Generate** once per test (the decoders' own generators, a few seconds to a
minute of CPU; stored in `kraken_doa_v2/testsignal_cache/`, at most 3 GB),
then **▶ Play**.

While it plays, a red **DEBUG PLAYBACK - not live** banner sits in the top
bar and over the spectrum; heimdall's data is disconnected and nothing is
sent to heimdall (its tuning stays as it was); retuning, gain and the
scanners are refused; the VFO, decoder and array settings the test sets up
are NOT saved - **Stop** puts your own settings back, and so does a restart
in the middle of a playback. Nothing from a playback goes out: web mapper,
decoder data log, Voice to Text, DoA recording and incident lookups pause.
Generating needs python3 with numpy and scipy (installed by `install.sh`;
not in the Docker image - generate on the host and copy the files into
`docker-data/kraken_doa/testsignal_cache/`). Test transmissions exist for
AIS, APRS, D-STAR, NXDN, TETRA, MPT1327, VOR, CTCSS / DCS, pulse radar,
1030 MHz interrogations, UAT, FLARM / OGN, 406 MHz distress beacons, LoRa /
Meshtastic, wildlife collars, DVB-T, LTE, ADS-B, DME / TACAN, ISM devices,
POCSAG, V.23 telemetry, P25, DMR and radiosondes - every decoder. POCSAG
and V.23 name no transmitter, so their
Check is the decoded messages and the VFO's own bearing (Digital squelch).
A busy receiver plays a test slower than real time rather than skipping
parts of it.

### AI Signal Lab

1. Install Claude Code on the Pi and log in once (a Claude subscription or an
   Anthropic API key):

   ```
   curl -fsSL https://claude.ai/install.sh | bash
   claude          # log in, then /exit
   ```

2. Enable the lab, which checks the login first:

   ```
   cd krakensdr_suite/kraken_doa_v2
   python3 ai/kraken_ai.py setup
   ```

   It stays off until you do this. The web UI can't switch it on, because it
   lets UI users run an AI agent on the Pi and compile code there. If other
   people can reach your receiver's web UI, set an API token
   (`KRAKEN_API_TOKEN` / the `api_token` file) first. `python3 ai/kraken_ai.py disable`
   switches it off again.

3. In the web UI (sidebar "🤖 AI Signal Lab"), pick the VFO sitting on the
   signal and press **🔍 Investigate this signal with AI**. The lab records the
   signal (5-60 s at the VFO's bandwidth) and measures it: spectrum, bursts,
   envelope, FM levels, symbol rate and repeated sync words, plus plots. The
   agent can make more captures and run its own analysis scripts. Then it
   writes a report: what the signal is, the evidence for it, and whether an
   existing decoder handles it. The report, the live progress and the
   agent's steps appear in the **🤖 AI** tab of the panel under the waterfall
   (the sidebar only shows the job's status); ask follow-up questions there.
4. **🛠 Create Decoder** has the agent write a plugin for it, build it and test
   it on the captures. This takes about 5-30 minutes. The plugin then shows up
   in every VFO's decoder list and in the "Digital Decoders" box; "Use on VFO"
   next to the finished job selects it on the AI box's VFO.
   **✍ Custom Instructions** passes your own hints to the agent: specs or
   links, what to show, what to ignore. Pressing the button again on an
   existing plugin improves that plugin.

Every investigation is kept with its conversation: an "AI" badge appears in
the spectrum above the analysed frequency, and the AI Signal Lab's History list
shows them all. Clicking either opens the **🤖 AI** tab of the panel under the
waterfall (also the "🤖 AI" button there), with the whole chat, the agent's
steps and a box for further questions; ✕ deletes an analysis together with its
captures and chat.

The agent's tools are restricted. It can read the plugin SDK and the existing
decoder plugins, run the capture/analysis tool (`ai/sigtool.py`) and Python scripts
in its session folder, and write only in `ai/sessions/<session>/` and
`plugins/<id>/`. It can't read the receiver's settings, API key or TLS key.
Sessions (captures, plots, reports, chats) are kept in `kraken_doa_v2/ai/sessions/`.
Captures are written to disk as they arrive, so long ones (up to a minute
from the web UI) need little memory. Every Python process the agent runs is
limited to the memory the Pi has free (minus a reserve for the receiver): a
script that needs more stops with an out-of-memory error instead of freezing
the Pi.
Usage counts against your Claude plan or API budget. A typical
investigation costs about $0.30-0.50 API-equivalent, and a decoder about
$1-5.

Other LLM command-line tools can be used instead:
`python3 ai/kraken_ai.py setup --backend command --command "your-cli args"`
(the prompt goes to its stdin, and it runs in the session folder). Such a tool
gets no tool restrictions from the lab; its own configuration decides what it
may do. The AI Signal Lab needs the native install (it is not available in
Docker). The plugins that ship with the suite work in Docker as well.
