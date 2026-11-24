# Farview

A remote-rendering browser engine written in Pion (with C/C++ FFI shims in the host runtime): web pages execute on a **host** (V8 + `curl-impersonate`), while a **client** lays them out and draws them natively with [`AFK-surf/invar`](https://github.com/AFK-surf/invar).

Instead of streaming video frames or opaque draw commands, the host synchronizes the authoritative DOM, stylesheets, CSSOM, sub-`<iframe>`s, selection, and font/image assets over a compact binary wire protocol (`pion/packages/proto`). The client runs `css` + `layout` locally and draws native `invar` UI nodes, keeping scrolling, hover/transitions, and form editing local.

## Architecture

- **`pion/packages/`** — pure-Pion workspace packages (72 package manifests total including `pion/cmd/`):
  - **Portable core** (shared by host and client, zero host V8/`fetch`/OS-only stubs): `html`, `css`, `layout`, `woff2`, `brotli`, `url`, `display`, `doc` (synced DOM), `proto` (wire protocol), `fvpaint` (`paint/`), `intmap`, `fvdmath` (`dmath/`), `stage` (draws a tab as `invar` UI nodes).
  - **Host runtime**: `page` / `page_js` / `page_mid` / `page_aux` (DOM, scripts, prelude), `js` (V8 bridge via `libnode`), `fetch` and `net` (`curl-impersonate`), `hostfont` (host `FontBackend`), `raster` (`invar/skia` + `invar/pixels` + `invar/svg` + `invar/font`), `wire`, `media`, `webauthn`, `pdf_export`, `agent_cli`, `host_core`, `farview_core`.
- **`pion/cmd/`** — 10 native binaries: `host`, `agent`, `app` (Farview's `invar` desktop client), `wpt`, `layout`, `probe`, `docshot`, `html5lib`, `jsprobe`, `unpack`.
- **`native/`** — C/C++ shims linked by host packages (`net`, `fetch`, `js`, `hostfont`, `page`).
- **`js/prelude.js`, `js/editing.js`** — page-side JavaScript sources, compiled into `pion/packages/page_js/{prelude,editing}.pion` via `python3 tools/gen_prelude.py`.
- **`tests/`** — `html5lib` test vectors, `pages` (self-contained layout test pages), `sites` (33 Chrome layout reference snapshots), `host` (hand-made pages for host E2E checks).

## Prerequisites

1. **Pion compiler (`pionc`)**: bootstrap from [`AFK-surf/pion`](https://github.com/AFK-surf/pion) (pinned in `.github/workflows/pion-ci.yml`).
2. **External native dependencies** (`deps/`, gitignored):
   - Run `sh tools/deps_pion.sh` to fetch pinned [`AFK-surf/invar`](https://github.com/AFK-surf/invar) and `lexiforest/curl-impersonate` v2.3.2 (with SHA-256 verification).
   - Provide shared Node.js 24 (`libnode`) in `deps/node/{include,lib}` (e.g., via conda-forge `nodejs=24.*`; see `.github/workflows/pion-ci.yml`).
3. **Linux system packages**: `patchelf`, `zlib1g-dev`, `fonts-dejavu-core`, `fonts-liberation2`, `libegl1`, `libgl1`, `libgl1-mesa-dri` (Ubuntu 24.04+ / `GLIBCXX_3.4.31` recommended for bundled `libnode`).

## Build, Test, and Package

```sh
# 1. Fetch pinned deps/invar + deps/curl-impersonate
sh tools/deps_pion.sh

# 2. Format check & typecheck
pionc fmt --check --json pion/packages pion/cmd
pionc check --json pion/packages
for c in host wpt agent layout probe html5lib jsprobe unpack; do
  pionc check --json "pion/cmd/$c"
done

# 3. Full unit test suite across all workspace packages (1,577 tests)
pionc test --deps --json pion/packages

# 4. Build native binaries
mkdir -p out
pionc build pion/cmd/host -o out/pion-host
pionc build pion/cmd/agent -o out/pion-agent
pionc build pion/cmd/html5lib -o out/pion-html5lib
pionc build pion/cmd/layout -o out/pion-layout
# macOS desktop client:
pionc build pion/cmd/app -o out/pion-app

# 5. Run html5lib parser conformance tests
out/pion-html5lib tests/html5lib/tree-construction/*.dat

# 6. Build self-contained release tarball in dist/
bash tools/package_pion.sh
```

## Tab, Agent, and Frame-Port Protocol

`out/pion-host --tab <port> <url>` runs a persistent tab backed by V8 and `curl-impersonate`. Every port listens on `127.0.0.1` by default (`FV_BIND` overrides). `tools/tab.sh` exercises the protocol end to end.

- **Ports**: Farview clients connect on `<port>` (multiple simultaneous clients; an interacting person takes the tab from an agent), the agent CLI connects on `FV_AGENT_PORT` (default `port + 1`), and the frame port listens on `FV_FRAME_PORT` (default `port + 2`). Passing port `0` picks a free ephemeral port.
- **State**: `FV_STATE_DIR` stores `url`, `storage.json` (`localStorage`), `cookies.txt` (Netscape format), and `host.json` (`{"pid","port","agent","frame"}`). `FV_TAB_SIZE=WxH` sets the initial CSS viewport (default `1280x800`).
- **Agent CLI** (`out/pion-agent 127.0.0.1 <agent_port> <step>...`):
  `look`, `click:sel`, `fill:sel=words`, `submit:sel`, `key:k[@sel]`, `go:address`, `back`, `forward`, `reload`, `wait:ms`, `eval:script` (JSON value; awaits promises up to 30 s), `shot:file.png`, `cookies:get=<url|domain>`, `cookies:set=<JSON>`, `cookies:clear=<domain>`, `passkeys:get[=<domain>]`, `passkeys:set=<JSON>`, `passkeys:clear=<domain>`, `upload:sel=file[,file...]`, `downloads`.
- **Passkeys**: built-in platform authenticator (`pion/packages/webauthn` + `pion/packages/page`) supporting ES256 and EdDSA (`attestation: "none"`) on HTTPS or `localhost`, persisted in `passkeys.json` under `FV_PROFILE_DIR` (or `FV_STATE_DIR`). Private keys never leave the host. Verified by `tools/passkeys.sh`.
- **Downloads**: `download` links (`data:`, `blob:`, HTTP/HTTPS) and `Content-Disposition: attachment` responses are saved into `FV_DOWNLOAD_DIR` (default `$FV_STATE_DIR/downloads`). Verified by `tools/files.sh`.
- **Frame port** (newline-delimited JSON in both directions):
  - **To host**: `{"size":{"width":W,"height":H,"scale":S}}`, `{"frame":{"format":"jpeg"|"png"|"rgba","quality":70}}`, `{"pointer":{"type":"down"|"move"|"up","x","y","button","buttons"}}`, `{"wheel":{"x","y","dx","dy"}}`, `{"key":{"type":"down"|"up","key","code","mods"}}` (CDP modifier bitmask: 1 Alt, 2 Ctrl, 4 Meta, 8 Shift), `{"text":"..."}`.
  - **From host**: `{"changed":seq}`, `{"size":...}`, `{"page":{"url","title"}}`, `{"taken":"agent"|"person"}`, `{"openTab":...}` (with `FV_NEW_TABS`), `{"closeTab":true}`.
- **Multi-window profiles** (`FV_NEW_TABS` + `FV_PROFILE_DIR`): `window.open` / `window.opener` / `postMessage` / `window.close()` communicate across tab processes via `ipc/<tab_id>/` (`FV_TAB_ID`, `FV_OPENER_ID`, `FV_OPENER_ORIGIN`). Verified by `tools/oauth.sh`.

## Additional Checks

- **Layout vs Chrome**: `./run.sh [failures] [sites...]` compares `out/pion-layout` against the 33 local snapshots in `tests/sites/`; `tools/first.py <site> [from]` prints the first diverging boxes.
- **Host E2E scripts**: `tools/{agent,boundary,canvas_anim,files,media,mse_hls,oauth,passkeys,pdf_audio,realtime,storage,tab}.sh` (note: `tools/bytes.sh` and `tests/flows.txt` fetch live public URLs and require internet access).
- **WPT**: serve locally with `tools/wpt-serve.sh <dir>` and run `python3 tools/wpt.py <baseline.json.gz> <out.json> <dir>...`.

## License

Farview's own source code is licensed under the **MIT License** — see [`LICENSE`](LICENSE).

Third-party tables, specifications, test suites, and build-time/runtime dependencies retain their respective copyright notices and license terms — see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and [`LICENSES/`](LICENSES/). In particular:

- `pion/packages/cookie/src/psl_data.pion` and `pion/packages/cookie/src/psl_index.pion` are derived from the Mozilla Public Suffix List and remain licensed under **MPL-2.0** (file-level copyleft; see `LICENSES/MPL-2.0.txt`).
- `pion/packages/layout/hyph_tables.pion` embeds Kuiken's `hyph-en-us` patterns (`FSFAP-no-warranty-disclaimer`; see `LICENSES/hyph-en-us.txt`).
- Binary distributions produced by `tools/package_pion.sh` bundle `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `LICENSES/` alongside runtime dependencies (`AFK-surf/invar`, `AFK-surf/pion`, `curl-impersonate`, Node.js 24 `libnode`, SQLite).
