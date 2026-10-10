"""Compile a netrecomp tree (generated C + runtime) into mono.dll or a test exe.

    py -3 tools/build.py --gen gen --out build/mono.dll            (Unity drop-in)
    py -3 tools/build.py --gen gen --out build/test.exe --exe      (standalone host)

x86 only: Unity 4's player is 32-bit, and generated code assumes 4-byte
pointers. Uses clang-cl with the MSVC x86 environment from vcvarsall, one
process per file, and skips objects newer than their source.
"""
import argparse, concurrent.futures, os, subprocess, sys, glob, json

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "..", "runtime")
# bdwgc, fetched by tools/fetch_deps.py (MIT-style licence; see NOTICE)
GC_DIR = os.path.join(HERE, "..", "deps", "gc-8.2.8")
GC_DEFS = ["/DGC_THREADS", "/DGC_NOT_DLL", "/DPARALLEL_MARK", "/DTHREAD_LOCAL_ALLOC", "/DNO_EXECUTE_PERMISSION",
           "/DGC_DISABLE_INCREMENTAL", "/D_CRT_SECURE_NO_WARNINGS", "/DDONT_USE_USER32_DLL", "/DGC_BUILTIN_ATOMIC"]


def msvc_env():
    """The x86 developer environment, captured once from vcvarsall."""
    vs = subprocess.run([r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe", "-latest",
                         "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
    bat = os.path.join(vs, r"VC\Auxiliary\Build\vcvarsall.bat")
    out = subprocess.run(f'cmd /s /c ""{bat}" x86 >nul && set"', capture_output=True, text=True, shell=True).stdout
    env = dict(os.environ)
    for line in out.splitlines():
        k, _, v = line.partition("=")
        if k:
            env[k] = v
    return env


def find_clang():
    for p in [r"C:\Program Files\LLVM\bin\clang-cl.exe", "clang-cl"]:
        if os.path.exists(p) or p == "clang-cl":
            return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--exe", action="store_true", help="standalone host exe instead of mono.dll")
    ap.add_argument("--opt", default="-O2", help="dash form: Git Bash rewrites /O2 into a path")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--compile-only", action="store_true")
    a = ap.parse_args()

    if not os.path.isdir(GC_DIR):
        sys.path.insert(0, HERE)
        import fetch_deps
        fetch_deps.fetch()
    env = msvc_env()
    cc = find_clang()
    objdir = os.path.join(os.path.dirname(os.path.abspath(a.out)), "obj")
    os.makedirs(objdir, exist_ok=True)
    srcs = sorted(glob.glob(os.path.join(a.gen, "*.c")))
    srcs += [s for s in sorted(glob.glob(os.path.join(RUNTIME, "*.c"))) if not os.path.basename(s).startswith("host_")]
    srcs.append(os.path.join(GC_DIR, "extra", "gc.c"))
    host = os.path.join(RUNTIME, "host_exe.c" if a.exe else "host_mono.c")
    if os.path.exists(host):
        srcs.append(host)
    hdrs = [os.path.join(a.gen, "nrgen.h"), os.path.join(RUNTIME, "nr.h"), os.path.join(RUNTIME, "nrrt.h")]
    hdr_time = max(os.path.getmtime(h) for h in hdrs if os.path.exists(h))
    flags = ["-m32", "/c", a.opt, "/Oy-", "/Zi", "/MT", "/GS-", "-fms-extensions", "/clang:-fno-delete-null-pointer-checks", "/EHa", "/W0", "-Wno-everything",
             "-ferror-limit=50", "/I", RUNTIME, "/I", a.gen, "/I", os.path.join(GC_DIR, "include"), "/DNR_BUILD"] + GC_DEFS

    def one(src):
        base = os.path.splitext(os.path.basename(src))[0]
        obj = os.path.join(objdir, ("bdwgc_" if src.startswith(GC_DIR) else "") + base + ".obj")
        if os.path.exists(obj) and os.path.getmtime(obj) > max(os.path.getmtime(src), hdr_time):
            return obj, None
        f = flags
        if base in ("meta", "chartab", "fmttab"):
            # tables: optimizing 40 MB of initializers buys nothing and takes 15 minutes
            f = [x if x != a.opt else "-O0" for x in flags]
        r = subprocess.run([cc] + f + [src, "/Fo" + obj],
                           capture_output=True, text=True, env=env)
        return obj, (r.stdout + r.stderr if r.returncode else None)

    objs, failed = [], 0
    with concurrent.futures.ThreadPoolExecutor(a.j) as ex:
        for obj, err in ex.map(one, srcs):
            objs.append(obj)
            if err:
                failed += 1
                sys.stdout.write(err[:4000])
    if failed:
        print(f"{failed} file(s) failed to compile")
        return 1
    if a.compile_only:
        return 0
    link = ["lld-link" if os.path.exists(r"C:\Program Files\LLVM\bin\lld-link.exe") else "link"]
    if link[0] == "lld-link":
        link = [r"C:\Program Files\LLVM\bin\lld-link.exe"]
    rsp = os.path.join(objdir, "link.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join(f'"{o}"' for o in objs))
    args = link + ["/nologo", "/machine:x86", "/debug", "/out:" + a.out, "@" + rsp,
                   "kernel32.lib", "user32.lib", "ole32.lib", "oleaut32.lib", "advapi32.lib", "shell32.lib", "ws2_32.lib"]
    if not a.exe:
        args += ["/dll"]
    else:
        args += ["/subsystem:console"]
    r = subprocess.run(args, capture_output=True, text=True, env=env)
    if r.returncode:
        print(r.stdout[-6000:] + r.stderr[-2000:])
        return 1
    print("built " + os.path.abspath(a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
