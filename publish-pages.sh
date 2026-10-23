#!/usr/bin/env bash
# Publish web/ to GitHub Pages: rebuilds the `pages` branch from the web/
# subtree of HEAD, stamps it, and prints the push. GitHub serves this repo's
# `pages` branch at https://ernestg4.github.io/Audiothurgist/ over https — a secure
# context, which Web Serial (desktop) and WebUSB (Android) both require.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
git branch -D pages 2>/dev/null || true
git subtree split --prefix=web -b pages >/dev/null 2>&1
# Stamp the published copy with the commit it came from (Kyklophoria, 2026-09-23):
# the footer's "page" field, and the scripts loaded as <name>.js?v=<commit> so a
# browser holding an old wire layer in its cache cannot pair it with a new page.
# A page that "never started working" was indistinguishable from a page that
# never arrived; now the footer says which one you have. sw.js stays network
# first, and its offline fallback matches ignoring the query.
stamp=$(git rev-parse --short HEAD)
wt=$(mktemp -d)
git worktree add -q "$wt" pages
sed -i "s|<!--page-build--><b>local</b>|<!--page-build--><b>$stamp</b>|" "$wt/index.html"
for s in hostlink edit demo; do
  sed -i "s|<script src=\"$s.js\"></script>|<script src=\"$s.js?v=$stamp\"></script>|" "$wt/index.html"
done
ok=1
grep -q "<b>$stamp</b>" "$wt/index.html" || ok=0
for s in hostlink edit demo; do grep -q "$s.js?v=$stamp" "$wt/index.html" || ok=0; done
if [ "$ok" != 1 ]; then echo "stamp failed"; git worktree remove --force "$wt"; exit 1; fi
git -C "$wt" commit -q -am "Page build $stamp"
git worktree remove "$wt"
echo "pages branch rebuilt at $(git rev-parse --short pages) from $stamp (stamped)"
echo "now: git push --force-with-lease github pages    → https://ernestg4.github.io/Audiothurgist/"
