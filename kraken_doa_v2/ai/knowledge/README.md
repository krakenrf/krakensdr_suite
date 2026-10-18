# AI assistant: your reference files

Put files here that the AI assistant should search when it identifies a
transmitter, for example:

- exports of your country's spectrum licence register (CSV) - e.g. New Zealand's
  Register of Radio Frequencies, the US FCC ULS, Ofcom, ACMA's RRL
- lists of local repeaters, beacons, broadcast transmitters, airport frequencies
- your own notes ("153.275 MHz = the fire service paging, Mt Eden mast")

The assistant reads (and may add notes to) this folder; nothing here leaves the
receiver unless the assistant quotes it in an answer. Everything except this
README is gitignored. What the assistant itself found out goes to
`ai/identifications.json` (its remember_signal tool - the "AI" badges on the
spectrum).
