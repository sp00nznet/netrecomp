# Roadmap

## Next
- **The Unity host:** `runtime/host_mono.c` and `mono.def`, the ~180 `mono_*` functions
  Unity 4's player calls (domains, images, classes, methods, fields, strings, arrays,
  `mono_runtime_invoke`, GC handles, `mono_add_internal_call`, custom attributes).
- A headless run of a Unity title: the player has `-batchmode -nographics`; recording needs
  the window (offstage).
- Delegates marshalled to native function pointers (Steamworks callbacks).
- Property custom attributes and parameter names in the descriptors.
- netlab recipe so builds run on the farm.

## Later
- pcrecomp-lift the Unity player itself, for a build with no JIT-era binary in it at all.
- ANSI `Encoding.Default` through the I18N assemblies, if a title reads text through it.
- A real thread pool and `IAsyncResult` for `BeginInvoke`/`EndInvoke`.
- XNA / plain .NET Framework hosts (a different host over the same compiler).

## Out of scope
- Shipping generated code or any game data.
- Reflection.Emit and anything else that generates IL at run time.
- 64-bit Unity players (5.x and later): the generated code assumes 4-byte pointers today.
