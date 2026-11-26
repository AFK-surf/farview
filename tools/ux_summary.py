#!/usr/bin/env python3
"""Summarises tools/ux_flows.sh timelines (UX events, one JSON a line) as a table, one row a flow step:
ms after the pointer went down until the press showed (press), loading showed (load), the new address reached the
client (url; Chrome: its navigation committed), the new page was first drawn (paint; Chrome: first contentful paint), loading ended (loaded), the page was usable (usable); the pointer's shape over
the target (cursor: 1 = hand), whether the client's hit test under the pointer found the link meant (hit), and what
went wrong (missed clicks, a second press needed, a new tab not opened or this page not kept). -1 = never."""
import json
import sys

CURSOR = {0: "arrow", 1: "hand", 2: "ibeam"}


def main(path):
    rows = []
    cur = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                e = json.loads(line)
            except ValueError:
                continue
            key = (e.get("flow"), e.get("variant"), e.get("label"))
            ev = e.get("ev")
            if ev == "ready":
                rows.append(("ready", e))
            elif ev == "slow_frame":
                lbl = str(e.get("label", "")).replace(" (second press)", "")
                k = (e.get("flow"), e.get("variant"), lbl)
                sl = cur.setdefault(k, {}).setdefault("slow", [])
                sl.append(e)
            elif ev in ("target", "press", "missed", "after_tab", "scrolled"):
                cur.setdefault(key, {})[ev] = e
            elif ev == "timeline":
                base = key if not str(e.get("label", "")).endswith("(second press)") else None
                info = cur.get(key, {}) if base else {}
                rows.append(("step", e, info))
                if base:
                    pass
            elif ev == "error":
                rows.append(("error", e))
    hdr = f"{'flow/variant':22} {'step':26} {'press':>5} {'load':>5} {'url':>6} {'paint':>6} {'loaded':>6} {'usable':>6} {'stall':>5} cursor hit  notes"
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        if r[0] == "ready":
            e = r[1]
            print(f"{e['flow'] + '/' + e['variant']:22} {'(start page ready)':26} {'':>5} {'':>5} {'':>6} {'':>6} {'':>6} {e['t']:>6}       first page {e['url']}")
            continue
        if r[0] == "error":
            e = r[1]
            print(f"{e.get('flow','') + '/' + e.get('variant',''):22} {e.get('label','')[:26]:26} ERROR {e.get('why')}")
            continue
        _, e, info = r
        if e.get("variant") == "chrome":
            notes = []
            if ("cmdclick" in e["label"] or "middleclick" in e["label"]) and e.get("new_tab", -1) < 0:
                notes.append("NO NEW TAB")
            print(
                f"{e['flow'] + '/chrome':22} {e['label'][:26]:26} {'':>5} {e['loading']:>5} {e['url']:>6} {e['paint']:>6} {e['loaded']:>6} {e.get('done', -1):>6} {'':>5} {'':6} {'':4} DCL {e.get('dcl')} {'; '.join(notes)}"
            )
            continue
        tgt = info.get("target", {})
        prs = info.get("press", {})
        notes = []
        if tgt:
            if not tgt.get("hover_ok", True):
                notes.append(f"WRONG TARGET: meant {tgt.get('href')} hover {tgt.get('hover_url') or '-'} hit {tgt.get('hit')}")
        wants_tab = e["new_tab"] >= 0 or "cmdclick" in e["label"] or "middleclick" in e["label"]
        if "cmdclick" in e["label"] or "middleclick" in e["label"]:
            if e["new_tab"] < 0:
                notes.append("NO NEW TAB")
            if e["from"] != e["to"]:
                notes.append("PAGE REPLACED (should stay)")
            if e["active_switched"]:
                notes.append("switched to new tab")
        elif e["url"] < 0:
            notes.append("NOTHING HAPPENED")
        if "missed" in info:
            notes.append("second press " + ("worked" if info["missed"].get("second_press_worked") else "did not work either"))
        if e["loading"] < 0 and e["url"] >= 0:
            notes.append("no loading state shown")
        u_in = e.get("url_in", e["url"])
        if e["paint"] >= 0 and u_in >= 0 and e["paint"] - u_in > 150:
            notes.append(f"new page drawn {e['paint'] - u_in} ms after it arrived (old page frozen meanwhile)")
        if e["scroll_moves"] and "scroll" in e["label"]:
            notes.append(f"scroll moved {e['scroll_moves']}x, ended at {e['scroll_end']}")
        if e["urls"] > 1:
            notes.append(f"{e['urls']} address changes")
        slow = info.get("slow", [])
        stall = max([x["ms"] for x in slow], default=0)
        if slow:
            notes.append(f"{len(slow)} frames >=50ms ({sum(x['ms'] for x in slow)} ms frozen)")
        if tgt and tgt.get("cursor") != 1:
            sc = tgt.get("stage_cursor")
            notes.append("no hand cursor over link" + (" (the page asks for the hand, the window is not told)" if sc == 1 else f" (stage asks {sc})"))
        if "scrolled" in info:
            sc = info["scrolled"]
            notes.append(f"scrolled {sc['notches']} notches in {sc['ms']} ms to reach it")
        cur_s = CURSOR.get(tgt.get("cursor"), str(tgt.get("cursor", prs.get("cursor", "")))) if (tgt or prs) else ""
        hit = "" if not tgt else ("ok" if tgt.get("hover_ok") else "BAD")
        fb = prs.get("press_feedback", -1) if prs else e["feedback"]
        print(
            f"{e['flow'] + '/' + e['variant']:22} {e['label'][:26]:26} {fb:>5} {e['loading']:>5} {e.get('url_in', e['url']):>6} {e['paint']:>6} {e['loaded']:>6} {e['usable']:>6} {stall:>5} {cur_s:6} {hit:4} {'; '.join(notes)}"
        )


if __name__ == "__main__":
    main(sys.argv[1])
