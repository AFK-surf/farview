#!/bin/bash
# The browser's own steps against a built host, as the client asks for them: an address typed, back, forward,
# reload, and a scroll the page's scripts hear. Fails on the first step that did not do what it should.
set -u
B=${FV_BIN_DIR:-out}
mkdir -p probe-out
port=7390
T=file://$PWD/tests/host
: > probe-out/shell.host.log; rm -f probe-out/shell.storage; FV_STORAGE=probe-out/shell.storage FV_HOST_LOG=probe-out/shell.host.log $B/pion-host $port "$T/a.html" > probe-out/shell.host.out 2>&1 & H=$!
trap 'kill $H 2>/dev/null' EXIT
sleep 1
# pages of its own, so what it checks is the browser's and not a site's
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell.raw \
  "go:$T/b.html" back forward reload "go:$T/scroll.html" scrollto:1200 > probe-out/shell.probe 2>&1
pages=$(grep "^page file" probe-out/shell.probe | sed 's/^page //')
echo "$pages"
want="$T/a.html
$T/b.html
$T/a.html
$T/b.html
$T/b.html
$T/scroll.html"
[ "$pages" = "$want" ] || { echo "pages were not these:"; echo "$want"; exit 1; }
# the page's script heard the scroll and redrew its fixed line: something came after it
tail -2 probe-out/shell.probe | grep -q "bytes scrollto:1200 [1-9]" || { echo "nothing after the scroll"; tail -5 probe-out/shell.probe; exit 1; }
# a password field the client is told to show as dots
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-pw.raw "go:$T/login.html" "click:input[name=user]" \
  "click:input[name=pw]" > probe-out/shell.pw 2>&1
grep -q "^field .*lines false, secret false" probe-out/shell.pw && grep -q "^field .*secret true" probe-out/shell.pw ||
  { echo "the password field was not marked secret"; cat probe-out/shell.pw; exit 1; }
