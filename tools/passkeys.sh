#!/bin/bash
# Passkeys in a tab (host --tab) against a built host, as an agent's browser must sign in with them: a passkey from an
# external credential store (Chrome DevTools' WebAuthn.Credential: a P-256 PKCS#8 key, standard base64) put in by agent's
# passkeys:set, a page's navigator.credentials.get signed with it (checked by the page's WebCrypto and by Node, the
# count gone on and kept); a passkey the page makes with navigator.credentials.create, signed with at once and in a
# second tab of the same profile (FV_PROFILE_DIR), its count going on from the first tab's; passkeys:get and :clear.
# Fails on the first thing that is not so.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/passkeys
rm -rf $O && mkdir -p $O/profile $O/state1 $O/state2 $O/www
port=${PASSKEYS_PORT:-7690}
port2=$((port + 5))
web=$((port + 10))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "passkeys: $1"; for f in $O/*.out; do echo "-- $f"; tail -20 $f; done; exit 1; }
cp tests/host/passkeys.html $O/www/
python3 -I -m http.server --bind 127.0.0.1 --directory $O/www $web > $O/web.out 2>&1 & W=$!
for _ in $(seq 100); do curl -sf "http://127.0.0.1:$web/passkeys.html" > /dev/null && break; sleep 0.1; done
# (localhost, a secure origin, to the server here)
export FV_CONNECT_TO=::127.0.0.1:
PAGE="http://localhost:$web/passkeys.html"
FV_PROFILE_DIR=$O/profile FV_STATE_DIR=$O/state1 FV_HOST_LOG=$O/host1.log $HOST --tab $port "$PAGE" >> $O/host1.out 2>&1 & H1=$!
FV_PROFILE_DIR=$O/profile FV_STATE_DIR=$O/state2 FV_HOST_LOG=$O/host2.log $HOST --tab $port2 "$PAGE" >> $O/host2.out 2>&1 & H2=$!
trap 'kill $H1 $H2 $W 2>/dev/null' EXIT
for s in state1 state2; do
  for _ in $(seq 100); do [ -s $O/$s/host.json ] && break; sleep 0.1; done
  [ -s $O/$s/host.json ] || fail "no host.json in $s"
done
A1() { $AGENT 127.0.0.1 $((port+1)) "$@"; }
A2() { $AGENT 127.0.0.1 $((port2+1)) "$@"; }
for A in A1 A2; do
  for _ in $(seq 50); do $A 'eval:document.title' 2>/dev/null | grep -qx '"passkeys"' && break; sleep 0.2; done
  $A 'eval:document.title' | grep -qx '"passkeys"' || fail "the page did not load ($A)"
done
# the last line an agent printed: a script's value as JSON
value() { tail -1 "$1"; }
A1 'eval:checks()' > $O/checks.out 2>&1
[ "$(value $O/checks.out)" = '[true,true,"function","function"]' ] || fail "the API is not there: $(value $O/checks.out)"

# a vault's passkey, as DevTools exports one
node -e '
const c = require("crypto");
const { privateKey, publicKey } = c.generateKeyPairSync("ec", { namedCurve: "P-256" });
const id = c.randomBytes(16).toString("base64");
const pk = { credentialId: id, rpId: "localhost", privateKey: privateKey.export({ type: "pkcs8", format: "der" }).toString("base64"), userHandle: Buffer.from("vault-user").toString("base64"), signCount: 7 };
require("fs").writeFileSync(process.argv[1], JSON.stringify([pk]));
require("fs").writeFileSync(process.argv[2], publicKey.export({ type: "spki", format: "der" }).toString("base64url"));
' $O/vault.json $O/vault.spki || fail "node made no key"
VID=$(node -e 'console.log(Buffer.from(JSON.parse(require("fs").readFileSync(process.argv[1]))[0].credentialId, "base64").toString("base64url"))' $O/vault.json)
A1 "passkeys:set=$(cat $O/vault.json)" > $O/set.out 2>&1
grep -qx 'ok: 1 set' $O/set.out || fail "passkeys:set: $(tail -1 $O/set.out)"
grep -q 'privateKey\|PRIVATE' $O/set.out && fail "passkeys:set printed the key"
grep -q "$(head -c 40 <(node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1]))[0].privateKey)' $O/vault.json))" $O/host1.log && fail "the host's log has the private key"
[ "$(if [ "$(uname)" = Darwin ]; then stat -f %Lp $O/profile/passkeys.json; else stat -c %a $O/profile/passkeys.json; fi)" = 600 ] || fail "passkeys.json is not its owner's alone"

# a sign-in with it: signed over the authenticator data and the client data's hash, the count 8 now
A1 "eval:signIn('localhost', ['$VID']).then(async a => Object.assign(a, { ok: await verify('$(cat $O/vault.spki)', a) }))" > $O/get1.out 2>&1
node -e '
const a = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8").trim().split("\n").pop());
const c = require("crypto");
const fail = m => { console.log(m + ": " + JSON.stringify(a)); process.exit(1); };
if (!a || a.ok !== true) fail("the page'"'"'s WebCrypto did not verify it");
const key = c.createPublicKey({ key: Buffer.from(process.argv[2], "base64url"), format: "der", type: "spki" });
const auth = Buffer.from(a.auth, "base64url"), cd = Buffer.from(a.cdata, "base64url");
const signed = Buffer.concat([auth, c.createHash("sha256").update(cd).digest()]);
if (!c.verify("sha256", signed, key, Buffer.from(a.sig, "base64url"))) fail("Node did not verify it");
if (!auth.subarray(0, 32).equals(c.createHash("sha256").update("localhost").digest())) fail("not localhost'"'"'s rpIdHash");
if (auth.length !== 37 || a.flags !== 5 || a.count !== 8) fail("authenticator data");
if (a.id !== process.argv[3] || a.rawId !== a.id || a.type !== "public-key" || a.attachment !== "platform" || !a.isPKC || !a.isAssertion) fail("the credential");
if (a.cdType !== "webauthn.get" || a.origin !== process.argv[4] || !a.challengeOk) fail("the client data");
if (a.user !== Buffer.from("vault-user").toString("base64url") || a.ext !== "{}" || a.json.response.signature !== a.sig) fail("user handle, extensions, toJSON");
' $O/get1.out $(cat $O/vault.spki) $VID "http://localhost:$web" || fail "the vault's passkey's sign-in"
A1 'passkeys:get=localhost' > $O/get-agent.out 2>&1
node -e 'const l = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8").trim().split("\n").pop()); if (l.length !== 1 || l[0].signCount !== 8) { console.log(JSON.stringify(l.map(p => p.signCount))); process.exit(1); }' $O/get-agent.out || fail "the count was not kept: $(tail -1 $O/get-agent.out | head -c 200)"

# a passkey the page makes: its attestation, then a sign-in in this tab and in the other tab of the profile
A1 "eval:register('localhost', 'alice')" > $O/create.out 2>&1
node -e '
const a = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8").trim().split("\n").pop());
const fail = m => { console.log(m + ": " + JSON.stringify(a)); process.exit(1); };
if (!a.isAttestation || a.alg !== -7 || a.cdType !== "webauthn.create" || a.flags !== 0x45) fail("the attestation");
const att = Buffer.from(a.att, "base64url"), auth = Buffer.from(a.auth, "base64url");
if (att.indexOf(Buffer.from("fmt")) < 0 || att.indexOf(Buffer.from("none")) < 0 || att.indexOf(auth) < 0) fail("attestationObject");
const n = auth.readUInt16BE(53), id = auth.subarray(55, 55 + n), cose = auth.subarray(55 + n);
if (id.toString("base64url") !== a.id || cose.length !== 77 || cose[0] !== 0xa5) fail("credential data");
const spki = Buffer.from(a.spki, "base64url");
if (!spki.subarray(spki.length - 64, spki.length - 32).equals(cose.subarray(10, 42)) || !spki.subarray(spki.length - 32).equals(cose.subarray(45, 77))) fail("COSE key and SPKI differ");
require("fs").writeFileSync(process.argv[2], a.id + " " + a.spki);
' $O/create.out $O/made.txt || fail "navigator.credentials.create"
read MID MSPKI < $O/made.txt
for A in A1 A2 A1; do
  $A "eval:signIn('localhost', ['$MID']).then(async a => ({ ok: await verify('$MSPKI', a), count: a.count, user: a.user }))" > $O/get-$A.out 2>&1
  echo "$A $(value $O/get-$A.out)" >> $O/counts.out
done
[ "$(cut -d' ' -f2- $O/counts.out)" = "$(printf '%s\n' '{"ok":true,"count":1,"user":"dXNlci1hbGljZQ"}' '{"ok":true,"count":2,"user":"dXNlci1hbGljZQ"}' '{"ok":true,"count":3,"user":"dXNlci1hbGljZQ"}')" ] ||
  fail "the made passkey across tabs: $(cat $O/counts.out)"
# none for another site; a page may not name a relying party not its own
A2 "eval:navigator.credentials.get({ publicKey: { challenge: new Uint8Array(32), rpId: 'example.com' } }).then(() => 'signed', e => e.name)" > $O/other.out 2>&1
[ "$(value $O/other.out)" = '"SecurityError"' ] || fail "another site's relying party: $(value $O/other.out)"
A2 "passkeys:clear=localhost" 'passkeys:get' > $O/clear.out 2>&1
[ "$(value $O/clear.out)" = '[]' ] || fail "passkeys:clear: $(value $O/clear.out)"
A1 "eval:signIn('localhost', []).then(() => 'signed', e => e.name)" > $O/none.out 2>&1
[ "$(value $O/none.out)" = '"NotAllowedError"' ] || fail "a sign-in with none kept: $(value $O/none.out)"
echo "passkeys: ok (vault P-256 key signed and verified by WebCrypto and Node, count 7 -> 8 kept; create -> get in two tabs, counts 1, 2, 3; SecurityError, clear, NotAllowedError)"
