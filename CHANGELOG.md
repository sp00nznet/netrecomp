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
  and GOG) lifts 88847/88848 methods.
- Runtime: mono's 32-bit object layout, bdwgc (conservative, parallel mark, thread-local
  allocation), SEH exceptions with hardware faults mapped to managed ones, static
  constructors, monitors, threads, GC handles, mscorlib internal calls for objects,
  strings, arrays, math, environment, time, file I/O, encodings and culture (invariant),
  and reflection over the class records.
- `NR_DEBUG=1`: symbolized native stacks for every throw and fault.
- `tools/build.py` (clang-cl x86, parallel, incremental), `tools/fetch_deps.py` (bdwgc
  pinned by SHA-256), `tools/conformance.py` with a pass-count baseline: 8/8 (basics,
  collections, hashing, interop, io, runtime, strings, structs).
- Unity liveness (`mono_unity_liveness_*`): a conservative walk from statics and from each
  root Unity names, reporting `UnityEngine.Object`s per call. Without it, scene loads and
  `UnloadUnusedAssets` unloaded atlases and prefabs the UI still used.
- Field types that are closed generic classes (Unity serializes `List<T>` fields) get their
  default constructor compiled.
- `MissingMethodException` for an uncompiled method names its class.
- `NR_TRACE=1` also logs each `mono_runtime_invoke` target and freed GC handle.