# CSS gradients drawn: a linear one's ends, layered ones over each other, a conic one starting from up (pixels of
# the picture the client draws, red green blue at a point)
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-grad.raw "go:$T/gradients.html" > probe-out/shell.grad 2>&1
px() { od -An -tu1 -j $(( ($2 * 412 + $1) * 4 )) -N3 probe-out/shell-grad.raw | tr -s ' ' | sed 's/^ //; s/ $//'; }
got="$(px 12 48) | $(px 184 48) | $(px 220 336) | $(px 370 336) | $(px 294 204)"
[ "$got" = "249 0 6 | 5 0 250 | 127 0 0 | 0 127 0 | 255 3 0" ] || { echo "gradients: got $got"; exit 1; }
# CSS transforms drawn: a box turned 45deg has its top tip above where the box was and an empty corner, a box at
# scale(0) shows nothing
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-tf.raw "go:$T/transforms.html" > probe-out/shell.tf 2>&1
tpx() { od -An -tu1 -j $(( ($2 * 412 + $1) * 4 )) -N3 probe-out/shell-tf.raw | tr -s ' ' | sed 's/^ //; s/ $//'; }
got="$(tpx 60 15) | $(tpx 25 30) | $(tpx 190 65)"
[ "$got" = "37 99 235 | 255 255 255 | 255 255 255" ] || { echo "transforms: got $got"; exit 1; }
# import() of a module with top-level await waits for it; performance marks are kept
got=$($B/pion-wpt "$T/modules-tla.html" 'document.getElementById("out").textContent + " / " + document.title' 2>/dev/null | tail -1)
[ "$got" = "import ready / tla 1" ] || { echo "modules: got $got"; exit 1; }
# a script's request answers with bytes as they are (every byte value), and a Blob counts its bytes
( cd tests/host && exec python3 -m http.server $((port+3)) --bind 127.0.0.1 > /dev/null 2>&1 ) & W=$!
for _ in $(seq 1 50); do curl -s -o /dev/null http://127.0.0.1:$((port+3))/a.html && break; sleep 0.1; done
got=$($B/pion-wpt http://127.0.0.1:$((port+3))/a.html '(()=>{const x=new XMLHttpRequest();x.open("GET","bytes.bin",false);x.overrideMimeType("text/plain; charset=x-user-defined");x.send();const t=x.responseText;let s=0;for(let i=0;i<t.length;i++)s+=t.charCodeAt(i)&255;const u=new Uint8Array(t.length);for(let i=0;i<t.length;i++)u[i]=t.charCodeAt(i)&255;return t.length+" "+s+" "+new Blob([u,"é"]).size})()' 2>/dev/null | tail -1)
# ... and one to another origin (localhost) that does not allow it is a network error, its answer not the page's
cors=$($B/pion-wpt http://127.0.0.1:$((port+3))/a.html "(()=>{try{const x=new XMLHttpRequest();x.open('GET','http://localhost:$((port+3))/bytes.bin',false);x.send();return 'read '+x.status}catch(e){return 'blocked '+e.name}})()" 2>/dev/null | tail -1)
# ... and a port sent to a frame of another origin (localhost) with a message comes back with its answer
fport=$($B/pion-wpt "http://127.0.0.1:$((port+3))/ports.html?frame=http://localhost:$((port+3))/portframe.html" 'document.title' 2>/dev/null | tail -1)
dport=$($B/pion-wpt "http://127.0.0.1:$((port+3))/ports.html?frame=data" 'document.title' 2>/dev/null | tail -1)
# ... and a message's source is the window that posted it, a same-origin frame's too, and can be answered
ssrc=$($B/pion-wpt "http://127.0.0.1:$((port+3))/source.html?frame=http://127.0.0.1:$((port+3))/sourceframe.html" 'document.title' 2>/dev/null | tail -1)
xsrc=$($B/pion-wpt "http://127.0.0.1:$((port+3))/source.html?frame=http://localhost:$((port+3))/sourceframe.html" 'document.title' 2>/dev/null | tail -1)
kill $W 2>/dev/null
[ "$dport" = "f hi|pong ping|w 1 true" ] || { echo "data: frame ports: got $dport"; exit 1; }
[ "$got" = "1024 130560 1026" ] || { echo "bytes: got $got"; exit 1; }
[ "$cors" = "blocked NetworkError" ] || { echo "cors: got $cors"; exit 1; }
[ "$fport" = "f hi|pong ping|w 1 true" ] || { echo "frame ports: got $fport"; exit 1; }
[ "$ssrc" = "from frame true|re back true|re ping true" ] || { echo "same-origin message source: got $ssrc"; exit 1; }
[ "$xsrc" = "from frame true|re back true|re ping true" ] || { echo "message source: got $xsrc"; exit 1; }
# a change a script measures right after making it is still drawn: the page the client ends with is the longer one
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-late.raw "go:$T/latechange.html" > probe-out/shell.late 2>&1
grep "^page: height" probe-out/shell.late | tail -1 | grep -q "height 250," || { echo "a measured change was never drawn"; tail -4 probe-out/shell.late; exit 1; }
# a tap on a label checks its box, a tap on a radio button picks it: the page hears both as changes
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-checks.raw "go:$T/checks.html" "click:#l" "click:#rl" > probe-out/shell.checks 2>&1
grep -q "^title cb true size l$" probe-out/shell.host.log || { echo "checks: the taps did not check"; grep "^title cb" probe-out/shell.host.log | tail -3; exit 1; }
# a finger drags a slider the page follows touches on (a captcha's way): the client is told where, and the page sees
# the drag end at the far side
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-slider.raw "go:$T/slider.html" "drag:40,80,320,80" > probe-out/shell.slider 2>&1
grep -q "^drag areas: [0-9.]*,[0-9.]* 40x40" probe-out/shell.slider && grep -q "^title slider done" probe-out/shell.slider ||
  { echo "slider: not dragged"; grep "^drag\|^title" probe-out/shell.slider; exit 1; }
# the page scrolls itself (window.scrollTo, scrollIntoView): its scripts see it at once, and the client is told where
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-scroll.raw "go:$T/scrollto.html" > probe-out/shell.scroll 2>&1
grep -q "^title scrolled 3000 500,500,3000" probe-out/shell.scroll && grep -q "^scrolled to 3000" probe-out/shell.scroll ||
  { echo "scrollto: the page did not scroll itself"; grep "^title\|^scrolled" probe-out/shell.scroll; exit 1; }
# iframes (a srcdoc, one of the page's origin, one its script fills) are drawn, and a tap in one is its document's
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-frames.raw "go:$T/frames.html" "tap:25,85" > probe-out/shell.frames 2>&1
grep -q "^title clicked in frame" probe-out/shell.frames || { echo "frames: the tap did not reach the frame"; grep "^title" probe-out/shell.frames; exit 1; }
# a worker made from a Blob answers the page's messages
got=$($B/pion-wpt "$T/worker.html" 'res' 2>/dev/null | tail -1)
[ "$got" = "42,hi function" ] || { echo "worker: got $got"; exit 1; }
# a worker's imported scripts share its scope: what one declares (a function, a var) the importer calls after
got=$($B/pion-wpt "$T/workerimport.html" 'res' 2>/dev/null | tail -1)
[ "$got" = '16' ] || { echo "worker imports: got $got"; exit 1; }
# localStorage outlives a page and a connection: a second visit counts two
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-st1.raw "go:$T/storage.html" > probe-out/shell.st1 2>&1
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-st2.raw "go:$T/storage.html" > probe-out/shell.st2 2>&1
grep -q "^title visits 2 true visits" probe-out/shell.st2 || { echo "storage: not kept"; grep "^title" probe-out/shell.st1 probe-out/shell.st2; exit 1; }
# ... kept by origin: the same page from another origin starts with none of them
( cd tests/host && exec python3 -m http.server $((port+3)) --bind 127.0.0.1 > /dev/null 2>&1 ) & W=$!
for _ in $(seq 1 50); do curl -s -o /dev/null http://127.0.0.1:$((port+3))/storage.html && break; sleep 0.1; done
$B/pion-probe 127.0.0.1 $port 412 900 probe-out/shell-st3.raw "go:http://127.0.0.1:$((port+3))/storage.html" > probe-out/shell.st3 2>&1
kill $W 2>/dev/null
grep -q "^title visits 1 true visits" probe-out/shell.st3 || { echo "storage: shared across origins"; grep "^title" probe-out/shell.st3; exit 1; }
# matchMedia answers for the viewport (800 px wide here)
got=$($B/pion-wpt "$T/a.html" '[matchMedia("(min-width: 700px)").matches, matchMedia("(max-width: 500px)").matches, matchMedia("screen and (orientation: landscape)").matches].join(" ")' 2>/dev/null | tail -1)
[ "$got" = "true false true" ] || { echo "matchMedia: got $got"; exit 1; }
# an img shows the picture its srcset, sizes and picture's sources pick for the viewport
got=$($B/pion-wpt "$T/srcset.html" '["a","b","c"].map(i => document.getElementById(i).currentSrc.split("/").pop()).join(" ")' 2>/dev/null | tail -1)
[ "$got" = "b.png m.png desk.png" ] || { echo "srcset: got $got"; exit 1; }
# a port sent in a message's transfer list arrives with it and carries the answer back
got=$($B/pion-wpt "$T/ports.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "pong ping|w 1 true" ] || { echo "ports: got $got"; exit 1; }
# IndexedDB keeps records by key and by index, refuses a taken key, walks a cursor back and completes
got=$($B/pion-wpt "$T/idb.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "up 0-2 | dup ConstraintError | names a,b | y 2 | range 2 | back 2,1 | done" ] || { echo "indexeddb: got $got"; exit 1; }
# a document's forms, frames and pictures read as its properties by name, after what Document itself has
got=$($B/pion-wpt "$T/docnamed.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "form v | frame true | pics 2 | own string | none undefined" ] || { echo "document named: got $got"; exit 1; }
# a canvas's drawings, kept to be drawn when its pixels are needed, read as drawn in order
got=$($B/pion-wpt "$T/canvasdefer.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "0,0,255,255 | 0,0,0,0 | 0,0,0,0 | 0,255,0,255 | 0,255,0,255 | 0,0,0,0 | 0,0,0,0 | 0,0,255,255" ] || { echo "canvas drawings deferred: got $got"; exit 1; }
# a same-origin frame's document reached from the page is the frame's own (its scripts' nodes and listeners)
got=$($B/pion-wpt "$T/framerealm.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "true input typed true" ] || { echo "frame document realm: got $got"; exit 1; }
# names follow the document as it changes: an element given an id, a form put in, an id changed, one taken out (each
# looked up before the change and after it)
got=$($B/pion-wpt "$T/docnamed.html" '(() => { const r = []; const d = document.createElement("div"); r.push(typeof window.later); d.id = "later"; document.body.append(d); r.push(window.later === d); const f = document.createElement("form"); f.name = "fm2"; r.push(typeof document.fm2); document.body.append(f); r.push(document.fm2 === f); d.id = "moved"; r.push(typeof window.later, window.moved === d); f.setAttribute("class", "x"); r.push(document.fm2 === f); f.remove(); r.push(typeof document.fm2); return r.join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = "undefined true undefined true undefined true true undefined" ] || { echo "names after changes: got $got"; exit 1; }
# an element and a document read as what they are to Object.prototype.toString (libraries tell a node from a plain
# object so)
got=$($B/pion-wpt "$T/docnamed.html" '[document.createElement("div"), document, new Event("x")].map(o => Object.prototype.toString.call(o)).join(" ")' 2>/dev/null | tail -1)
[ "$got" = "[object HTMLDivElement] [object HTMLDocument] [object Event]" ] || { echo "toString tags: got $got"; exit 1; }
# a script a page inserts (and a string timer) runs as a script: its top-level var is the window's under "use strict"
got=$($B/pion-wpt "$T/strictvar.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "number function number 1" ] || { echo "inserted script scope: got $got"; exit 1; }
# a gradient written as CSSOM has it (its direction before its method, the default method left out, colors computed),
# and one that breaks its grammar dropped, so the declaration before it stays
got=$($B/pion-wpt "$T/a.html" '(() => { const d = document.createElement("div"); document.body.append(d); d.style.backgroundImage = "linear-gradient(in oklab 90deg, red, blue)"; const a = d.style.backgroundImage; d.style.backgroundImage = "linear-gradient(red, blue, 50%)"; return a + " | " + getComputedStyle(d).backgroundImage; })()' 2>/dev/null | tail -1)
[ "$got" = 'linear-gradient(90deg in oklab, red, blue) | linear-gradient(90deg in oklab, rgb(255, 0, 0), rgb(0, 0, 255))' ] || { echo "gradient values: got $got"; exit 1; }
# a transform reads as its matrix (Swiper and carousels read their position out of it), and a transition between two
# transform lists that do not match goes through the decomposed matrices (Chrome's matrix at a third of the way)
got=$($B/pion-wpt "$T/a.html" '(() => { const d = document.createElement("div"); document.body.append(d); d.style.transform = "rotate(90deg) translateX(10px)"; const a = getComputedStyle(d).transform; const an = d.animate([{ transform: "matrix(1, 0, 0, 1, 0, -6)" }, { transform: "matrix(0, 7, -1, 0, 6, 0)" }], { duration: 3000 }); an.pause(); an.currentTime = 1000; return a + " | " + getComputedStyle(d).transform; })()' 2>/dev/null | tail -1)
[ "$got" = 'matrix(0, 1, -1, 0, 0, 10) | matrix(2.598076, 1.5, -0.5, 0.866025, 2, -4)' ] || { echo "transform matrices: got $got"; exit 1; }
# Web Animations: an animation is listed by getAnimations, waits for a frame to start (pending), and a time set on it
# shows the effect there
got=$($B/pion-wpt "$T/a.html" '(() => { const d = document.createElement("div"); document.body.append(d); const a = d.animate({ opacity: [0, 1] }, 100); const s = [d.getAnimations().length, a.playState, a.pending]; a.pause(); a.currentTime = 50; s.push(getComputedStyle(d).opacity, a.effect.getComputedTiming().progress); return s.join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = '1 running true 0.5 0.5' ] || { echo "web animations: got $got"; exit 1; }
# CSS transitions are animations a script can find and seek (getAnimations), the parent's starting before its child's,
# and a child inheriting the value follows the parent's transition
got=$($B/pion-wpt "$T/a.html" '(() => { const st = document.createElement("style"); st.textContent = ".c{font-size:10px} .c.to{font-size:20px} .c.how{transition:font-size 2s linear 0s} .t{font-size:inherit} .t.how{transition:font-size 2s linear 0s}"; document.head.append(st); const c = document.createElement("div"); c.className = "c"; const t = document.createElement("div"); t.className = "t"; c.append(t); document.body.append(c); const r = [getComputedStyle(t).fontSize]; c.classList.add("how"); t.classList.add("how"); c.classList.add("to"); r.push(getComputedStyle(t).fontSize, getComputedStyle(c).fontSize); const as = document.getAnimations(); r.push(as.length, as.map(a => a.effect.target.className + ":" + a.transitionProperty).join(",")); as.forEach(a => a.currentTime = 1000); r.push(getComputedStyle(c).fontSize, getComputedStyle(t).fontSize); return r.join(" | "); })()' 2>/dev/null | tail -1)
[ "$got" = '10px | 10px | 10px | 1 | c how to:font-size | 15px | 15px' ] || { echo "css transitions: got $got"; exit 1; }
# CSS animations are animations a script can find and seek (getAnimations), every keyframe of the rule in between
got=$($B/pion-wpt "$T/a.html" '(() => { const st = document.createElement("style"); st.textContent = "@keyframes slide { from { width: 100px } 50% { width: 400px } to { width: 300px } } .z { animation: slide 1s linear }"; document.head.append(st); const d = document.createElement("div"); d.className = "z"; document.body.append(d); const a = d.getAnimations()[0]; a.currentTime = 250; const w = getComputedStyle(d).width; a.currentTime = 750; return [a.constructor.name, a.animationName, w, getComputedStyle(d).width].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = 'CSSAnimation slide 250px 350px' ] || { echo "css animations: got $got"; exit 1; }
# a shadow reads with its color first and its lengths in px, and goes from one to another shadow by shadow
got=$($B/pion-wpt "$T/a.html" '(() => { const d = document.createElement("div"); document.body.append(d); d.style.boxShadow = "2px 3px red"; const r = [getComputedStyle(d).boxShadow]; const a = d.animate({ boxShadow: ["0 0 0 black", "10px 10px 10px red"] }, 100); a.pause(); a.currentTime = 50; r.push(getComputedStyle(d).boxShadow); return r.join(" | "); })()' 2>/dev/null | tail -1)
[ "$got" = 'rgb(255, 0, 0) 2px 3px 0px 0px | rgb(128, 0, 0) 5px 5px 5px 0px' ] || { echo "shadows: got $got"; exit 1; }
# an event in a shadow tree comes out to the page retargeted to its host, and a host's child is in its slot
got=$($B/pion-wpt "$T/a.html" '(() => { const h = document.createElement("x-card"); h.id = "card"; document.body.append(h); const sr = h.attachShadow({ mode: "open" }); sr.innerHTML = "<div><button id=b>go</button><slot></slot></div>"; const r = []; document.addEventListener("click", e => r.push(e.target.id, e.composedPath().length)); sr.getElementById("b").click(); const t = document.createElement("span"); h.append(t); r.push(t.assignedSlot && t.assignedSlot.localName); return r.join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = 'card 8 slot' ] || { echo "shadow events: got $got"; exit 1; }
# markup with a declarative shadow root (setHTMLUnsafe) gets one, and its slots take the host's children by name
got=$($B/pion-wpt "$T/a.html" '(() => { const d = document.createElement("div"); document.body.append(d); d.setHTMLUnsafe("<p id=h><template shadowrootmode=open><slot name=a><i>fallback</i></slot><slot></slot></template><span slot=a>x</span>t</p>"); const h = d.querySelector("#h"), sl = h.shadowRoot.querySelectorAll("slot"); return [h.shadowRoot.mode, sl[0].assignedNodes().map(n => n.localName).join(","), sl[1].assignedNodes().map(n => n.nodeName).join(","), d.querySelectorAll("template").length].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = 'open span #text 0' ] || { echo "declarative shadow: got $got"; exit 1; }
# Web Audio is there (silent): a context, nodes connected, parameters and buffers
got=$($B/pion-wpt "$T/a.html" '(() => { const c = new AudioContext(), o = c.createOscillator(), g = c.createGain(); o.connect(g).connect(c.destination); o.frequency.setValueAtTime(220, 0); const b = c.createBuffer(2, 100, 44100); return [c.state, o.frequency.value, g.gain.value, b.getChannelData(1).length, typeof OfflineAudioContext].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = 'suspended 220 1 100 function' ] || { echo "web audio: got $got"; exit 1; }
# a quirks mode document (a frame written anew without a doctype) takes unitless lengths and hashless colors; a
# standards one drops them, and a length property drops a value that is not a length
got=$($B/pion-wpt "$T/a.html" '(() => { const f = document.createElement("iframe"); document.body.append(f); const w = f.contentWindow; w.document.open(); w.document.write("<style>#x{width:9;color:abc;height:20px;height:junk}</style><div id=x></div>"); w.document.close(); const x = w.document.getElementById("x"), cs = w.getComputedStyle(x); const d = document.createElement("div"); d.style.width = "7"; d.style.color = "abc"; return [w.document.compatMode, cs.width, cs.color, cs.height, document.compatMode, JSON.stringify(d.style.width + d.style.color)].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = 'BackCompat 9px rgb(170, 187, 204) 20px CSS1Compat ""' ] || { echo "quirks: got $got"; exit 1; }
# the attributes scripts take the getters of (a consent manager wraps them) are accessors where a browser has them:
# Node's nodeType answering for every kind of node, navigator's on Navigator, the window's closed; interfaces are not
# enumerable on the window
got=$($B/pion-wpt "$T/a.html" '(() => { const g = (C, k) => Object.getOwnPropertyDescriptor(C.prototype, k).get; const nt = g(Node, "nodeType"); const r = [nt.call(document.body), nt.call(document), nt.call(document.createTextNode("")), g(Node, "nodeName").call(document.body), g(Navigator, "languages").call(navigator).length > 0, Object.getOwnPropertyNames(navigator).length, typeof Object.getOwnPropertyDescriptor(window, "closed").get, typeof g(XMLHttpRequest, "withCredentials"), typeof g(Request, "url"), typeof g(HTMLElement, "dataset"), typeof new SecurityPolicyViolationEvent("securitypolicyviolation").blockedURI]; let en = 0; for (const k in window) if (k === "Node" || k === "ProcessingInstruction") en++; r.push(en); return r.join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = '1 9 3 BODY true 0 function function function function string 0' ] || { echo "idl accessors: got $got"; exit 1; }
# a button shows its popover target (drawn fixed, :popover-open, beforetoggle then toggle), a modal dialog matches :modal
# and its form of method dialog closes it with the button's value; showing it hides the auto popover
got=$($B/pion-wpt "$T/popover.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = 'none before open block true fixed before closed true true false toggle closed>closed closed yes false' ] || { echo "popover: got $got"; exit 1; }
# a page's own scripts cannot reach the host's input entry points (they would act as its user, with trusted events),
# nor replace the gate the host's calls go through
got=$($B/pion-wpt "$T/a.html" '(() => { const r = [typeof window.__click, typeof window.__mouse, typeof window.__deliver, typeof window.__netDone, typeof __fvH("guess")]; try { window.__fvH = () => ({}); } catch (e) {} r.push(typeof __fvH("guess")); return r.join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = 'undefined undefined undefined undefined undefined undefined' ] || { echo "host-only entry points: got $got"; exit 1; }
# the editing commands work in an editing host: bold wraps the selection, the host hears an input event, and the
# state reads back
got=$($B/pion-wpt "$T/a.html" '(() => { const d = document.createElement("div"); d.contentEditable = "true"; d.innerHTML = "foo bar"; document.body.append(d); let ev = ""; d.addEventListener("input", e => ev = e.inputType); const r = document.createRange(); r.setStart(d.firstChild, 4); r.setEnd(d.firstChild, 7); getSelection().removeAllRanges(); getSelection().addRange(r); const ok = document.execCommand("bold"); return [ok, d.innerHTML, ev, document.queryCommandState("bold"), d.isContentEditable, d.contentEditable].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = 'true foo <b>bar</b> formatBold true true true' ] || { echo "editing: got $got"; exit 1; }
# SVG's DOM: each element its own interface, attributes as animated values, and the geometry charts measure with
got=$($B/pion-wpt "$T/a.html" '(() => { const d = document.createElement("div"); d.innerHTML = "<svg width=200 height=100 viewBox=\"0 0 100 50\"><g transform=\"translate(10 5)\"><rect id=r x=1 y=2 width=30 height=10 /><path id=p d=\"M0 0 H30 V40\" /></g></svg>"; document.body.append(d); const r = d.querySelector("#r"), p = d.querySelector("#p"), svg = d.querySelector("svg"); const b = r.getBBox(), g = r.parentNode.getBBox(), pt = svg.createSVGPoint(); pt.x = 1; pt.y = 2; const m = r.getCTM(); const q = pt.matrixTransform(m); return [r instanceof SVGRectElement, r instanceof SVGGeometryElement, r.width.baseVal.value, [b.x, b.y, b.width, b.height].join(","), [g.x, g.y, g.width, g.height].join(","), p.getTotalLength(), [q.x, q.y].join(","), svg.viewBox.baseVal.width, r.parentNode.transform.baseVal.getItem(0).matrix.e].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = 'true true 30 1,2,30,10 0,0,31,40 70 22,14 100 10' ] || { echo "svg dom: got $got"; exit 1; }
# a point of an svg hits the shape painted there (its fill, its stroke, as pointer-events says), else the svg; and
# presentation attributes are its style
got=$($B/pion-wpt "$T/a.html" '(() => { const d = document.createElement("div"); d.style.cssText = "position:absolute;left:0;top:0"; d.innerHTML = "<svg width=200 height=100 style=\"display:block\"><rect id=a x=10 y=10 width=50 height=50 fill=blue /><circle id=c cx=150 cy=50 r=20 fill=none stroke=red stroke-width=6 /></svg>"; document.body.append(d); const at = (x, y) => { const e = document.elementFromPoint(x, y); return e ? e.id || e.localName : "-"; }; return [at(30, 30), at(100, 90), at(150, 50), at(170, 50), getComputedStyle(d.querySelector("#a")).fill].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = 'a svg svg c rgb(0, 0, 255)' ] || { echo "svg hit: got $got"; exit 1; }
# Web Crypto over the host's BoringSSL: AES-GCM round trip, ECDSA sign and verify, PBKDF2 into a key, random values
got=$($B/pion-wpt "$T/subtle.html" 'res' 2>/dev/null | tail -1)
[ "$got" = 'hello 21 64 true 248,207 true false AES-GCM' ] || { echo "web crypto: got $got"; exit 1; }
# the clipboard: navigator.clipboard's text, and a copy command copies the selection after its copy event
got=$($B/pion-wpt "$T/clip.html" 'res' 2>/dev/null | tail -1)
[ "$got" = 'one | true | copy:true | copy me' ] || { echo "clipboard: got $got"; exit 1; }
# an element's dataset lists its data- attributes (Apple's modal finds its parts with for (k in el.dataset))
got=$($B/pion-wpt "$T/docnamed.html" '(() => { const d = document.createElement("div"); d.setAttribute("data-modal-element-content", ""); d.setAttribute("data-x", "1"); d.setAttribute("title", "t"); const k = []; for (const i in d.dataset) k.push(i); return k.join(",") + " " + JSON.stringify(d.dataset); })()' 2>/dev/null | tail -1)
[ "$got" = 'modalElementContent,x {"modalElementContent":"","x":"1"}' ] || { echo "dataset keys: got $got"; exit 1; }
# a computed style reads custom properties (var()s substituted) and logical ones as their physical sides
got=$($B/pion-wpt "$T/docnamed.html" '(() => { const d = document.createElement("div"); d.style.cssText = "--a: 1px; --b: var(--a); margin-right: 5px"; document.body.appendChild(d); const c = getComputedStyle(d); return [c.getPropertyValue("--b"), c.marginInlineEnd, c.getPropertyValue("margin-inline-end")].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = "1px 5px 5px" ] || { echo "computed custom and logical: got $got"; exit 1; }
# a custom property set to a CSS-wide keyword takes the parent's value (initial: none); revert-layer the layer below's
got=$($B/pion-wpt "$T/docnamed.html" '(() => { const st = document.createElement("style"); st.textContent = "@layer a { .k { --c: below; } } @layer b { .k { --c: var(--none, revert-layer); } }"; document.head.appendChild(st); const p = document.createElement("div"); p.style.cssText = "--a: 20px; --b: 20px"; const c = document.createElement("div"); c.className = "k"; c.style.cssText = "--a: inherit; --b: initial"; p.appendChild(c); document.body.appendChild(p); const s = getComputedStyle(c); return [s.getPropertyValue("--a"), "[" + s.getPropertyValue("--b") + "]", s.getPropertyValue("--c")].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = "20px [] below" ] || { echo "custom property keywords: got $got"; exit 1; }
# a property registered with @property (or CSS.registerProperty) that does not inherit starts from its initial value
got=$($B/pion-wpt "$T/docnamed.html" '(() => { const st = document.createElement("style"); st.textContent = "@property --r { syntax: \"<length>\"; inherits: false; initial-value: 3px; }"; document.head.appendChild(st); CSS.registerProperty({ name: "--q", syntax: "<color>", inherits: false, initialValue: "green" }); const p = document.createElement("div"); p.style.cssText = "--r: 9px; --q: red"; const c = document.createElement("div"); p.appendChild(c); document.body.appendChild(p); const s = getComputedStyle(c); let dup = "no"; try { CSS.registerProperty({ name: "--q", syntax: "*", inherits: true }); } catch (e) { dup = e.name; } return [getComputedStyle(p).getPropertyValue("--r"), s.getPropertyValue("--r"), s.getPropertyValue("--q"), dup].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = "9px 3px green InvalidModificationError" ] || { echo "registered custom property: got $got"; exit 1; }
# attr() takes the element's attribute: as a string, with a unit, or parsed by type(); its fallback when it is not that
got=$($B/pion-wpt "$T/docnamed.html" '(() => { const d = document.createElement("div"); d.setAttribute("data-w", "40"); d.setAttribute("data-t", "hi"); d.setAttribute("data-c", "nope"); d.style.cssText = "width: attr(data-w px); --s: attr(data-t); color: attr(data-c type(<color>), green)"; document.body.appendChild(d); const s = getComputedStyle(d); return [s.width, s.getPropertyValue("--s"), s.color].join(" "); })()' 2>/dev/null | tail -1)
[ "$got" = '40px "hi" rgb(0, 128, 0)' ] || { echo "attr(): got $got"; exit 1; }
# a frame's document in its own encoding (GBK said by its meta), not the page's UTF-8
got=$($B/pion-wpt "$T/gbkframe.html" 'frames[0].document.title' 2>/dev/null | tail -1)
[ "$got" = "编码 中文网页 GBK 你好" ] || { echo "gbk frame: got $got"; exit 1; }
# a page in GBK (said by its meta) reads as Chinese, and TextDecoder decodes GBK
got=$($B/pion-wpt "$T/gbk.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "编码 中文网页 GBK 你好" ] || { echo "gbk: got $got"; exit 1; }
# URLs parsed as the URL Standard says, a range cut out of text, and a same-origin frame's own function called
got=$($B/pion-wpt "$T/urlrange.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "http://User:pa%20ss@example.com/a/c%20d?q=1%202#f%20g | http://example.com | lo big wo | Helrld | true | framed" ] || { echo "urlrange: got $got"; exit 1; }
# a CSS transition (from the shorthand) halfway after half its time, then its events in order and its end value
got=$(FV_WAIT=2000 $B/pion-wpt "$T/transition.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "100px | transitionrun:background-color:0 | transitionstart:background-color:0 | transitionrun:width:0 | transitionstart:width:0 | rgb(100, 50, 0) 200px | transitionend:background-color:1 | transitionend:width:1 | 300px" ] || { echo "transition: got $got"; exit 1; }
# CSS Color 4/5 colors (color-mix, lab, relative, color()) as computed values; color: initial is black, not inherited
got=$($B/pion-wpt "$T/colors.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "color(srgb 0.5 0 0.5) | lab(50 20 30) | rgb(255, 128, 128) | oklch(0.7 0.1 200 / 0.5) | color(srgb 1 0 1) | color(display-p3 1 0 0) | rgb(0, 0, 0)" ] || { echo "colors: got $got"; exit 1; }
# cascade layers (order, unlayered over layered, !important reversed, sublayers, @import layer()) and @scope
got=$($B/pion-wpt "$T/layers.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "rgb(0, 0, 3) | 10px | 9px | rgb(0, 1, 0) | rgb(5, 0, 0) | 3px | 0px | 0px | 4px" ] || { echo "layers: got $got"; exit 1; }
# CSS math functions (round, mod, rem, trigonometry, pow, hypot, sign, clamp) worked out in computed values
got=$($B/pion-wpt "$T/math.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "3 | 1 | 9 | 25px | 25px | 45deg | 3s | 0.5 | 60px | 38px" ] || { echo "math: got $got"; exit 1; }
# grid content distribution, self-start by the item's direction, line names in the computed template
got=$($B/pion-wpt "$T/gridalign.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "50,60 | 0,0 | 170,0 | 40,0 | [a] 50px [b] 50px [c] | [a] 30px [b] 30px" ] || { echo "gridalign: got $got"; exit 1; }
# right to left: a narrower block against the right, a wider one overflowing left, an abspos box's static position
got=$($B/pion-wpt "$T/rtlblock.html" 'document.title' 2>/dev/null | tail -1)
[ "$got" = "180 | -100 | 40" ] || { echo "rtlblock: got $got"; exit 1; }
# a form sent into the frame its target names: the frame loads the answer (the blank frame's own load first)
got=$(FV_WAIT=3000 $B/pion-wpt "$T/formframe.html" 'document.title' 2>/dev/null | tail -1)
case "$got" in "submitted | "*"load a") ;; *) echo "formframe: got $got"; exit 1 ;; esac
# constraint validation: a form with a missing and a mistyped field is not sent until they are fixed
got=$($B/pion-wpt "$T/validate.html" 'res.join(" ")' 2>/dev/null | tail -1)
[ "$got" = "true true false 2 0 1 true" ] || { echo "validate: got $got"; exit 1; }
# what elements matched is kept between layouts: each kind of change restyles what it should
got=$($B/pion-wpt "$T/restyle.html" 'res.join(" ")' 2>/dev/null | tail -1)
[ "$got" = "rgb(255, 0, 0) rgb(0, 0, 255) rgb(0, 128, 0) rgb(128, 0, 128) rgb(1, 2, 3) rgb(255, 165, 0) rgb(4, 5, 6) rgb(7, 8, 9) rgb(10, 11, 12) rgb(13, 14, 15) rgb(16, 17, 18) rgb(0, 0, 0)" ] ||
  { echo "restyle: got $got"; exit 1; }
# a canvas's drawings are in its pixels: fills, an arc, clearRect, a clip, a canvas drawn on it, putImageData, a
# gradient stroke, text, and it reads back as a PNG
got=$($B/pion-wpt "$T/canvas.html" 'res.join(" ")' 2>/dev/null | tail -1)
[ "$got" = "255,0,0,255 0,0,255,255 0,0,0,0 0,0,0,0 0,255,0,255 0,0,0,0 255,255,0,255 10,20,30,255 255 data:image/png;base64, measured inked" ] ||
  { echo "canvas: got $got"; exit 1; }
# text decoded from part of a buffer is that part's
got=$($B/pion-wpt "$T/a.html" '(()=>{const b=new TextEncoder().encode("hello world 号码");return new TextDecoder().decode(b.subarray(6,11))+"|"+new TextDecoder().decode(new DataView(b.buffer,12))})()' 2>/dev/null | tail -1)
[ "$got" = "world|号码" ] || { echo "text decoding: got $got"; exit 1; }
echo "shell: all steps did what they should"
