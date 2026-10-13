# netrecomp: .NET IL static recompiler

netrecomp compiles a .NET program's IL (every assembly in a Unity 4 game's
`*_Data/Managed` folder, or a console program) into C, and links it with a small runtime.
Nothing is JIT-compiled or interpreted at run time.

It's a sibling of [pcrecomp](https://github.com/sp00nznet/pcrecomp) and
[gmrecomp](https://github.com/sp00nznet/gmrecomp) and follows the same house style for
recompilation projects. pcrecomp lifts x86. A Unity game's x86 is only Unity's generic
player, and the game itself is IL run by mono, so the IL is what gets compiled here. For
Unity, the output is meant to be a drop-in `mono.dll` that the unmodified player loads.

**Generated source is not distributed.** You supply your own copy of the game;
the recompiler runs on your machine and writes C into a gitignored folder.

## Status

**v0.1.0-dev, pre-alpha.** The compiler, the corlib runtime and the Unity host (the
`mono.dll` that Unity's player loads) work well enough to play Shadowrun Returns from the
main menu into a level; most of the game is still untested.

| What | State |
|---|---|
| Translator | Shadowrun Returns (Steam and GOG, 25 assemblies): **88867/88868 methods** lift; the one left is a P/Invoke taking a non-blittable struct |
| Generated C | all files compile (clang-cl, x86) |
| Runtime: core, GC, exceptions, threads, file I/O, reflection | conformance tests pass |
| Runtime: mono embedding API (`mono.dll` for Unity) | Unity 4.2's player boots, renders, loads saves and runs gameplay; scripted input for automated runs |

Conformance (current corpus): **9/9 tests** match the real CLR's output and exit code.
See [Conformance](#conformance).

| Title | Repo | State |
|---|---|---|
| Shadowrun Returns (2013, Unity 4.2) | [shadowrun-returns-recomp](https://github.com/sp00nznet/shadowrun-returns-recomp) | in game (Steam): menus, New Game, character creation, loading saves, exploration |

## Screenshots

Shadowrun Returns (Steam) running on the recompiled `mono.dll`:

![Main menu](https://raw.githubusercontent.com/sp00nznet/shadowrun-returns-recomp/main/docs/screenshots/main-menu-steam.png)

![In game](https://raw.githubusercontent.com/sp00nznet/shadowrun-returns-recomp/main/docs/screenshots/in-game-steam.png)

## Getting Started

Most people want a game repo, which fetches this toolkit itself. To use the toolkit
directly:

1. Install **Python 3.10+**, the **.NET 8 SDK**, **Visual Studio 2022** with the C++
   workload, and **LLVM** (clang-cl) 18 or newer.
2. Build the translator:
   ```
   dotnet build -c Release tools\netrecomp
   ```
   Expected output ends with `Build succeeded.`
3. Recompile a game's managed code:
   ```
   dotnet tools\netrecomp\bin\Release\net8.0\netrecomp.dll lift "D:\Games\Shadowrun Returns\Shadowrun_Data\Managed" -o gen
   ```
   Expected output: `lifted 88867/88868 methods (4413 icalls, 450/451 pinvokes) in 8.5s -> gen`
4. Compile it:
   ```
   py -3 tools\build.py --gen gen --out build\mono.dll --opt=-O1
   ```
   The first run fetches bdwgc into `deps\`. Copy the result over the game's
   `*_Data\Mono\mono.dll` (keep the original).

Usual trip-ups: `python` opening the Microsoft Store means the Store alias is on; use `py`.
In Git Bash, pass compiler flags in dash form (`--opt=-O1`): Bash rewrites `/O1` into a path.

## Usage

```
netrecomp lift <Managed dir> -o <gen dir> [--exe program.exe] [--report failures.txt]
netrecomp stats <Managed dir>           opcode histogram and feature counts
netrecomp dump <dll> <Ns.Type>...       fields and method signatures
py -3 tools\build.py --gen <gen dir> --out <file> [--exe] [--opt=-O1] [--compile-only]
py -3 tools\conformance.py --corlib <Managed dir>
```

- `--exe` compiles a console program against the folder's corlib, and `build.py --exe`
  links it with `runtime/host_exe.c`.
- `NR_DEBUG=1` makes the runtime print every throw and hardware fault with a symbolized
  native stack and an annotated dump of the faulting stack.
- `NR_TRACE=1` logs every mono API call Unity makes (with invoke targets and icall
  registrations); `NR_TRACE_ICALLS=1` logs every internal call managed code makes;
  `NR_LOG=<file>` sends all of it to a file.
- `NR_INPUT=<file>` drives the game window from a command file (`click X Y`, `move`,
  `key VK`, `wait MS`) without touching the real mouse; see `docs/unity-host.md`.

## Conformance

`tools/conformance.py` compiles each `conformance/tests/*.cs` against the game's own
mscorlib (mono's 2.0 profile), runs it on the real CLR as the oracle, recompiles it, runs
that, and compares stdout and the exit code. The pass count is checked against
`conformance/baseline.json`; a drop fails the run.

The corlib isn't redistributable, so the harness needs a game's `Managed` folder
(`--corlib`, or `NR_CORLIB`). Without one every test reports SKIP.

| Test | Covers |
|---|---|
| `basics` | arithmetic, strings, formatting, virtual/interface dispatch, generics, structs, boxing, enums, iterators, lambdas, multicast delegates, try/catch/finally, null/div-by-zero/range/cast/overflow exceptions, GC churn |
| `collections` | List, Dictionary with struct keys, HashSet, Queue, Stack, SortedDictionary, LinkedList, ArrayList, Hashtable, LINQ, string switch, array covariance |
| `runtime` | parsing and formatting, [Flags] enums, reflection (fields, methods, properties, Activator), nullables, struct operators, threads and locks, thread pool, nested finally, file I/O, Guid |
| `interop` | P/Invoke: stdcall and cdecl, string and array marshalling, bool returns, a managed callback from native code |
| `io` | File and Directory existence, Path helpers, a write/read round trip |
| `strings` | Format, Concat, Replace, Split, case, Contains, ordinal and ignore-case comparisons |
| `hashing` | Hashtable, OrderedDictionary and Dictionary with keys overriding GetHashCode/Equals |
| `structs` | value-type arrays: ref locals into elements, a generic list of structs, struct operators, Array.Copy |
| `seh` | foreach over struct enumerators with continue/break/throw, nested finally, return from try, exceptions crossing finally |

## Documentation

- [docs/architecture.md](docs/architecture.md): the parts and why they're shaped this way
- [docs/recompiler.md](docs/recompiler.md): reachability, lowering, and the build flags that matter
- [docs/runtime.md](docs/runtime.md): object model, GC, exceptions, icalls, reflection
- [docs/unity-host.md](docs/unity-host.md): the mono API Unity uses, liveness, scripted input

## Building from source

`tools/build.py` compiles the generated C, `runtime/*.c` and bdwgc's `extra/gc.c` with
clang-cl for x86 (`-m32 /MT /EHa /Oy- -fno-delete-null-pointer-checks`) in parallel, and
links with lld-link. The translator is a .NET 8 console app using Mono.Cecil.

## License

MIT, see [LICENSE](LICENSE). Third-party components are listed in [NOTICE](NOTICE).
