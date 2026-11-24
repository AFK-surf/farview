# Third-Party Notices

Farview's own source code is licensed under the MIT License (see `LICENSE`).
The following third-party components, tables, specifications, and test suites
included in this repository or linked into distributed binaries retain their
original copyright notices and license terms. Full license texts are stored in
`LICENSES/`.

## Source Distribution Components

| Component & Repository Paths | Upstream & Version / Commit | Original Copyright | Modifications / Use | SPDX | License Text |
|---|---|---|---|---|---|
| Vendored Pion packages (`pion/packages/{cookie,csp,css_color,dash,encoding,exif,gif,hls,http_cache,icc,idna,mime,opentype,structured_fields,unicode_linebreak,unicode_segment,webp,webvtt,wgsl}`) | `AFK-surf/pion-packages` (vendored snapshots; see per-package `pion.toml`) | Copyright (c) 2026 AFK-surf contributors | Vendored and adapted for Farview host/client integration | `MIT` (plus per-package third-party tables below) | `LICENSES/MIT.txt` |
| Mozilla Public Suffix List (`pion/packages/cookie/src/psl_data.pion`, `pion/packages/cookie/src/psl_index.pion`) | `publicsuffix/list` @ `763a4c200052` | Mozilla Foundation and Public Suffix List contributors | Normalized to ASCII/Punycode, sorted, and indexed into Pion string tables. **File-level copyleft**: these two files remain under MPL-2.0; source availability for these files is maintained in this repository. | `MPL-2.0` | `LICENSES/MPL-2.0.txt` |
| Public Suffix List Test Vectors (`pion/packages/cookie/src/cookie_test.pion`) | `publicsuffix/list` (`tests/test_psl.txt`) | Public Suffix List contributors (dedicated to the public domain via CC0 1.0) | Transcribed into Pion cookie/PSL unit test assertions | `CC0-1.0` | `LICENSES/CC0-1.0.txt` |
| Unicode Character Database & IDNA/Security data (`pion/packages/idna/src/tables_uts39.pion`, `pion/packages/idna/src/tables_uts46.pion`, `pion/packages/idna/src/nfc.pion`, `pion/packages/cookie/src/unicode_noncanon.pion`) | Unicode Consortium, UCD / UTS #46 / UTS #39 (IDNA: Unicode 16.0.0) | Copyright © 1991-2026 Unicode, Inc. | Converted to Pion lookup tables | `Unicode-3.0` | `LICENSES/Unicode-3.0.txt` |
| Unicode 16.0.0 UAX #29 Segmentation Tables (`pion/packages/unicode_segment/src/tables.pion`) | Unicode Consortium, Unicode 16.0.0 UCD (`GraphemeBreakProperty.txt`, `WordBreakProperty.txt`, `SentenceBreakProperty.txt`, `emoji-data.txt`, `DerivedCoreProperties.txt`, `EastAsianWidth.txt`) | Copyright © 1991-2026 Unicode, Inc. | Converted to Pion UAX #29 extended grapheme, word, and sentence lookup tables | `Unicode-3.0` | `LICENSES/Unicode-3.0.txt` |
| Unicode 16.0.0 UAX #14 Line Breaking Tables (`pion/packages/unicode_linebreak/src/tables.pion`) | Unicode Consortium, Unicode 16.0.0 UCD (`LineBreak.txt`, `EastAsianWidth.txt`, `UnicodeData.txt`, `emoji-data.txt`) | Copyright © 1991-2026 Unicode, Inc. | Converted to Pion UAX #14 line breaking property and rule lookup tables | `Unicode-3.0` | `LICENSES/Unicode-3.0.txt` |
| WHATWG HTML Named Character References, WebVTT Entities, Encoding Indices & HTML Rendering UA Styles (`pion/packages/html/entities.pion`, `pion/packages/webvtt/src/entities_table.pion`, `pion/packages/encoding/src/tables.pion`, `pion/packages/css/ua.pion`) | WHATWG (`html.spec.whatwg.org/entities.json`, `whatwg/encoding`, HTML Standard Rendering section) | Copyright © WHATWG (Apple, Google, Mozilla, Microsoft) | Encoded into Pion lookup tables (`tools/gen_entities.py`) and user-agent default style rules | `BSD-3-Clause` | `LICENSES/WHATWG-Encoding-LICENSE.txt`, `LICENSES/BSD-3-Clause.txt` |
| Web Platform Tests (WPT) Editing Implementation (`js/editing.js`, `js/editing.LICENSE.md`, generated `pion/packages/page_js/editing.pion`) | `web-platform-tests/wpt` (`editing/include/implementation.js`) | Copyright © web-platform-tests contributors | Null-handling adjustments and embedding via `tools/gen_prelude.py` | `BSD-3-Clause` | `LICENSES/WPT-BSD.txt`, `js/editing.LICENSE.md` |
| Web Platform Tests (WPT) Ported Test Cases (`pion/packages/css_color/src/wpt_test.pion` @ `6a5adcd6`, `pion/packages/webvtt/webvtt_wpt_conformance_test.pion`, `pion/packages/cookie/src/cookie_test.pion`, `pion/packages/encoding/src/encoding_test.pion`) | `web-platform-tests/wpt` | Copyright © web-platform-tests contributors | Test vectors transcribed into Pion unit tests | `BSD-3-Clause` | `LICENSES/WPT-BSD.txt` |
| html5lib Test Suite (`tests/html5lib/`) | `html5lib/html5lib-tests` | Copyright (c) 2006-2013 James Graham, Geoffrey Sneddon, and other contributors | Unmodified tokenizer and tree-construction `.dat` test vectors | `MIT` | `LICENSES/html5lib-MIT.txt`, `tests/html5lib/LICENSE` |
| Brotli Static Dictionary, Transforms & Context LUTs (`pion/packages/brotli/tables.pion`) | `google/brotli` | Copyright (c) 2009, 2010, 2013-2016 by the Brotli Authors | Converted to Pion tables via `tools/gen_brotli.py` | `MIT` | `LICENSES/Brotli-MIT.txt` |
| US English Hyphenation Patterns (`pion/packages/layout/hyph_tables.pion`) | `hyphenation/tex-hyphen` (`hyph-utf8` / `hyph-en-us`) | Copyright (C) 1990, 2004, 2005 Gerard D.C. Kuiken | Transformed into Pion string tables via `tools/gen_hyph.py` with original copyright and permission notice preserved | `FSFAP-no-warranty-disclaimer` | `LICENSES/hyph-en-us.txt` |
| RFC 6386 VP8 Normative Tables (`pion/packages/webp/src/vp8_tables.pion`) | IETF RFC 6386 (§20.26–§20.27 reference implementation) | Copyright (c) 2010, 2011, Google Inc. | Normative probability and quantization tables extracted into Pion; patent grant notice retained | `BSD-3-Clause` | `LICENSES/RFC6386-license-and-patents.txt` |
| Go Standard Library `mime` Test Vectors (`pion/packages/mime/{encoding,media_type,multipart}_test.pion`) | Go (`golang/go` `mime/multipart`) | Copyright (c) 2009 The Go Authors | Adapted into Pion unit tests | `BSD-3-Clause` | `LICENSES/Go-BSD.txt` |
| `ttf-parser` Semantic Test Suite (`pion/packages/opentype/src/opentype_test.pion`) | `harfbuzz/ttf-parser` v0.25.1 (dual MIT / Apache-2.0; MIT selected) | Copyright (c) 2018 Yevhenii Reizner | Semantic test cases ported to Pion | `MIT` | `LICENSES/ttf-parser-MIT.txt` |

