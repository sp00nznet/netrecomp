# The recompiler

`tools/netrecomp` reads every assembly in a folder (a Unity game's `*_Data/Managed`) and
writes C. Run it with:

```
dotnet tools\netrecomp\bin\Release\net8.0\netrecomp.dll lift <Managed dir> -o gen [--exe test.exe] [--report fail.txt]
```

## What gets compiled

Reachability is rapid type analysis:

- **Roots.** Every non-generic method of every non-framework assembly (the game,
  `UnityEngine`, plugins), because Unity's native side finds those by name. The framework
  (`mscorlib`, `System*`, `Mono.Security`, `I18N*`, `Boo.Lang`, `UnityScript.Lang`) is
  compiled only where reached. `--exe` adds a program's entry point and its own assembly.
- **Calls** compile their target. **Construction** (`newobj`, `box`, `newarr`, attribute
  types) compiles the type's whole vtable and interface map, since a virtual call can land
  on any of them.
- **Generic virtual methods** get, per call site, the override from every constructed type,
  instantiated at the call's arguments.
- **Reflection the IL can't show** gets hints: `EqualityComparer<T>` and `Comparer<T>` build
  `GenericEqualityComparer<T>` / `GenericComparer<T>` with `MakeGenericType`, so those are
  compiled when `T` implements the interface (`Gen.AotHints`).

A method that can't be translated still gets a body: it throws `NotSupportedException`
naming the method and the reason, and is counted in the report. Shadowrun Returns today:
`lifted 88432/88433 methods`; the one left is a P/Invoke taking a struct with a `bool`
field by value. The conformance tests lift 100%.

## Lowering

- **Eval stack:** each slot is a C variable named by depth and C type (`s2i`, `s0p`,
  `s1v17`), so nothing moves at branches. Kinds: I4, I8, R4, R8, native int, pointer/object,
  value type. A join where paths push a native int and a pointer (a `fixed` statement)
  settles on native int, and each edge into it converts (`EdgeMoves`).
- **Exceptions** are Win32 SEH with no `__finally` and no filter writing to a local. Clang
  outlines x86 filters and `__finally` blocks, and in a function that realigns its stack
  (clang does that for SSE spills) the outlined code addresses the parent's locals through
  a stale base pointer: Shadowrun's `createSharedPortraitAtlases` overwrote its own return
  address that way. So:
  - catch: one `__except` for all clauses of a try; the filter (`nr_catch_any` with null
    outs) parks the match per thread and the handler body takes it with `nr_caught`.
  - finally: a catch-all `__except` that only records the exception, then the finally body
    inline after it (both paths), then `if (ex) nr_throw(ex)`.
  - `leave` out of such a try sets `lv<off>` and jumps to an exit label at the end of the
    `__try`; after the finally body a `switch` continues to the target, repeating at each
    enclosing try.
  A label on a try's first instruction is placed outside the `__try`, because C can't jump
  into one.
- **Null references and integer division by zero** are hardware faults that the catch filter
  turns into managed exceptions (as mono did). Two compiler flags make that true in C; see
  [Build notes](#build-notes).
- **Calls:** direct calls to the C function; virtual calls through `NR_VT(o)[slot]`;
  interface calls through `nr_iface`; `constrained.` resolves at compile time (the type is
  always concrete). `callvirt` to a non-virtual method gets an explicit null check.
- **Static constructors:** precise types check on static method / constructor entry;
  `beforefieldinit` types check at every static field access, including from their own
  methods. One recursive lock serialises all cctors (a ceiling noted in `nr_core.c`).
- **Strings:** `newobj String::.ctor(X)` becomes `String::CreateString(X)` on a null `this`,
  which is how mono's 2.x class library expects it.
- **Delegates:** `Invoke` walks mono's `prev` chain, then calls the target with it as the
  first argument when it is non-null (instance methods and closed static methods alike).
  `BeginInvoke` runs `Invoke` on a new thread; there is no `IAsyncResult`.

## Build notes

Things that each cost an afternoon:

- `-fno-delete-null-pointer-checks` (`/clang:` prefix under clang-cl). Without it, clang
  folds `obj = null; obj.ToString()` into undefined behaviour and the program executes
  garbage instead of faulting into a `NullReferenceException`.
- `/EHa`. Without async exceptions, clang's `__try` only covers *calls*: a fault inside the
  same function as the `__try` sails past the filter.
- `/Oy-`. Frame pointers, so `NR_DEBUG=1` can print native stacks of throws and faults.
- In Git Bash, `/O2` is rewritten into a path. `build.py` takes dash forms (`--opt=-O1`).
- Cecil's resolver must hand back the assemblies already loaded (`Resolver` in
  `Types.cs`), by simple name: compact-framework assemblies reference mscorlib 3.5, and a
  second copy of a definition breaks every lookup keyed by definition.
