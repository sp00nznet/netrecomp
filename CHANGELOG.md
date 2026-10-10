# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versions: [SemVer](https://semver.org/).

## [Unreleased]

### Added
- `tools/netrecomp`: IL -> C translator on Mono.Cecil. Full generic instantiation, rapid
  type analysis with generic virtual methods, vtables and interface maps (ECMA-335 II.12.2),
  SEH-based exception regions, stack-kind merging at joins, icall and P/Invoke wrappers,
  delegate `Invoke`/`BeginInvoke`, custom attributes as factories, class records and
  reflection descriptors, Unicode and number-formatting tables. Shadowrun Returns (Steam
  and GOG) lifts 88432/88433 methods.
- Runtime: mono's 32-bit object layout, bdwgc (conservative, parallel mark, thread-local
  allocation), SEH exceptions with hardware faults mapped to managed ones, static
  constructors, monitors, threads, GC handles, mscorlib internal calls for objects,
  strings, arrays, math, environment, time, file I/O, encodings and culture (invariant),
  and reflection over the class records.
- `NR_DEBUG=1`: symbolized native stacks for every throw and fault.
- `tools/build.py` (clang-cl x86, parallel, incremental), `tools/fetch_deps.py` (bdwgc
  pinned by SHA-256), `tools/conformance.py` with a pass-count baseline: 3/3.
