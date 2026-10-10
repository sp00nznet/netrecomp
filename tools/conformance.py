"""Conformance harness: every conformance/tests/*.cs is compiled against the
target corlib, run on the real CLR (the oracle), recompiled with netrecomp,
run again, and the two outputs (stdout + exit code) compared.

    py -3 tools/conformance.py --corlib "D:\\Games\\Shadowrun Returns\\Shadowrun_Data\\Managed"
    py -3 tools/conformance.py --update-baseline ...

The corlib is the game's own mscorlib (mono's 2.0 profile); it isn't
redistributable, so without one every test reports SKIP and the run passes.
Fails when the pass count drops below conformance/baseline.json.
"""
import argparse, json, os, subprocess, sys, glob, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CSC = r"C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe"
CONFIG = '<?xml version="1.0"?><configuration><startup><supportedRuntime version="v4.0"/></startup></configuration>'


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corlib", default=os.environ.get("NR_CORLIB"))
    ap.add_argument("--work", default=os.path.join(ROOT, "scratch", "conformance"))
    ap.add_argument("--only")
    ap.add_argument("--update-baseline", action="store_true")
    a = ap.parse_args()

    tests = sorted(glob.glob(os.path.join(ROOT, "conformance", "tests", "*.cs")))
    if a.only:
        tests = [t for t in tests if os.path.splitext(os.path.basename(t))[0] == a.only]
    if not a.corlib or not os.path.exists(os.path.join(a.corlib, "mscorlib.dll")):
        for t in tests:
            print(f"SKIP {os.path.basename(t)}: no corlib (pass --corlib or set NR_CORLIB)")
        print(f"conformance: 0/{len(tests)} passed, {len(tests)} skipped")
        return 0

    r = run(["dotnet", "build", "-c", "Release", os.path.join(HERE, "netrecomp")])
    if r.returncode:
        print(r.stdout[-3000:]); return 1
    nr = os.path.join(HERE, "netrecomp", "bin", "Release", "net8.0", "netrecomp.dll")
    os.makedirs(a.work, exist_ok=True)
    passed, results = 0, {}
    for src in tests:
        name = os.path.splitext(os.path.basename(src))[0]
        d = os.path.join(a.work, name)
        os.makedirs(d, exist_ok=True)
        exe = os.path.join(d, name + ".exe")
        refs = [f"-r:{os.path.join(a.corlib, x)}" for x in ("mscorlib.dll", "System.dll", "System.Core.dll")]
        r = run([CSC, "-nologo", "-nostdlib", "-noconfig", "-unsafe", "-out:" + exe] + refs + [src])
        if r.returncode:
            results[name] = "FAIL (csc)"; print(f"FAIL {name}: csc\n{r.stdout[-2000:]}"); continue
        with open(exe + ".config", "w") as f:
            f.write(CONFIG)
        oracle = run([exe])
        gen = os.path.join(d, "gen")
        r = run(["dotnet", nr, "lift", a.corlib, "--exe", exe, "-o", gen])
        if r.returncode:
            results[name] = "FAIL (lift)"; print(f"FAIL {name}: lift\n{(r.stdout + r.stderr)[-2000:]}"); continue
        out = os.path.join(d, "bin", name + ".exe")
        r = run([sys.executable, os.path.join(HERE, "build.py"), "--gen", gen, "--out", out, "--exe", "--opt=-O1"])
        if r.returncode:
            results[name] = "FAIL (build)"; print(f"FAIL {name}: build\n{r.stdout[-2000:]}"); continue
        try:
            got = run([out], timeout=120)
        except subprocess.TimeoutExpired:
            results[name] = "FAIL (timeout)"; print(f"FAIL {name}: timeout"); continue
        same = got.stdout == oracle.stdout and got.returncode == oracle.returncode
        if same:
            passed += 1; results[name] = "PASS"; print(f"PASS {name}")
        else:
            results[name] = "FAIL (output)"
            exp, act = oracle.stdout.splitlines(), got.stdout.splitlines()
            first = next((i for i in range(max(len(exp), len(act))) if i >= len(exp) or i >= len(act) or exp[i] != act[i]), 0)
            print(f"FAIL {name}: line {first + 1}: expected {exp[first] if first < len(exp) else '<eof>'!r}, got {act[first] if first < len(act) else '<eof>'!r}"
                  f" (exit {oracle.returncode} vs {got.returncode})")
            if got.stderr:
                print("  " + got.stderr.strip().splitlines()[-1])
    print(f"conformance: {passed}/{len(tests)} passed")
    base_file = os.path.join(ROOT, "conformance", "baseline.json")
    base = json.load(open(base_file)) if os.path.exists(base_file) else {"passed": 0}
    if a.update_baseline:
        json.dump({"passed": passed, "total": len(tests), "results": results}, open(base_file, "w"), indent=2)
        print("baseline updated")
    elif passed < base.get("passed", 0):
        print(f"REGRESSION: {passed} < baseline {base['passed']}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
