# Colours: the page's palette, colour-blind safe

The page (`web/index.html`) takes its hues from IBM Design Library's colour-blind-safe set:

| IBM name | hex |
|---|---|
| Ultramarine 40 | `#648fff` |
| Indigo 50 | `#785ef0` |
| Magenta 50 | `#dc267f` |
| Orange 40 | `#fe6100` |
| Gold 20 | `#ffb000` |

Every colour on the page is a token in the `<style>` block at the top: `:root` holds the dark set, and the light set
is written twice (under `prefers-color-scheme` and under the `data-theme` toggle; the self-test checks that the two
match). The canvases read the same tokens through `getComputedStyle`. The displays stay dark in the light theme, so
the canvas tokens (`--cv-*`, the classes, `--sc*`) are one set for both themes.

**The module's LEDs are out of scope for now.** They are a different kind of display and will get their own pass,
so the panel still shows the old colours. `src/` is untouched.

## The mapping

The IBM hues are for marks and fills. Text that carries the same meaning uses a tint of the same hue: lighter on the
dark theme and darker on the light one, chosen so that it measures at least 4.5:1 on every surface (`bg`, `panel`,
`raise`, `btn`, `pad`). The self-test's "contrast:" checks measure this.

| meaning | hue | mark / fill | text, dark | text, light | was |
|---|---|---|---|---|---|
| kick | Magenta 50 | `#dc267f` | (class name in `--ink`) | (class name in `--ink`) | `#eb4614` red-orange |
| snare | Ultramarine 40 | `#648fff` | ″ | ″ | `#28c878` green |
| hat | Gold 20 | `#ffb000` | ″ | ″ | `#e6c828` yellow |
| other | Indigo 50, a step darker | `#6a50e0` | ″ | ″ | `#3c78e6` blue |
| accent (buttons on, the Chop, the brand); markers you placed or moved | Orange 40 | `#fe6100` (`--accent`, `--cv-orange`) | `#fe904d` `--accent-ink` | `#9d3c00` | `#eb7414` / `#e06a0c` |
| markers not yet touched | neutral grey | `#9a988f` `--cv-marker` | — | — | same |
| a recording (lane), the selected slice, a locked knob, the selected key | Gold 20 | `#ffb000` (`--cv-rec`, `--cv-sel`, `--lock`) | — | — | `#ffd54a` / `#e9b800` |
| ok (clock locked, link, a finished upload) | Ultramarine | `--ok-line` `#2a3c6b` / `#a2bcff` | `#c1d2ff` `--ok-ink` | `#415da6` | green `#9be0b0` / `#1d6b3a` |
| warn (clock following, a notice) | Gold | `--warn-line` `#664600` / `#ffc84d` | `#ffd473` `--warn-ink` | `#7a5400` | `#ffcf80` / `#744800` |
| bad (an error, record, danger buttons, a drop that orphans a recording) | Magenta | `--bad-line` `#6b1f45` / `#ef9dc5` | `#ec88b9` `--bad-ink` | `#a51d5f` | red `#ff8577` / `#a3261a` |
| Brain page, the Brain jack map | Indigo | `--brain-line` `#3d3278` / `#c2b7f8` | `#a18ef5` `--brain-ink` | `#3e3080` | light blue `#9fd0f5` / `#1d5a88` |
| a call / punch, onsets | pale Ultramarine | `#b2c7ff` `--cv-call` | `#8babff` `--call-ink` | `#405ca3` | `#78a0ff` |
| the playhead on the source, an unselected wave, the Play tab's output trace | pale neutral | `#cfcdc4` `--cv-ok` | — | — | mint `#9be0b0` |
| grid boundary, the current step | white | `#ffffff` `--cv-play` | — | — | same |

The surfaces, the neutral inks and the canvas greys are unchanged.

