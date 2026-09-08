# web-serial-polyfill (vendored)

Google's Web Serial API polyfill on top of WebUSB, version **1.0.15**,
`dist/serial.js` copied unmodified from the npm package
(https://github.com/google/web-serial-polyfill). Apache-2.0, see LICENSE.

Audiothurgist loads it only when the browser has `navigator.usb` but not
`navigator.serial` — Chrome on Android — so a phone or tablet with a USB
OTG cable can drive the module from the same page. On desktop Chromium the
native Web Serial API is used and this file is never touched.
