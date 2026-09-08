# tools/web — search and fetch from the shell

For agents (and people) when the built-in WebSearch / WebFetch tools are
rate-limited. No API keys.

    tools/web/search "erica synths sample drum review slice" 8
    tools/web/fetch https://www.ericasynths.lv/products/sample-drum/
    tools/web/fetch https://www.reddit.com/r/modular/... --js      # JavaScript-rendered pages
    tools/web/fetch URL --raw                                       # the HTML itself

`search` is DuckDuckGo via the `ddgs` package; `fetch` is curl + trafilatura's
readable-text extraction, falling back to Playwright's headless Chromium when
a page comes back empty. Setup (once): `uv venv .venv && uv pip install
--python .venv/bin/python ddgs playwright trafilatura && .venv/bin/python -m
playwright install chromium`. Chromium needs the usual system libraries
(`sudo apt install libnss3 libatk-bridge2.0-0 libxkbcommon0 libgbm1
libasound2t64`); `lynx -dump -nolist URL` is a fine no-Python alternative
for plain pages. The venv is gitignored.
