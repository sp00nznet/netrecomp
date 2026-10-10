"""Fetch the pinned third-party sources the runtime links: bdwgc.

    py -3 tools/fetch_deps.py

Downloads into deps/ (gitignored), checks the SHA-256 and unpacks. build.py
runs this itself when deps/ is missing.
"""
import hashlib, os, sys, tarfile, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = [
    ("gc-8.2.8", "https://github.com/ivmai/bdwgc/releases/download/v8.2.8/gc-8.2.8.tar.gz",
     "7649020621cb26325e1fb5c8742590d92fb48ce5c259b502faf7d9fb5dabb160"),
]


def fetch():
    d = os.path.join(ROOT, "deps")
    os.makedirs(d, exist_ok=True)
    for name, url, sha in DEPS:
        if os.path.isdir(os.path.join(d, name)):
            continue
        tgz = os.path.join(d, name + ".tar.gz")
        if not os.path.exists(tgz):
            print(f"fetching {url}")
            urllib.request.urlretrieve(url, tgz)
        got = hashlib.sha256(open(tgz, "rb").read()).hexdigest()
        if got != sha:
            os.remove(tgz)
            sys.exit(f"{name}: SHA-256 mismatch ({got}); download removed")
        with tarfile.open(tgz) as t:
            t.extractall(d, filter="data")
        print(f"unpacked {name}")


if __name__ == "__main__":
    fetch()
