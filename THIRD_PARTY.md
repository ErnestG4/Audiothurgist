# Third-party code and attribution

Audiothurgist is AGPL-3.0. The components below are used under their own
licenses; each is credited here, in the interactive manual, and at the point
of use in the source.

## capicola — Matthew Nielsen (Heavylight Industries)

- **What:** the keyframe (extrema-sampling) DSP core: `Analyzer`, `SparseLine`,
  `Granule`, `KeyframeRecorder`, the TKEO transient `Detector`, `DeluxeLine`
  (B-spline/Hermite reads with derivatives and the Teager-Kaiser energy
  operator), Zavalishin ZDF filters, tabulated shapers.
- **Where:** `vendor/capicola` (git submodule, pinned; his LICENSE, README
  and AI policy travel with it). Include path `vendor/capicola/lib`.
- **License:** GNU AGPL-3.0 — compatible with ours. Modified copies of
  capicola shipped in hardware must offer corresponding source; we vendor it
  unmodified and pin the commit.
- **Paper:** M. Nielsen, "Keyframe Time Stretching via Extrema Sampling",
  *Proc. 29th Int. Conf. Digital Audio Effects (DAFx26)*, Cambridge MA, 2026.
  CC-BY-4.0. A copy is in `docs/`.
- **Authorship:** the engine is Matthew's human-authored work (see the AI
  policy in his README). Audiothurgist uses it as a library and does not
  re-derive it. What we use, and how, is listed in `docs/keyframe-lessons.md`.
- **Status:** vendored 2026-09-05 at commit `f2b7415` with Matthew's full
  permission and encouragement. Components in use:
  - `Analyzer.h` + `SparseLine.h` + `DeluxeLine.h` — offline extrema
    analysis at load for the **Density** classifier lens (keyframes per
    step as a bandwidth estimate; `docs/keyframe-lessons.md` L2).

## Hermetic Modular alchemy-sdk, libDaisy, DaisySP-LGPL

Sibling checkouts referenced by the Makefile (`../alchemy-sdk`,
`../DaisySP/DaisySP-LGPL`); see their own licenses. `reverbsc` is from
DaisySP-LGPL.

## Google web-serial-polyfill

`web/vendor/web-serial-polyfill/serial.js` is version 1.0.15 of Google's
Web Serial API polyfill on top of WebUSB, copied unmodified from the npm
package (Apache-2.0; the license travels with it). The web app loads it
only on browsers with WebUSB and no Web Serial — Chrome for Android — so a
phone or tablet can drive the module over a USB OTG cable.

## Mutable Instruments Grids

`core/bb_grids_map.h` carries the Grids drum map and node tables verbatim
(Émilie Gillet, MIT), with the `U8Mix` / bilinear lookup reproduced faithfully.
