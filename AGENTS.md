# Farview — agent rules

A 100% pure-Pion browser engine: pages run on a **host** (V8 + `curl-impersonate`), and a **client** lays them out and
draws them with `invar` UI/Metal on the person's device. Read `README.md` for the tab/agent/frame-port protocol and env
vars; this file holds only what you must not get wrong.

## Layout of the repository

- `pion/packages/` — pure-Pion workspace packages:
  - Portable core shared by host and client: `html`, `css`, `layout`, `woff2`, `brotli`, `url`, `display`, `doc` (the
    synced DOM), `proto` (the wire), `fvpaint`, `intmap`, `fvdmath`, `stage` (draws a tab as `invar` UI nodes).
  - Host runtime packages: `page` / `page_js` / `page_mid` / `page_aux` (DOM, scripts, prelude), `js` (V8 bridge),
    `fetch` and `net` (`curl-impersonate`), `hostfont` (the host's `FontBackend`), `raster` (`invar/skia` + `invar/pixels`),
    `wire`, `media`, `webauthn`, `pdf_export`, `agent_cli`, `host_core`, `farview_core`.
- `pion/cmd/` — 10 native binaries: `host`, `agent`, `app` (Farview's own `invar` desktop client), `wpt`, `layout`,
  `probe`, `docshot`, `html5lib`, `jsprobe`, `unpack`.
- `native/` — shared C/C++ shims linked by host packages (`net`, `fetch`, `js`, `hostfont`, `page`).
- `js/prelude.js`, `js/editing.js` — the page-side JS sources (see below).
- `deps/` — gitignored external dependencies fetched by `tools/deps_pion.sh` (`deps/invar`, `deps/curl-impersonate`)
  and `deps/node` (conda-forge `nodejs=24.*` `libnode`), plus `deps/icon_pack` written by `tools/deps_icons.sh`.
- Icons: shell_ui draws every icon through `shell_icon_shapes` (`pion/packages/shell_ui/icons.pion`) from
  `deps/icon_pack`: Central Icons (Iconists, licensed, `FV_ICONS=central`) in Farview's own builds, invar's MIT icons
  otherwise, by the same names (`tools/icon_map.txt`). Never commit Central Icons path data to this public repository.
- `tests/` — `html5lib`, `sites` (33 Chrome layout snapshots), `pages`, `host` (hand-made pages for host checks).
- `tools/` — check, packaging (`tools/package_pion.sh`), dependency (`tools/deps_pion.sh`), and bench scripts.

## Architecture red lines

- **Portable core stays portable**: `html`, `css`, `layout`, `woff2`, `brotli`, `url`, `display`, `doc`, `proto`,
  `fvpaint`, `stage` must not depend on host V8/`fetch`/OS-only stubs. Fonts reach layout only through
  `layout::FontBackend` (host: `hostfont`; client: `Fonts::new_portable([], [])` + `Stage::add_font`). Two documents in
  one process each bind their own `Fonts`.
- **Never move to "host paints `Op[]` / video frames, client replays them".** The host syncs the authoritative DOM,
  stylesheets, CSSOM, iframes, selection and resources; the client runs `css` + `layout` itself and draws native `invar`
  nodes, keeping interaction state and form editing local.
- **Pure Pion**: the repository is 100% pure Pion (72 `pion.toml` manifests across `pion/packages/` and `pion/cmd/`).

## Generated code

- `js/prelude.js` and `js/editing.js` are the only sources. After editing either, run
  `python3 tools/gen_prelude.py`, which rewrites `pion/packages/page_js/prelude.pion` and
  `pion/packages/page_js/editing.pion` (and runs `node --check`). Never hand-edit those two `.pion` files; commit them
  together with the JS.
- An internal entry the host calls in the page must be registered in the `hostOnly` list near the end of
  `js/prelude.js` (it is removed from `globalThis` and called through the hidden handle).
- Other generators (`tools/gen_entities.py`, `tools/gen_brotli.py`, `tools/gen_hyph.py`) work the same way: change the
  generator or its input, not the output.

## Layout, DOM and script invariants

- **Incremental layout**: passes before/after layout visit only boxes that changed — never add a whole-tree walk.
- **Typed style reads on hot paths**: use the integer-property API (`word_id`, `length_id`, …) in
  `pion/packages/css/cascade.pion`; do not call `Style::word("name")` (string lookup) in layout/paint hot code.
- **Style invalidation**: matching is cached per element `uid`; every new host op that mutates the DOM must call
  `touch_at(...)` or `touch()` (`pion/packages/page/page.pion`). Check with `tests/host/restyle.html`.
- **No reference cycles / leaks**: break parent↔child links of trees detached from the document; release the context
  and module caches of a removed `<iframe>`; never hang cross-layout caches on the box itself.
- **V8**: same-origin frame access relies on `fv_js_set_origin` using an *internalized* string as the security token;
  custom elements upgrade only in the main document's non-inert tree; `keepalive`/`sendBeacon` do not count toward
  `network_busy`; each callback runs under a watchdog (`FV_SCRIPT_LOG=1` prints the stack).

## Build, test and package

```sh
sh tools/deps_pion.sh                           # fetches pinned deps/invar + deps/curl-impersonate
pionc fmt --check --json pion/packages pion/cmd # formatting check
pionc check --json pion/packages                # fast typecheck across 62 library + 10 cmd = 72 manifests
pionc test --deps --json pion/packages          # full workspace unit test suite (1,577 tests)
pionc build pion/cmd/host -o out/pion-host      # build native binary (host/agent/app/wpt/layout/...)
bash tools/package_pion.sh                      # self-contained macOS/Linux release tarball in dist/
```

Checks (see each script's header for what it asserts):

- Host E2E: `tools/{agent,boundary,bytes,canvas_anim,files,media,mse_hls,oauth,passkeys,pdf_audio,realtime,storage,tab}.sh`.
- Layout vs Chrome (33 sites): `./run.sh [failures] [sites]`, `tools/first.py`, `tools/compare.py`.
- WPT: serve locally with `tools/wpt-serve.sh <dir>` and run `tools/wpt.py` against it (do not depend on wpt.live);
  a change must not regress any file.
- **Before merging a layout change**: unit tests; `tools/incremental.sh` (incremental relayout == full layout, box by
  box and paint); interaction pages under `FV_CHECK_LAYOUT=1 FV_CHECK_PAINT=1`; Speedometer 3 via `tools/sp3.sh`;
  layout WPT dirs vs `main`; `tools/memory.sh` (no growth).

## Performance

- A/B on `instructions retired` from `/usr/bin/time -l`, not wall-clock (build machines are noisy). To find a
  regression in a large PR, build each commit and bisect on instruction counts.
- Profile with the host's own sampler `FV_SAMPLE=<file>` (macOS; `native/fetch/fetch.c`) rather than the system
  `sample`; `FV_PROF=1` prints per-phase times. `FV_NO_FORK=1` keeps the host in one process for a debugger.