**Why this mapping.** Four classes take four of the five hues. With Orange left out, the weakest pair is
Ultramarine / Indigo (ΔE00 9.4 under deuteranopia at IBM's values); with any other hue left out, Magenta / Orange
collapses under tritanopia (6.6) or Orange / Gold under deuteranopia (10.6). So the classes are Magenta, Ultramarine,
Gold and Indigo, and Orange is free for the accent and for the markers you placed. Orange is the page's accent
already, so "orange = yours" still holds. The weak Ultramarine / Indigo pair is given to snare / other, because
"other" is the class you need to tell apart least. Indigo is taken a step darker (`#6a50e0`), which lifts that pair
to 14.7 under deuteranopia while kick / other stays at 15.1 or more. The three classes that matter most (kick, snare
and hat) are at least 23 apart in every vision type; under the old colours their worst pair was 12.2.

ok / warn / bad are Ultramarine / Gold / Magenta, a blue–yellow–magenta set, not the old red / green. The tints
were chosen so that ok stays apart from both bad and Brain (which sit beside it in the status line) in both themes.

## Simulation

`node tools/colors-check.mjs [--md]` reads the tokens from the page, simulates deuteranopia, protanopia and
tritanopia with the Machado, Oliveira & Fernandes (2009) matrices at severity 1.0 (in linear RGB), and prints
CIEDE2000 for every pair a user has to tell apart. A ΔE00 under 10 counts as weak. Each weak pair must also differ
by something other than colour (the last column), or the script fails. The table below is its output for this
commit (weak values in bold):

| group | pair | normal | deutan | protan | tritan | second cue |
|---|---|---:|---:|---:|---:|---|
| class | kick `#dc267f` / snare `#648fff` | 38.7 | 31.7 | 26.2 | 74.7 | name on the key cell; letter on the bar strip |
| class | kick `#dc267f` / hat `#ffb000` | 58.7 | 33.2 | 54.2 | 23.2 | name on the key cell; letter on the bar strip |
| class | kick `#dc267f` / other `#6a50e0` | 28.4 | 34.2 | 15.1 | 51.9 | name on the key cell; letter on the bar strip |
| class | snare `#648fff` / hat `#ffb000` | 59.6 | 65.2 | 60.4 | 60.9 | name on the key cell; letter on the bar strip |
| class | snare `#648fff` / other `#6a50e0` | 21.0 | 14.7 | 15.7 | 19.7 | name on the key cell; letter on the bar strip |
| class | hat `#ffb000` / other `#6a50e0` | 69.7 | 72.9 | 68.5 | 48.9 | name on the key cell; letter on the bar strip |
| lane | cv-orange / cv-marker | 30.0 | 24.2 | 23.7 | 28.2 | untouched markers dotted, hollow handle; dotted key border |
| lane | cv-rec / cv-orange | 25.3 | 10.6 | 18.0 | 18.1 | a recording is labelled with its key, a marker m<n> |
| status (dark) | ok-ink / bad-ink | 29.5 | 15.0 | 15.3 | 40.1 | the words |
| status (light) | ok-ink / bad-ink | 32.4 | 26.3 | 14.9 | 59.3 | the words |
| status (dark) | ok-ink / brain-ink | 17.6 | 16.8 | 17.4 | 17.3 | the words |
| status (light) | ok-ink / brain-ink | 15.5 | 11.1 | 12.9 | 17.0 | the words |
| scope | out R `#648fff` / in L `#a796f5` | 12.9 | **7.5** | **4.2** | 17.5 | the input is dashed |

This is a summary. Run the script for all 53 pairs: every class, lane, overlay, status and scope pair. With the
old colours the same script finds 9 weak pairs, one with nothing else to tell it by (J3 orange against J6 red on the
scope, 5.5 under deuteranopia), and kick / snare (red / green) at 15.6 under deuteranopia. Now it finds one weak pair,
and that pair has a second cue.

## Not by colour alone

What carries a meaning besides its colour:

- **Classes.** The key cells always name the class (`0.375 s · hat`). The Play tab's bar strip letters each step
  K / S / H / O (new; drawn when a step is at least 9 px wide). The ring beside it shows the same 16 steps as the
  strip. LOOPVIEW: Play's free loop on the same strip puts each class in its own **lane** (other · hat · snare · kick,
  top to bottom; on the ring, its own track) and letters a mark at least 11 px wide; a reversed hit is a **◀ wedge**, a
  ratchet is **split in two**, a held note is a **bar to its release** — and the newest pass is told from the older
  ones by **brightness** (full / 40 %), which the line under the bar also counts in words.
- **Marker kinds.** A marker you placed or moved is orange and solid. An untouched marker is grey and is now drawn
  **dotted** in the marker lane, with a **hollow** handle (new). Its key cell already had a dotted border, and the
  legend's swatch is now a dotted box to match. Recordings are labelled with their key, markers with `m<n>`, and grid
  boundaries live in the top lane.
- **Calls.** A called step's outline on the strip is now **dashed**; the current step's outline is solid white.
- **Status.** Every ok / warn / bad / Brain state is a word ("clock locked", "clock following", "page Brain", the
  error sentence). A drop that would orphan a recording also fills the cell and opens a prompt. The Keys note said
  "red" and now says "magenta".
- **The buffer bar.** The break and the recordings were "other" and "snare" (meaningless there, and the weakest
  pair). They are now the wave's neutral and the recording gold, in that order left to right, and labelled below.
- **Scope traces.** These are told apart by line style as well as colour: the outputs are solid, the inputs (J1 / J2)
  are in long dashes and the jacks J3–J8 are in short dashes (`SC_DASH`). The channel buttons name every trace.

## The scope's ten channels

| channel | colour | line |
|---|---|---|
| out L | neutral `#e8e6df` | solid |
| out R | Ultramarine `#648fff` | solid |
| in L | pale Indigo `#a796f5` | long dash |
| in R | Gold `#ffb000` | long dash |
| J3 (clock in) | pale Gold `#ffd473` | short dash |
| J4 (v/oct in) | pale Indigo `#a796f5` | short dash |
| J5 (gate / style in) | neutral `#e8e6df` | short dash |
| J6 (kick gate) | Magenta `#dc267f` = kick | short dash |
| J7 (snare gate) | Ultramarine `#648fff` = snare | short dash |
| J8 (hat gate) | Gold `#ffb000` = hat | short dash |

Ten channels with five hues means some colours repeat, so every repeat is in a different line style. The traces the
presets show together are all distinct in colour: Audio (out L / out R), CV (J3, J4, J5, and J6 in the Brain map)
and Gates (out L, J6, J7, J8). J6–J8 keep the class colours. Out R is not orange, because the "on" channel buttons
are orange and its swatch would vanish on them.

## Left as they are

- `<meta name="theme-color">` and the manifest's `theme_color` / `background_color` are `#0e0e10`, the dark `--bg`.
  They are neutral, and the manifest cannot read a CSS variable.
- `web/icons/icon-*.png` still use the old orange (`#eb7414`-ish) on near-black. That pair is colour-blind safe;
  re-rendering the icon is a taste call.
- `demo.js`'s `pageColors` stand in for the firmware descriptor's page colours (the module's LEDs). The page does
  not draw them.
