# Audiothurgist — web frontend

`index.html` is the instrument's face in a browser: a live view of the bar,
per-slice waveforms you can select and punch into steps, an output scope,
and dice / reslice / mute / flow / history / sample controls. It speaks
**HostLink over Web Serial** to the module's front USB-C — the same protocol
Hermetic's `/program` uses — plus the firmware's `StateExt` (commands
0x60–0x6F, v0.7.0+). No dependencies, one file, phone-friendly.

## Running it

Web Serial needs a **secure context** and a Chromium browser (Chrome, Edge,
Opera). `file://` will not work. Either:

- **Locally:** `cd web && python3 -m http.server 8080` → http://localhost:8080
- **Hosted:** publish `web/` on Codeberg Pages (a `pages` branch, or a repo
  named `pages`) → https://combust.codeberg.page/Audiothurgist/

Close the `/program` tab first — one page holds the serial port at a time.
Press **Connect**, pick the Alchemy Lab (0483:5740), and the bar starts moving.

## What talks to what

| Page needs            | HostLink command        | Notes                                   |
|-----------------------|-------------------------|-----------------------------------------|
| identity, max body    | `0x01 HELLO`            | standard                                |
| option labels, files  | `0x02 GET_DESCRIPTOR`   | reads the module's own descriptor JSON  |
| bar / playhead / meta | `0x60 GET_STATE`        | ~14 Hz                                  |
| waveform              | `0x61 GET_ENV`          | refetched when the material changes     |
| scope                 | `0x62 SCOPE`            | 256 samples, ×4 decimated               |
| buttons, punch, scrub | `0x63 ACTION`           | dice·reslice·mute·punch·flow·scrub·load |

Flashing, presets and settings stay in `/program`; this page is deliberately
the *instrument* view only.
