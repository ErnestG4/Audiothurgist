#!/usr/bin/env bash
# Publish web/ to Codeberg Pages: rebuilds the `pages` branch from the web/
# subtree of HEAD and prints the push. Codeberg serves a repo's `pages`
# branch at https://<user>.codeberg.page/<repo>/ over https — a secure
# context, which Web Serial (desktop) and WebUSB (Android) both require.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
git branch -D pages 2>/dev/null || true
git subtree split --prefix=web -b pages >/dev/null 2>&1
echo "pages branch rebuilt at $(git rev-parse --short pages) from $(git rev-parse --short HEAD)"
echo "now: git push -f origin pages    → https://combust.codeberg.page/Audiothurgist/"
