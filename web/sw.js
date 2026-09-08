/* sw.js — the smallest service worker that makes the page installable
 * (home-screen icon on Android, and the requirement for a Trusted Web
 * Activity). Network first, cache as the fallback, so a republished page
 * is picked up on the next load and the last good copy opens when the
 * phone is offline. Serial traffic never goes through here. */
const CACHE = 'audiothurgist-v1';
const SHELL = ['./', './index.html', './hostlink.js', './edit.js', './demo.js', './manifest.webmanifest', './icons/icon-192.png', './icons/icon-512.png', './vendor/web-serial-polyfill/serial.js'];
self.addEventListener('install', e => { e.waitUntil(caches.open(CACHE).then(c => c.addAll(SHELL)).catch(() => {})); self.skipWaiting(); });
self.addEventListener('activate', e => { e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== CACHE).map(k => caches.delete(k))))); self.clients.claim(); });
self.addEventListener('fetch', e => {
  if (e.request.method !== 'GET' || new URL(e.request.url).origin !== self.location.origin) return;
  e.respondWith(fetch(e.request).then(r => { const copy = r.clone(); caches.open(CACHE).then(c => c.put(e.request, copy)).catch(() => {}); return r; })
    .catch(() => caches.match(e.request, { ignoreSearch: true }).then(r => r || caches.match('./index.html'))));
});
