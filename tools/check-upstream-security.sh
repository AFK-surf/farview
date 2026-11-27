#!/usr/bin/env bash
# Upstream security & dependency audit for Farview.
#
# Farview does not fork Chromium or V8: it pins upstream components and must notice quickly when one of them
# ships a security fix. This script reports, in one structured pass:
#
#   1. Pinned security-critical components (tools/deps.env, tools/deps_pion.sh, .github/workflows/):
#      pion, invar, curl-impersonate (bundles BoringSSL), Node.js / V8 (libnode), WebView2; checks that every
#      workflow agrees with tools/deps.env and that every curl-impersonate download is SHA-256 pinned.
#   2. Banned unsafe source patterns (each rule lists its reviewed exceptions):
#        shell-exec        system()/popen()/"sh -c"/ShellExecute outside the reviewed opener
#        plaintext-secret  writing password/passwd values to files or logs
#        glass-liquid      hand-rolled `Glass::liquid` anywhere outside an `lg` package
#        download-path     joining a download directory with an unsanitised remote filename
#   3. (--upstream, needs network) Upstream release currency: latest curl-impersonate release, Node.js 24 LTS
#      security releases newer than the pinned Windows libnode (and their V8 versions), pion/invar main heads.
#      Outdated components are reported as warnings; they fail only with --strict.
#
# Usage:
#   bash tools/check-upstream-security.sh                 # static audit of this checkout (exit 1 on violations)
#   bash tools/check-upstream-security.sh --upstream      # + upstream currency report (network)
#   bash tools/check-upstream-security.sh --upstream --strict   # outdated security releases fail too
#   bash tools/check-upstream-security.sh --root DIR      # audit another tree (no git needed)
#   bash tools/check-upstream-security.sh --self-test     # plant synthetic violations, assert each is caught
#
# Output is plain text; under GitHub Actions it also writes ::error::/::warning:: annotations and appends a
# Markdown summary to $GITHUB_STEP_SUMMARY.
set -u

ROOT=""
UPSTREAM=0
STRICT=0
SELF_TEST=0
while [ $# -gt 0 ]; do
  case "$1" in
    --root) ROOT="$2"; shift 2 ;;
    --upstream) UPSTREAM=1; shift ;;
    --strict) STRICT=1; shift ;;
    --self-test) SELF_TEST=1; shift ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "check-upstream-security: unknown argument $1" >&2; exit 2 ;;
  esac
done

SCRIPT_PATH="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/$(basename -- "$0")"
if [ -z "$ROOT" ]; then
  ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
fi

VIOLATIONS=0
WARNINGS=0
SUMMARY=""

note() { printf '%s\n' "$*"; SUMMARY="$SUMMARY$*
"; }
violation() {
  # $1 rule, $2 location, $3 message
  VIOLATIONS=$((VIOLATIONS + 1))
  printf '  VIOLATION [%s] %s: %s\n' "$1" "$2" "$3"
  SUMMARY="$SUMMARY- :x: **$1** \`$2\`: $3
"
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    file="${2%%:*}"
    line="${2#*:}"; line="${line%%:*}"
    case "$line" in ''|*[!0-9]*) line=1 ;; esac
    echo "::error file=$file,line=$line,title=security-audit $1::$3"
  fi
}
warn() {
  WARNINGS=$((WARNINGS + 1))
  printf '  WARN [%s] %s\n' "$1" "$2"
  SUMMARY="$SUMMARY- :warning: **$1**: $2
"
  if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::warning title=security-audit $1::$2"; fi
}

