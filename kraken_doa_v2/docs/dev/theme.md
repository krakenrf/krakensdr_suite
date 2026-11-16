# The colour palette

`kraken_doa.html` (and `array_calculator.html`) use one small palette,
defined as CSS variables at the top of the page's CSS and, with the numbers
behind it, in `tools/theme_palette.py`. This file says why it looks the way
it does and how to use it. The layout and wording rules are in *sidebar.md*.

## Why it was changed (2026-11-16)

The user found the colour scheme ugly and asked for one chosen by colour
theory. Measured on the old page (every colour literal, in OKLCH - lightness
L, chroma C, hue h):

- 340 distinct colours in 1627 places; 70 % of them one blue hue
  (h 250-285), another 14 % plain greys.
- The structure was as saturated as the things to act on: the lines
  (`#0f3460`, C 0.09; `#2a4a7a`, `#1a4a7a` ~0.09-0.10) and the surfaces
  (C 0.04-0.06) against accents of C 0.13-0.16. With base and accent in the
  same hue and nearly the same chroma, nothing stood out - "blue on blue".
- The state colours were raw Material tones of unequal weight (`#b71c1c` C
  0.17 next to `#1b5e20` C 0.11, `#ff9800` 0.17, `#f44336` 0.21), and the
  text came in a dozen blue-greys.

## The scheme

Dark interface practice plus three rules of colour harmony:

1. **A tinted neutral base.** Everything structural - surfaces, lines,
   text - has the accent's hue (h 255) at almost no chroma (C 0.012-0.027).
   It reads as graphite with a cool cast, never as a colour, so it recedes
   and the data (spectrum, waterfall, map, VFO colours) is the colourful part
   of the screen. Depth comes from lightness alone: darker is further back
   (page < bars < panels < headers), text boxes and plot wells are recessed.
2. **One accent** (azure, h 250, C up to 0.165) for what can be acted on or
   is selected: primary buttons, the lit tab / segment / rail button,
   slider thumbs, focus rings, links and numeric readouts (lighter, less
   chroma). Roughly the 60-30-10 split: neutral base, lighter neutrals,
   a tenth accent. The old cyan (h 231) and blues (h 252-262) are one hue
   now; an analogous spread of a few degrees is all that is left.
3. **States as a family, the complement for attention.** Good (green, h
   152), attention (amber, h 78 - the complement of the azure, so it is the
   colour that stands out most against the interface), problem (red, h 25)
   and playback / replay (violet, h 300) share lightness steps and a chroma
   ceiling (C ~0.15 at mid lightness), so none shouts louder than another.
   Neon primaries are gone.

Lightness was kept in the conversion, so every contrast relation the page
had is still there; chroma and hue changed.

## The tokens

`python3 tools/theme_palette.py` prints them with their OKLCH values;
`--contrast` the table below; `--css` the `:root` block; `--check` verifies
the page against the palette.

| | Tokens | For |
|---|---|---|
| Surfaces | `--s0` .. `--s5` (L 0.13 .. 0.30) | s0 plot / list wells, s1 the page behind the panels and text boxes, s2 the sidebar, bars, boxes inside boxes, s3 panels and boxes, s4 headers, s5 hover |
| Lines | `--l1` .. `--l5` (L 0.35 .. 0.52) | l1 lines between parts, l2 text-box borders and plain buttons, l3 / l4 control borders, l5 strong |
| Text | `--t6` .. `--t0` (L 0.57 .. 0.915) | t6 units / placeholders, t5 muted, t4 / t3 labels, t2 secondary, t1 / t0 text and values |
| Accent | `--ac-d2`, `--ac-d`, `--ac`, `--ac-h`, `--ac-l`, `--ac-t`, `--ac-t2`, `--ac-t3` | containers, **`--ac` fills**, hover / selected borders, thumbs and focus, accent text |
| States | `--ok-*`, `--warn-*`, `--bad-*`, `--spec-*` with `-d` (container), `-m` (fill / border), plain, `-t`, `-t2` (text) | good, attention, problem / destructive, playback |

Contrast (WCAG ratio; 4.5 = body text, 3 = large text and control parts) on
the surfaces s1 / s2 / s3 / s4:

| Token | s1 | s2 | s3 | s4 |
|---|---|---|---|---|
| t0 | 15.0 | 14.3 | 13.4 | 12.0 |
| t2 | 10.3 | 9.8 | 9.2 | 8.3 |
| t3 (labels) | 7.8 | 7.4 | 6.9 | 6.2 |
| t5 (muted) | 5.4 | 5.2 | 4.8 | 4.3 |
| t6 (faint: units) | 4.3 | 4.1 | 3.8 | 3.5 |
| ac-t | 9.2 | 8.7 | 8.2 | 7.4 |
| warn-t | 11.6 | 11.1 | 10.4 | 9.3 |
| bad-t | 8.5 | 8.1 | 7.5 | 6.8 |

