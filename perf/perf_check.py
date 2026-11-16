#!/usr/bin/env python3
"""Checks the fast-scroll test's "[fill] phase=..." lines against so/perf/budget.json.

  perf_check.py --device windows-2cpu --cache warm [--tag BUILD] [--budget budget.json] [--history history.jsonl] LOG [LOG...]

Each LOG is one run (one app start). Per phase and metric the P90 over runs is taken as the worse tail (the highest for
a "max" budget, the lowest for a "min" one; with 5 runs that is the worst run). A passing metric whose runs spread by
more than 15% of max(|median|, the budget value) is "noisy, rerun", not a pass. Exit: 0 pass, 1 fail, 2 noisy only,
3 nothing to check. The result is appended to the history file. Standard library only."""
import argparse, fnmatch, json, math, os, statistics, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SECTIONS = ("ttf", "frames", "key")


def num(v):
    v = v.rstrip("%")
    if v.endswith("MB"):
        v = v[:-2]
    try:
        return float(v)
    except ValueError:
        return None


def parse_line(line):
    """One [fill] line -> (phase, {metric: float})."""
    toks = line.split()
    m, sec, phase = {}, "", None
    for t in toks[1:]:
        if "=" not in t:
            sec = t if t in SECTIONS else sec
            continue
        k, v = t.split("=", 1)
        if k == "phase":
            phase = v
            continue
        name = sec + "." + k if sec else k
        if k == "keys":
            m["keys_ok"] = 1.0 if v == "ok" else 0.0
        elif k == "box" and "/" in v:
            a, b = v.split("/")
            if int(b) > 0:
                m["box_pct"] = 100.0 * int(a) / int(b)
        else:
            x = num(v)
            if x is not None:
                m[name] = x
    if "frames.per" in m and m["frames.per"] > 0 and "frames.p95" in m:
        m["frames.p95_periods"] = m["frames.p95"] / m["frames.per"]
    return phase, m


def load_run(path):
    runs = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("[fill] phase="):
                p, m = parse_line(line)
                if p:
                    runs[p] = m
    return runs


def p90(vals, worse_high):
    s = sorted(vals)
    i = max(0, min(len(s) - 1, math.ceil(0.9 * len(s)) - 1))
    return s[i] if worse_high else s[len(s) - 1 - i]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", required=True)
    ap.add_argument("--cache", default="warm")
    ap.add_argument("--tag", default="")
    ap.add_argument("--budget", default=os.path.join(HERE, "budget.json"))
    ap.add_argument("--history", default=os.path.join(HERE, "history.jsonl"))
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("logs", nargs="+")
    a = ap.parse_args()
    budget = json.load(open(a.budget, encoding="utf-8"))
    rules = budget["devices"][a.device]["rules"]
    spread_max = budget.get("noisy_spread", 0.15)
    runs = [load_run(p) for p in a.logs]
    runs = [r for r in runs if r]
    if not runs:
        print("perf_check: no [fill] phase lines in", a.logs)
        return 3
    fails, noisy, passes, results = [], [], 0, {}
    for rule in rules:
        if rule["cache"] not in ("*", a.cache):
            continue
        for phase in sorted({p for r in runs for p in r}):
            if not fnmatch.fnmatch(phase, rule["phase"]):
                continue
            vals = [r[phase][rule["metric"]] for r in runs if phase in r and rule["metric"] in r[phase]]
            if not vals:
                continue
            hi = "max" in rule
            lim = rule["max"] if hi else rule["min"]
            v = p90(vals, hi)
            ok = v <= lim if hi else v >= lim
            results.setdefault(phase, {})[rule["metric"]] = {"p90": v, "limit": lim, "vals": vals, "ok": ok}
            tag = "%s %s" % (phase, rule["metric"])
            msg = "%-34s p90=%-8.4g %s %-6g runs=%s" % (tag, v, "<=" if hi else ">=", lim, [round(x, 3) for x in vals])
            if not ok:
                fails.append(msg)
            else:
                spread = max(vals) - min(vals)
                if len(vals) > 1 and spread > spread_max * max(abs(statistics.median(vals)), abs(lim), 1e-9):
                    noisy.append(msg)
                else:
                    passes += 1
    if not a.quiet:
        for f in fails:
            print("FAIL  ", f)
        for n in noisy:
            print("NOISY ", n, " (rerun)")
    verdict = "FAIL" if fails else "NOISY" if noisy else "PASS"
    print("perf_check %s device=%s cache=%s runs=%d: %d pass, %d fail, %d noisy%s" %
          (verdict, a.device, a.cache, len(runs), passes, len(fails), len(noisy), " tag=" + a.tag if a.tag else ""))
    try:
        with open(a.history, "a", encoding="utf-8") as h:
            h.write(json.dumps({"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "tag": a.tag, "device": a.device,
                                "cache": a.cache, "runs": len(runs), "verdict": verdict, "fails": len(fails),
                                "noisy": len(noisy), "results": results}) + "\n")
    except OSError as e:
        print("perf_check: history not written:", e)
    return 1 if fails else 2 if noisy else 0


if __name__ == "__main__":
    sys.exit(main())