# ---------------------------------------------------------------------------------------------------------------
# Self-test: plant one violation per rule in a scratch tree and require every rule to fire, and a clean tree to pass.
# ---------------------------------------------------------------------------------------------------------------
if [ "$SELF_TEST" = 1 ]; then
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  mk_base() {
    d="$1"
    mkdir -p "$d/tools" "$d/.github/workflows" "$d/native/x" "$d/pion/packages/demo" "$d/pion/packages/shell_ui" \
      "$d/pion/packages/lg" "$d/pion/cmd/app"
    cat > "$d/tools/deps.env" <<'EOF'
PION_REV=1111111111111111111111111111111111111111
INVAR_REV=2222222222222222222222222222222222222222
CURL_IMPERSONATE_VER=v2.3.2
NODE_WIN_VER=v24.10.0
WEBVIEW2_VER=1.0.4258.31
EOF
    cat > "$d/tools/deps_pion.sh" <<'EOF'
CI_SHA256=0000000000000000000000000000000000000000000000000000000000000000
EOF
    printf 'env:\n  PION_REV: 1111111111111111111111111111111111111111\n' > "$d/.github/workflows/pion-ci.yml"
    printf 'int ok(void) { return 0; }\n' > "$d/native/x/clean.c"
    printf 'fn ok() -> Int {\n  1\n}\n' > "$d/pion/packages/demo/clean.pion"
    printf 'fn card() -> Unit {\n  Glass::liquid(1)\n}\n' > "$d/pion/packages/lg/glass.pion"
  }
  CLEAN="$TMP/clean"
  mk_base "$CLEAN"
  if ! bash "$SCRIPT_PATH" --root "$CLEAN" > "$TMP/clean.log" 2>&1; then
    cat "$TMP/clean.log"
    echo "self-test: FAIL (clean synthetic tree reported violations)"
    exit 1
  fi
  BAD="$TMP/bad"
  mk_base "$BAD"
  # shell-exec
  printf '#include <stdlib.h>\nvoid open_dl(const char *p) { char c[512]; snprintf(c, 512, "open %%s", p); system(c); }\n' > "$BAD/native/x/open.c"
  printf 'fn run(u : String) -> Unit {\n  spawn_cmd("/bin/sh -c curl " + u)\n}\n' > "$BAD/pion/packages/demo/shell.pion"
  # plaintext-secret
  printf 'fn save(w : World, password : String) -> Unit {\n  fs_write(w, "/tmp/creds.txt", password)\n}\n' > "$BAD/pion/packages/demo/creds.pion"
  printf 'void dump(FILE *f, const char *password) { fprintf(f, "pw=%%s", password); }\n' > "$BAD/native/x/log.c"
  # glass-liquid
  printf 'fn bar() -> Unit {\n  Glass::liquid(2)\n}\n' > "$BAD/pion/packages/shell_ui/bar.pion"
  # download-path
  printf 'fn place(dir : String, suggested_filename : String) -> String {\n  dir + "/" + suggested_filename\n}\n' > "$BAD/pion/cmd/app/downloads_place.pion"
  # pin drift
  printf 'env:\n  PION_REV: 9999999999999999999999999999999999999999\n' > "$BAD/.github/workflows/pion-win.yml"
  # unpinned download
  printf 'curl -L https://github.com/lexiforest/curl-impersonate/releases/download/v2.3.2/x.tar.gz\n' >> "$BAD/tools/deps_pion.sh"
  sed -i.bak 's/^CI_SHA256=.*$//' "$BAD/tools/deps_pion.sh" && rm -f "$BAD/tools/deps_pion.sh.bak"
  bash "$SCRIPT_PATH" --root "$BAD" > "$TMP/bad.log" 2>&1
  rc=$?
  fail=0
  if [ "$rc" = 0 ]; then echo "self-test: expected non-zero exit on synthetic violations"; fail=1; fi
  for want in "shell-exec] native/x/open.c" "shell-exec] pion/packages/demo/shell.pion" \
    "plaintext-secret] pion/packages/demo/creds.pion" "plaintext-secret] native/x/log.c" \
    "glass-liquid] pion/packages/shell_ui/bar.pion" "download-path] pion/cmd/app/downloads_place.pion" \
    "pin-drift] .github/workflows/pion-win.yml" "unpinned-download] tools/deps_pion.sh"; do
    if grep -qF "$want" "$TMP/bad.log"; then
      echo "self-test: caught  [$want"
    else
      echo "self-test: MISSED  [$want"
      fail=1
    fi
  done
  if grep -qF "glass-liquid] pion/packages/lg/" "$TMP/bad.log"; then
    echo "self-test: false positive on the lg package"; fail=1
  fi
  if [ "$fail" != 0 ]; then
    echo "----- audit output on the synthetic tree -----"
    cat "$TMP/bad.log"
    echo "self-test: FAIL"
    exit 1
  fi
  echo "self-test: OK (clean tree passes; 8/8 synthetic violations caught)"
  exit 0
fi

cd "$ROOT" || { echo "check-upstream-security: cannot cd to $ROOT" >&2; exit 2; }