White on the fills: `--ac` 4.8, `--ok-m` 5.6, `--bad-m` 7.0, `--spec-m` 6.9.

## Rules

- In the page's CSS write `var(--token)`, not a new colour. In JS - inline
  styles in generated HTML, `el.style`, canvas code - write the token's hex
  (canvas cannot read a CSS variable; the script holds copies). A changed
  token therefore means: `tools/theme_palette.py --css` into the `:root`
  block, and the old hex replaced in the script.
- Pick the token by role, not by looks: a nested box is `--s2`, a header
  `--s4`, a text box `--s1` with an `--l2` border, a line `--l1`, a label
  `--t3`, a unit `--t6`.
- The accent only for things to act on or that are selected. A state colour
  only for its state - amber is "attention", not decoration.
- **Data colours are not part of the palette and were left exactly as they
  were**: the VFO colours (`getDecimatorColor`), the saved frequencies'
  category swatches (`SF_PALETTE`, stored on the receiver), the Voice
  lobes' and viewshed sites' colours (`V2T_LOBE_COLS`, `VS_COLS`), the
  coverage and range scales (`PL_STOPS`, a ColorBrewer scale, and
  `RG_STOPS`), the masts' categories (`MAST_CATS`), the waterfall colour
  maps (numeric) and what plugins send. Scales and categorical sets depend
  on their exact steps - never "harmonise" them.
- The top bar's calibration colour is a 13 % wash of the state colour from
  the top plus a 2 px line of it (`.top-bar.cal-*`); the calibration chip's
  colours are tokens (waiting grey, measuring / cooldown amber, applying
  azure, verifying violet, converged green).
- heimdall's page (`heimdall_v2/index.html`) has its own light scheme and
  is not covered.

## How the page was converted

Three scripted passes over the page (kept out of the repo; the rules are
here so a later page can be treated the same way):

1. **Recolour**: every colour literal (hex of any length, `rgb()` /
   `rgba()`; not HTML entities, not the inlined uPlot) through one function
   of (L, C, h): greys take the base tint; hue 222-300 with L < 0.5 and
   C < 0.125 (the navy surfaces and lines) -> h 255, chroma x 0.3 (0.012 ..
   0.030), lightness stretched x 1.3 around L 0.228 (without chroma the
   surfaces need more lightness between them); blue-greys (C < 0.075) ->
   h 255, half the chroma; the other blues -> hue pulled to 250 (30 % of
   the distance kept, 238 .. 262) under a chroma ceiling by lightness;
   green / amber / red / violet -> hue pulled toward 152 / 78 / 25 / 300
   (45 % kept) under one ceiling; teal (180-222) kept. Lines holding a data
   palette are skipped.
2. **Tokens**: every neutral to the nearest step of the ladder (by
   lightness); an accent or state colour to its family's token when within
   0.036 in OKLab, else kept. In the CSS as `var(--name)`, elsewhere as the
   hex. 340 -> 172 distinct colours; 979 `var()` uses; 98 literals left in
   the CSS (state and data shades between tokens).
3. **Roles by hand**: the elevation order (body and page s1, sidebar / top
   bar / rail s2, boxes s3, headers s4, nested boxes s2, text boxes s1),
   the top bar's calibration wash, button hover, the cooldown / verifying
   chip colours.

`array_calculator.html` went through passes 1 and 2 with the hexes written
out (it has no `:root` block).

## Tests (2026-11-16)

Headless Chromium against a TEST build; `test_theme.py` in the job's
scratch, 41 checks:

- the palette block defines 45 tokens, every `var(--...)` in the page names
  a defined token or one of the page's own variables;
- with every colour blanked, the page's text is identical to the one
  before (apart from the palette block and the hand-set role rules): only
  colours changed;
- the data palettes above are byte for byte as before; `getDecimatorColor`
  returns the same eight colours;
- computed styles: the surfaces sit on their tokens (page s1, sidebar s2,
  box s3, header s4, rail s2, VFO card s2, text box s1), their lightness
  rises in that order, their chroma is <= 0.02; the primary button is the
  accent, a plain button neutral, a switch that is on green;
- contrast where the text is drawn: labels, values, titles, button text,
  text boxes (>= 4.5 or 7), the faint version line (>= 3);
- the calibration chip's six states: hues in their families, chroma
  0.13-0.16, waiting neutral;
- the array calculator loads with a neutral background, no JS errors.

The five other suites pass unchanged (53 sidebar, 78 drag, 35 page, 33
hide, 48 top bar; one of them compared a computed colour, updated to the
token). Looked at: the live page with a test transmission, the idle page,
every sidebar box, both viewshed modes with the coverage legend, range,
replay, the save dialog, the power dialog, independent mode, the assistant
dock, the top bar in each calibration state, the phone layout, the array
calculator. Not seen: Firefox / Safari, the Wideband variant's controls with
real data, a Pi's display.
