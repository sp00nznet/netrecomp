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
  and GOG) lifts 88867/88868 methods.
- Runtime: mono's 32-bit object layout, bdwgc (conservative, parallel mark, thread-local
  allocation), SEH exceptions with hardware faults mapped to managed ones, static
  constructors, monitors, threads, GC handles, mscorlib internal calls for objects,
  strings, arrays, math, environment, time, file I/O, encodings and culture (invariant),
  and reflection over the class records.
- `NR_DEBUG=1`: symbolized native stacks for every throw and fault.
- `tools/build.py` (clang-cl x86, parallel, incremental), `tools/fetch_deps.py` (bdwgc
  pinned by SHA-256), `tools/conformance.py` with a pass-count baseline: 9/9 (basics,
  collections, hashing, interop, io, runtime, seh, strings, structs).
- Unity liveness (`mono_unity_liveness_*`): a conservative walk from statics and from each
  root Unity names, reporting `UnityEngine.Object`s per call. Without it, scene loads and
  `UnloadUnusedAssets` unloaded atlases and prefabs the UI still used.
- Field types that are closed generic classes (Unity serializes `List<T>` fields) get their
  default constructor compiled.
- `MissingMethodException` for an uncompiled method names its class.
- `NR_TRACE=1` also logs each `mono_runtime_invoke` target and freed GC handle.
- `NR_INPUT=<file>`: scripted mouse and keyboard for the player window (patched
  `GetCursorPos`/`GetAsyncKeyState`/`GetKeyState` plus posted messages), for driving a game
  on a virtual monitor. `NR_TRACE_ICALLS=1` logs every internal call. `NR_DEBUG` fault
  reports annotate the stack around ESP; `nr_debug_watch` arms a hardware write watchpoint.
- `RNGCryptoServiceProvider` (system RNG): `Guid.NewGuid` and everything behind it.
- `AssemblyName` carries the assembly's version.

### Fixed
- Exception lowering no longer uses `__finally` or filters that write locals: in functions
  clang realigns, the outlined handlers wrote through a stale base pointer and smashed the
  frame (a crash loading a Shadowrun save). Finally is a recording `__except` plus the body
  inline; `leave` goes through an exit label and a dispatch switch.
- `String.Split` kept its parts in unscanned memory, so a collection mid-split could free
  them.
- `CompareInfo` ignored `OrdinalIgnoreCase` (`EndsWith(x, OrdinalIgnoreCase)` was false).
- Enums get vtables even when only the runtime boxes them (`Array.GetValue` on an array
  from `CreateInstance`); `List<T>` ctors are rooted for fields inherited from generic bases.
