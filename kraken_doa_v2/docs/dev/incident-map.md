# Incident map

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Incident map (`src/incidents.cpp`, `geo_address.cpp`, `geo_http.cpp`):**
- Plugins send free-text messages with `host.message(from, text)`
  (`kp::Info::messages`; pagers: every alphanumeric POCSAG / FLEX page). Wire MESSAGE (28)
  -> Engine -> `dig::set_message_handler()` (set by `incidents::start()`;
  unset in digi_test: dropped) with the DigitalDecoder* (resolved to its VFO
  through `decimator_manager`, so a removed VFO's messages are dropped)
- Address finding (`geo::geocode`, no local street data - the user wants an
  online lookup, NOT a downloaded OSM extract). Latin-script countries; text
  folded to upper-case ASCII words (accents; ß -> SS, Æ -> AE, Œ -> OE;
  apostrophes kept in the word for the search - Nominatim needs O'CONNELL -
  but dropped from keys; , ; : ( ) recorded as breaks a name doesn't cross).
  Candidate groups, those with a house number next to them first, <= 10
  lookups per message:
  1. name + type after (English ST/RD/AVE..., German / Dutch / Nordic /
     Hungarian / Turkish words written separately: STRASSE, ALLEE, WEG,
     GATE, GATAN, VEJ, UTCA, CADDESI...) + optional direction ("QUEEN ST W")
  2. type before + name (RUE, AVENUE/AVENIDA, BOULEVARD, CALLE, VIA, PIAZZA,
     RUA, PRACA, UL/ULICA, JL/JALAN... - `prefix_types()`; UL / OS are not
     in OSM's names and are dropped from the key; a name without its prefix
     type in OSM also matches)
  3. one word with the type joined on (`COMPOUNDS`: -STRASSE/-STR/-STRAAT
     -> ST, -WEG, -PLATZ, -ALLEE, -GRACHT, -LAAN, -GATAN, -VAGEN, -GADE,
     -VEJ, -VEIEN, -KATU, -TIE, -UTCA...)
  House number before or after the street ("NO" / "NR" before it skipped).
  Name words: up to 4, never a stop word (English pager words + FEU,
  INCENDIO, BRAND, POZAR...) or a word with digits except ordinals (5TH,
  42ND). `name_word()`: SAINT -> ST, MOUNT -> MT, NORTH -> N (US "N MAIN
  ST" = North Main Street), FIFTH -> 5TH. Each candidate is searched in Nominatim (`geo::Nominatim`,
  nominatim.openstreetmap.org jsonv2 + addressdetails, viewbox = the radius
  around the station, bounded) - a result counts only if its road's key
  (`street_key`: ST/STREET, SAINT/ST, MOUNT/MT...) is the candidate's
  (motorways / highways also without leading words). Choice: (1) the house
  (house_number) in a place the text names (result suburb / town / city
  key - also without CITY / CENTRAL / N / MITTE... - found in the rest of
  the text, compared loosely: AE/OE/UE = A/O/U so ZUERICH = Zürich, no
  spaces / apostrophes), (2) the junction with a cross street (closest pair
  <= 1.5 km; "5TH AVE / E 42ND ST"), (3) a search with the 1-2 words after
  the street ("17 ATKINSON AVE OTAHUHU"; Nominatim only answers when every
  word matched), (4) the house nearest the station, (5) the street in a
  named place, (6) the nearest. Key comparison (`same_street`): equal, or
  equal loosely (umlauts as AE/OE/UE, spaces: "MH" = "M.H."), or OSM adds
  a direction (O'Connell Street Lower, Queen Street West).
  Confidence high / medium / low (several same-named streets, or the words
  after the street found nothing). <= 8 lookups per message
- Nominatim usage policy: User-Agent naming kraken_doa, >= 1.1 s between
  requests (callers queue on the lock), answers cached 24 h (2000). Plain
  OpenSSL HTTPS GET (`geo::https_get`, certificate verified). HTTP 429 /
  502-504: no requests for 1 min doubling to 30 min; reported as offline,
  so the messages wait (seen during testing: ~100 test lookups in a few
  minutes got 429s)
- `incidents.cpp`: worker thread + queue (300). No internet (no HTTP answer)
  -> the message is retried every minute for 30 min; an error answer or
  no address -> dropped. An incident = VFO + address key (street key +
  number) per PLUGIN - the same address paged within 2 h on ANY VFO adds a
  page to it and its (VFO, frequency) to `heard` (fire + ambulance pagers
  paging one address = one incident; table "D0, D2" / "153.2750,
  157.4500", popup "Received on"). `vfo` / `rf_hz` = the first / latest.
  The message handler gets the VFO's RF (`MessageHandler(.., rf_hz)`). Kept 24 h, max 5000,
  saved to incidents.tsv (cwd; reloaded at start; written <= every 10 s and
  at shutdown via `incidents::save_now()`). A new incident logs "📍
  Incident: ..." in the decoder's event log
- Output: `incidents::map_json(plugins)` in `build_map_message` (plugin
  "incidents", kind "incident" = red triangle in the UI, `v` = the VFO that
  received it; shown while any decoder running that plugin -
  `DigitalDecoder::active_plugin()`, the fixed or detected one - has Plot on
  map on, so a removed VFO's incidents stay visible), and NOT per VFO:
  `incidents::message_json()` -> `{"incidents":{seq, geo, cols:[VFO, MHz,
  Paged, ...], rows:[[id, age, plugin, vfo, cells..]]}}`
  (`MessageBuilders::build_incidents_message`: from the 1 Hz map timer when
  `incidents::seq()` (bumped by new / updated / expired / clear) or the geo
  status changed, else every 15 s; forced on `GET_INCIDENTS` (page connect)
  and INCIDENTS_CLEAR). UI `incOnMessage` -> `incData`, rows shown in the
  tab of their plugin (`digIncRows`); `incidents::status_json()` also rides
  as `"geo"` next to `"digital"` (UI `geoStatus`)
- Commands: `GEO_RADIUS_KM:<10-1000>` (persisted, schema before
  DECIMATORS), `INCIDENTS_CLEAR`, `GET_INCIDENTS`. UI: `digIncidentsRow(plugin)`
  in the tab of a messages plugin (`kp::Info::messages`; Plot on map shown
  for its VFOs too). Test: scratchpad h/t_tabs.py (POCSAG on D0, a second
  VFO joins, switches to DMR, is removed; incidents merged across VFOs) and
  t_tabs_adsb.py (merged aircraft table)
- Tests (scratchpad): `tools/geo_test.cpp` (`geo_test LAT LON KM "text"...`,
  online) on NZ fire / ambulance style pages; end to end on a TEST build
  with other ports (scratchpad/kbuild: config.hpp ports 18080/18081/18091/
  18092 - the live stack keeps 8080/8091) fed synthetic POCSAG pages
- Tested (tools/geo_test, scratchpad world.py: sample pages with a station
  in each country): US (S Wacker Dr, N Michigan Ave, 350 5th Ave, the 5th
  Ave / E 42nd St junction), Canada, UK, Ireland, Australia, South Africa,
  NZ, France, Germany, Austria, Switzerland, Netherlands, Spain, Italy,
  Portugal, Brazil, Sweden, Denmark, Norway, Finland, Poland, Hungary - the
  right street everywhere, mostly the house itself
- Not found: non-Latin scripts (Cyrillic, Greek, Arabic, CJK - not read),
  route numbers (SH1, I-95, A1), landmarks, names OSM spells out where the
  page abbreviates them (Jakarta's "Jl. MH Thamrin" is "Jalan Mohammad
  Husni Thamrin"), streets Nominatim can't match (e.g. "SOUTHERN MOTORWAY"
  - OSM: "Auckland Southern Motorway", found only with "Auckland")