# Files to scan: tracked files in a git checkout, otherwise everything under the tree (minus deps/ and .git/).
list_files() {
  if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 && [ "$(git -C "$ROOT" rev-parse --show-toplevel)" = "$(pwd -P)" ]; then
    git ls-files
  else
    find . -type f ! -path './.git/*' ! -path './deps/*' ! -path './.pion/*' ! -path './out/*' ! -path './tmp/*' | sed 's|^\./||'
  fi
}
FILES="$(list_files | LC_ALL=C sort)"
C_FILES="$(printf '%s\n' "$FILES" | grep -E '\.(c|cc|cpp|h|hpp|m|mm)$' | grep -E '^(native|pion)/' || true)"
PION_FILES="$(printf '%s\n' "$FILES" | grep -E '^pion/.*\.pion$' | grep -vE '_test\.pion$' || true)"

note "== Farview upstream security audit =="
note "root: $(basename "$ROOT")"
note ""

# ---------------------------------------------------------------------------------------------------------------
# 1. Pinned components
# ---------------------------------------------------------------------------------------------------------------
note "-- 1. Pinned security-critical components --"
env_get() { sed -n "s/^$1=//p" tools/deps.env 2>/dev/null | head -n 1; }
if [ ! -f tools/deps.env ]; then
  violation pin-missing "tools/deps.env" "single source of pinned versions is missing"
else
  PION_REV="$(env_get PION_REV)"
  INVAR_REV="$(env_get INVAR_REV)"
  CI_VER="$(env_get CURL_IMPERSONATE_VER)"
  NODE_WIN_VER="$(env_get NODE_WIN_VER)"
  WEBVIEW2_VER="$(env_get WEBVIEW2_VER)"
  NODE_SPEC="$(grep -rhoE "nodejs=[0-9]+\.\*" .github/workflows tools 2>/dev/null | head -n 1)"
  printf '  %-18s %s\n' "pion" "${PION_REV:-<unset>}"
  printf '  %-18s %s\n' "invar" "${INVAR_REV:-<unset>}"
  printf '  %-18s %s  (TLS: bundled BoringSSL)\n' "curl-impersonate" "${CI_VER:-<unset>}"
  printf '  %-18s %s  (macOS/Linux libnode, V8 follows Node)\n' "node/V8" "${NODE_SPEC:-<unset>}"
  printf '  %-18s %s  (Windows libnode, V8 follows Node)\n' "node/V8 (win)" "${NODE_WIN_VER:-<unset>}"
  printf '  %-18s %s\n' "WebView2 SDK" "${WEBVIEW2_VER:-<unset>}"
  SUMMARY="$SUMMARY