## Runtime & Binary Distribution Dependencies (Fetched at Build Time, Not Vendored in Git)

Binary release archives produced by `tools/package_pion.sh` include `LICENSE`,
`THIRD_PARTY_NOTICES.md`, `LICENSES/` (including `Node.js-LICENSE.txt`,
`invar-NOTICE.txt`, `invar-THIRD_PARTY_NOTICES.md`, `invar/`, and
`curl-impersonate/`). Anyone redistributing compiled Farview binaries must
comply with the licenses of the runtime components linked or bundled into those
binaries:

- **`AFK-surf/invar`** (OpenType shaping, `skia`/`pixels`/`svg`/`font` rasterization, UI/Metal host): MIT License (plus third-party notices in `deps/invar/NOTICE`, `deps/invar/THIRD_PARTY_NOTICES.md`, and `deps/invar/LICENSES/`, including Apache-2.0 components — see `LICENSES/Apache-2.0.txt`).
- **`AFK-surf/pion`** (Pion compiler & C runtime `libpionrt.a`): MIT License.
- **`lexiforest/curl-impersonate` v2.3.2** (statically linked `libcurl-impersonate.a` for Chrome TLS fingerprinting): MIT (curl/curl-impersonate), OpenSSL/ISC/MIT (BoringSSL), MIT (nghttp2, nghttp3, ngtcp2, c-ares, brotli), BSD-3-Clause (zstd), Zlib (zlib), and **LGPL-3.0-or-later / GPL-2.0-or-later** (`libidn2` and `libunistring`). Bundled license files from the upstream archive are copied into `LICENSES/curl-impersonate/`. If redistributing statically linked binaries containing `libidn2`/`libunistring`, ensure compliance with LGPL relinking/corresponding-source obligations (or link `libidn2`/`libunistring` dynamically).
- **Node.js 24 (`libnode`)** (dynamically linked via conda-forge `nodejs=24.*`): MIT License (Node.js), BSD-3-Clause (V8), Unicode-3.0 (ICU), Apache-2.0 (OpenSSL 3 — see `LICENSES/Apache-2.0.txt`), MIT (libuv, uvwasi), Apache-2.0 (simdjson), and other components listed in `LICENSES/Node.js-LICENSE.txt`.
- **SQLite** (system or bundled `libsqlite3`): Public Domain (blessing).
- **System C/C++ runtime libraries**: On Linux, `tools/package_pion.sh` excludes `libstdc++.so*` and `libgcc_s.so*` from the archive so binaries use the host system's C/C++ runtime libraries (Ubuntu 24.04+ / `GLIBCXX_3.4.31` recommended for conda-forge Node.js 24) rather than redistributing conda's GPL runtime copies. On macOS, conda-forge builds may bundle `libc++.1.dylib` (LLVM Project, `Apache-2.0 WITH LLVM-exception`; see `LICENSES/Apache-2.0.txt`).
