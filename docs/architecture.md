# Architecture

```
game's Managed/*.dll ──► tools/netrecomp (C#, Mono.Cecil) ──► <gen>/m_*.c      one C function per method
 (yours: IL + corlib)     types, layouts, vtables,             <gen>/nrgen.h    structs, prototypes
                          RTA reachability, IL -> C            <gen>/meta.c     class records, vtables,
                                                                                descriptors, attributes,
                                                                                string literals
                                                               <gen>/chartab.c, fmttab.c  corlib tables
                                                                        │
runtime/*.c + bdwgc (deps/) ────────────────────────────────────────────┤ tools/build.py (clang-cl, x86)
                                                                        ▼
                                  mono.dll  (drop-in for a Unity 4 player)   or   test.exe (host_exe.c)
```

| Part | Language | Owns |
|---|---|---|
| `tools/netrecomp/Types.cs` | C# | concrete types and methods: every generic instantiated, resolution across assemblies |
| `tools/netrecomp/Layout.cs` | C# | C types, field layout, vtables, interface maps |
| `tools/netrecomp/Gen.cs` | C# | reachability (RTA), icall / P/Invoke / delegate wrappers, output files |
| `tools/netrecomp/Emit.cs` | C# | IL -> C for one method: stack typing, SEH regions, calls, dispatch |
| `tools/netrecomp/Meta.cs` | C# | `nrgen.h` and `meta.c`: everything the runtime and the mono API answer from |
| `tools/netrecomp/Attrs.cs` | C# | custom attributes as constructor factories |
| `runtime/nr.h` | C | the contract between generated code and the runtime |
| `runtime/nr_core.c` | C | allocation, casts, dispatch, exceptions, strings, cctors, icall registry, P/Invoke, GC handles |
| `runtime/nr_corlib.c` | C | mscorlib internal calls (objects, arrays, math, time, threads, monitors, file I/O) |
| `runtime/nr_reflect.c` | C | reflection internal calls from the class records |
| `runtime/host_exe.c` | C | console host for conformance tests |
| `tools/build.py` | Python | compiles a generated tree + runtime + bdwgc |
| `tools/conformance.py` | Python | C# tests vs the real CLR |

## Why these choices

- **IL, not x86.** A Unity game's `Shadowrun.exe` is Unity's generic player; the game is
  IL in `Assembly-CSharp.dll`, run by mono's JIT. pcrecomp lifts the player; netrecomp
  compiles the IL.
- **A drop-in `mono.dll`.** Unity 4's player loads `Mono/mono.dll` and calls about 180
  `mono_*` functions. Replacing that DLL keeps the player (and its renderer, audio, input)
  unchanged and swaps the JIT for compiled code. Object layout therefore follows mono's
  32-bit ABI; see [runtime.md](runtime.md).
- **Cecil in C#, not a Python metadata reader.** Metadata, signatures and generic
  resolution are the error-prone part of an IL compiler; Cecil has them right.
- **Full generic instantiation.** No shared code, no runtime generic context: each
  `List<T>` is compiled per `T`. Simple, fast, and the only cost is code size.
- **x86 only.** Unity 4 players are 32-bit, and the generated code assumes 4-byte pointers.