| component | pinned |
|---|---|
| pion | \`${PION_REV:-unset}\` |
| invar | \`${INVAR_REV:-unset}\` |
| curl-impersonate (BoringSSL) | \`${CI_VER:-unset}\` |
| Node.js / V8 (macOS, Linux) | \`${NODE_SPEC:-unset}\` |
| Node.js / V8 (Windows) | \`${NODE_WIN_VER:-unset}\` |
| WebView2 SDK | \`${WEBVIEW2_VER:-unset}\` |

"
  for key in PION_REV INVAR_REV CURL_IMPERSONATE_VER; do
    [ -n "$(env_get $key)" ] || violation pin-missing "tools/deps.env" "$key is not pinned"
  done
  for rev_key in PION_REV INVAR_REV; do
    v="$(env_get $rev_key)"
    if [ -n "$v" ] && ! printf '%s' "$v" | grep -qE '^[0-9a-f]{40}$'; then
      violation pin-floating "tools/deps.env" "$rev_key must be a full 40-hex commit, got '$v'"
    fi
  done
  # Every workflow that names a pinned revision must agree with deps.env.
  for wf in $(printf '%s\n' "$FILES" | grep -E '^\.github/workflows/.*\.ya?ml$'); do
    while IFS= read -r hit; do
      [ -n "$hit" ] || continue
      ln="${hit%%:*}"
      val="$(printf '%s' "${hit#*:}" | sed -E 's/.*PION_REV[:=][[:space:]]*"?([0-9a-f]+).*/\1/')"
      if [ -n "$PION_REV" ] && printf '%s' "$val" | grep -qE '^[0-9a-f]{40}$' && [ "$val" != "$PION_REV" ]; then
        violation pin-drift "$wf:$ln" "PION_REV $val disagrees with tools/deps.env ($PION_REV)"
      fi
    done <<EOF
$(grep -nE 'PION_REV[:=][[:space:]]*"?[0-9a-f]{40}' "$wf" 2>/dev/null || true)
EOF
  done
  # curl-impersonate downloads must be SHA-256 verified.
  if [ -f tools/deps_pion.sh ]; then
    if grep -q "curl-impersonate/releases/download" tools/deps_pion.sh && ! grep -qE 'SHA256=[0-9a-f]{64}' tools/deps_pion.sh; then
      violation unpinned-download "tools/deps_pion.sh:1" "curl-impersonate archives are downloaded without a SHA-256 pin"
    fi
  fi
fi
note ""

# ---------------------------------------------------------------------------------------------------------------
# 2. Banned unsafe patterns
# ---------------------------------------------------------------------------------------------------------------
note "-- 2. Banned unsafe source patterns --"
# scan RULE ALLOW_REGEX MESSAGE PATTERN FILES...   (ALLOW_REGEX matches "path:line:text" of reviewed exceptions)
scan() {
  rule="$1"; allow="$2"; msg="$3"; pat="$4"; shift 4
  [ $# -gt 0 ] || return 0
  hits="$(grep -nHE -- "$pat" "$@" 2>/dev/null || true)"
  [ -n "$hits" ] || return 0
  while IFS= read -r h; do
    [ -n "$h" ] || continue
    if [ -n "$allow" ] && printf '%s' "$h" | grep -qE -- "$allow"; then continue; fi
    # ignore comment-only lines
    text="${h#*:*:}"
    case "$(printf '%s' "$text" | sed -E 's/^[[:space:]]+//')" in '//'*|'/*'*|'*'*|'#'*) continue ;; esac
    violation "$rule" "$(printf '%s' "$h" | cut -d: -f1,2)" "$msg"
  done <<EOF
$hits
EOF
}
# shellcheck disable=SC2086
set -f
# shell-exec: no shell in download / open / media paths. Reviewed: the Windows opener in split_proc.c, which
# passes a resolved, existence-checked path to ShellExecuteA as a single argument (no command line is built).
scan shell-exec '^pion/cmd/app/split_proc\.c:[0-9]+:.*ShellExecuteA\(' \
  "spawns a shell / command interpreter (use fork+execv / CreateProcess with an argv, never a command string)" \
  '(^|[^_[:alnum:]])(system|popen|_popen|_wpopen|ShellExecute[AW]?|WinExec)[[:space:]]*\(|execl[p]?[[:space:]]*\([^)]*"(/bin/)?(ba|z)?sh"' $C_FILES
scan shell-exec '' "builds a shell command line" \
  '"[^"]*(/bin/sh|/bin/bash|sh -c|cmd(\.exe)? /c)' $PION_FILES
# plaintext-secret: never persist or log a password in clear text.
scan plaintext-secret '' "writes a password in plain text (store it sealed / hashed, or in the OS keychain)" \
  '(fs_write|fs_append|write_file|write_atomic|posix_write|kv_set|fprintf|fputs|fwrite|printf|println|log_)[_[:alnum:]]*[[:space:]]*\([^;]*(password|passwd)' $PION_FILES $C_FILES
# glass-liquid: only the lg (liquid glass) package may draw Glass::liquid.
scan glass-liquid '^pion/packages/lg/|/lg/[^:]*:' "hand-rolled Glass::liquid outside the lg package (use lg components)" \
  'Glass::liquid' $PION_FILES
# download-path: download / open code must sanitise remote filenames before joining them to a directory.
DL_FILES="$(printf '%s\n' $PION_FILES | grep -iE 'download|save_as|opener' || true)"
scan download-path 'safe_name|sanitize|safe_filename|basename_only' \
  "joins a remote filename to a directory without sanitising it (path traversal: ../, absolute, drive letters)" \
  '\+[[:space:]]*"/"[[:space:]]*\+[[:space:]]*[_[:alnum:].]*(suggested|disposition|remote_name|file_name|filename|fname)' $DL_FILES
set +f
note "  rules: shell-exec, plaintext-secret, glass-liquid, download-path"
note ""

# ---------------------------------------------------------------------------------------------------------------
# 3. Upstream currency (network)
# ---------------------------------------------------------------------------------------------------------------
if [ "$UPSTREAM" = 1 ]; then
  note "-- 3. Upstream release currency --"
  OUTDATED=0
  fetch() { curl -fsSL --max-time 30 ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} "$1" 2>/dev/null; }
  # curl-impersonate
  latest_ci="$(fetch https://api.github.com/repos/lexiforest/curl-impersonate/releases/latest | python3 -c 'import json,sys; print(json.load(sys.stdin).get("tag_name",""))' 2>/dev/null || true)"
  if [ -z "$latest_ci" ]; then
    # API rate-limited / unavailable: fall back to the highest vX.Y.Z tag (no prereleases).
    latest_ci="$(git ls-remote --tags --refs https://github.com/lexiforest/curl-impersonate.git 2>/dev/null \
      | sed -n 's|.*refs/tags/\(v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)$|\1|p' | sort -t. -k1.2,1n -k2,2n -k3,3n | tail -n 1)"
  fi
  if [ -z "$latest_ci" ]; then
    warn upstream-unreachable "could not read the latest curl-impersonate release"
  elif [ "$latest_ci" != "${CI_VER:-}" ]; then
    warn outdated "curl-impersonate ${CI_VER:-?} is behind upstream $latest_ci (BoringSSL / TLS fixes ship here): bump CURL_IMPERSONATE_VER and the SHA-256 pins"
    OUTDATED=$((OUTDATED + 1))
  else
    note "  curl-impersonate ${CI_VER} is current"
  fi
  # Node.js 24 (V8): security releases newer than the pinned Windows libnode; latest V8 for the floating spec.
  node_json="$(fetch https://nodejs.org/dist/index.json || true)"
  if [ -z "$node_json" ]; then
    warn upstream-unreachable "could not read the Node.js release index"
  else
    node_report="$(printf '%s' "$node_json" | PINNED="${NODE_WIN_VER:-}" python3 -c '
import json, os, sys
rel = json.load(sys.stdin)
pinned = os.environ.get("PINNED", "")
def key(v): return tuple(int(x) for x in v.lstrip("v").split("."))
line = [r for r in rel if r["version"].startswith("v24.")]
if not line:
    print("none"); sys.exit()
latest = max(line, key=lambda r: key(r["version"]))
print("latest %s %s" % (latest["version"], latest.get("v8", "?")))
if pinned:
    newer_sec = [r["version"] for r in line if key(r["version"]) > key(pinned) and r.get("security")]
    print("security %s" % " ".join(sorted(newer_sec, key=key)))
' 2>/dev/null || true)"
    latest_node="$(printf '%s\n' "$node_report" | sed -n 's/^latest //p')"
    sec_after="$(printf '%s\n' "$node_report" | sed -n 's/^security //p')"
    if [ -n "$latest_node" ]; then
      note "  Node.js 24 latest: ${latest_node% *} (V8 ${latest_node#* }); macOS/Linux builds resolve ${NODE_SPEC:-nodejs=24.*} at build time"
    fi
    if [ -n "$sec_after" ]; then
      warn outdated "Windows libnode ${NODE_WIN_VER:-?} predates Node.js security release(s): $sec_after (V8 fixes ship with Node): bump NODE_WIN_VER"
      OUTDATED=$((OUTDATED + 1))
    elif [ -n "${NODE_WIN_VER:-}" ] && [ -n "$latest_node" ]; then
      note "  Windows libnode ${NODE_WIN_VER} has no newer Node.js 24 security release"
    fi
  fi
  # pion / invar: how far the pins trail main (informational).
  for pair in "pion:${PION_REV:-}" "invar:${INVAR_REV:-}"; do
    name="${pair%%:*}"; pin="${pair#*:}"
    head="$(git ls-remote "https://github.com/AFK-surf/$name.git" refs/heads/main 2>/dev/null | cut -f1)"
    if [ -z "$head" ]; then
      warn upstream-unreachable "could not read $name main"
    elif [ "$head" != "$pin" ]; then
      note "  $name pin ${pin:0:12} trails main ${head:0:12} (review upstream security commits before bumping)"
    else
      note "  $name pin is main"
    fi
  done
  if [ "$STRICT" = 1 ] && [ "$OUTDATED" -gt 0 ]; then
    VIOLATIONS=$((VIOLATIONS + OUTDATED))
  fi
  note ""
fi

note "-- Result: $VIOLATIONS violation(s), $WARNINGS warning(s) --"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  { echo "## Upstream security audit"; echo; printf '%s\n' "$SUMMARY"; } >> "$GITHUB_STEP_SUMMARY"
fi
[ "$VIOLATIONS" = 0 ]
