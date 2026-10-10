# The Unity host (`mono.dll`)

`runtime/host_mono.c` implements the mono embedding API that a Unity 4 player calls,
so a netrecomp build can replace `<Game>_Data/Mono/mono.dll`. The player resolves its
mono functions by name with `GetProcAddress`; for Unity 4.2 that is exactly 180
`mono_*` functions plus `g_free`, and it logs `mono: function X not found` for any
that are missing. The list came from the strings in the player executable.

## Handle types

| mono type | here |
|---|---|
| `MonoDomain*` | a static record: the root domain and one child; every domain sees the same compiled world |
| `MonoImage*`, `MonoAssembly*` | `NrImage*`, the same pointer for both |
| `MonoClass*`, `MonoVTable*` | `NrClass*` |
| `MonoType*` | `NrType*`: `&klass->byval_arg`, or `this_arg` when byref |
| `MonoMethod*`, `MonoMethodSignature*` | `NrMethodDesc*` |
| `MonoClassField*` | `NrFieldDesc*` |
| `MonoProperty*` | `NrProp*`, synthesized from `get_`/`set_` methods |
| `MonoCustomAttrInfo*` | a malloc'd `{ table, count }`, freed by `mono_custom_attrs_free` |
| objects, strings, arrays, threads | the managed objects themselves (mono's layout) |

## Loading assemblies

Unity reads each DLL in `Managed` and passes the bytes to
`mono_image_open_from_data_with_name`. The host matches the file name against the images
the build was generated from, ignores the bytes for code, and hashes them: an assembly that
differs from the one the build was made from (a Steam build run on a GOG copy, say) logs
`... differs from the assembly this mono.dll was built from`. Unity then enumerates an
image's classes with `mono_image_get_table_rows(image, TYPEDEF)` and `mono_class_get`
by token; the records keep each type's metadata token for that.

## Behaviour that differs from mono

- `mono_runtime_invoke` calls the exact method given (no virtual dispatch), boxes value
  results, and returns exceptions through `exc` when it is non-null.
- `mono_class_get_method_from_name` searches the class itself, as mono's does; fields and
  properties search the parents too.
- `mono_stack_walk`, `mono_method_get_last_managed` and `mono_pmip` report nothing, so
  Unity's log lines carry no managed stack traces.
- The liveness functions walk the heap conservatively (any word that holds the start of a
  GC object with a class record is a reference), from class statics and from each root
  Unity passes. Results go to Unity's callback at the end of every `from_root` and
  `from_statics` call, since Unity attributes them to that root; flushing only at `end`
  made scene loads unload ~1700 assets the menu still used. The visited set spans the
  whole calculation. Over-reporting only keeps an asset loaded.
- `MonoAssemblyName` is touched only through its first member, the name.
- P/Invoke resolution tries Unity's plugin callback (`mono_set_find_plugin_callback`) and
  its registered dl fallback before `LoadLibrary`.
