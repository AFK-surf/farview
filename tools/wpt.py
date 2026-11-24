# Runs WPT directories from wpt.live and compares with Chrome's results: python3 tools/wpt.py <summary.json.gz> <out.json> <dir>...
# FV_WPT_EXE: the wpt.exe to run (a copy keeps a long run on one build while the tree is rebuilt).
# FV_WPT_BASE: where the tests are served, wpt.live by default; a local server (tools/wpt-serve.sh) is fast and gives
# every run the same answers.
import gzip, json, resource, subprocess, sys, concurrent.futures as cf
summary = json.load(open(sys.argv[1]))
out_path, dirs = sys.argv[2], sys.argv[3:]
import os
# a test page may run one long script (thousands of subtests in a loop): the host's watchdog for a page that never
# ends gives it a minute here, as a browser lets such a page run
os.environ.setdefault("FV_SCRIPT_MS", "60000")
exe = os.environ.get("FV_WPT_EXE", "out/pion-wpt")
todo = [(p, v["c"]) for p, v in summary.items() if any(p.startswith("/" + d + "/") for d in dirs) and v["c"][1] > 1]
print("tests", len(todo), flush=True)
base = os.environ.get("FV_WPT_BASE", "https://wpt.live")
local = base != "https://wpt.live"
def one(item):
    p, c = item
    # wpt.live drops a fetch now and then: a page whose harness never ran is tried again; a local server is not
    for attempt in range(1 if local else 3):
        try:
            # a limit on CPU time, not on the clock: a busy machine makes a run slower, never different
            r = subprocess.run([exe, base + p], capture_output=True, text=True, timeout=600, errors="replace",
                               preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CPU, (60, 60)))
            d = json.loads(r.stdout.splitlines()[0])
            if "none" in d and not local and attempt < 2:
                continue
            return p, c, d.get("pass", 0), d.get("total", 0)
        except Exception:
            if local or attempt == 2:
                return p, c, 0, 0
    return p, c, 0, 0
res = []
with cf.ThreadPoolExecutor(4) as ex:
    for i, r in enumerate(ex.map(one, todo)):
        res.append(r)
        if i % 50 == 0:
            json.dump(res, open(out_path, "w"))
            print(i, sum(x[2] for x in res), sum(x[1][0] for x in res), flush=True)
json.dump(res, open(out_path, "w"))
for d in dirs:
    rows = [x for x in res if x[0].startswith("/" + d + "/")]
    # a file counts no more passes than Chrome's there (a test that makes its subtests from what the browser has can
    # make more of them here)
    a, b = sum(min(x[2], x[1][0]) for x in rows), sum(x[1][0] for x in rows)
    print(f"{d}: ours {a} chrome {b} = {a * 100 / max(b, 1):.1f}%  files {len(rows)}")
