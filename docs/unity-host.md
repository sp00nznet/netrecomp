# The Unity host (`mono.dll`)

`runtime/host_mono.c` implements the mono embedding API that a Unity 4 player calls,
so a netrecomp build can replace `<Game>_Data/Mono/mono.dll`. The player resolves its
mono functions by name with `GetProcAddress`; for Unity 4.2 and 4.3 that is exactly 180
`mono_*` functions plus `g_free`, and Unity 4.6 asks for 206 (including two named
`unity_mono_*`). It logs `mono: function X not found` for any that are missing. The lists
came from the strings in the player executables.

## Unity 4.5/4.6 additions

| Export | here |
|---|---|
| `mono_class_get_userdata_offset`, `_get_userdata`, `_set_userdata` | `NrClass.unity_userdata`; Unity reads the pointer at that offset directly |
| `mono_unity_liveness_allocate_struct`, `_stop_gc_world`, `_start_gc_world`, `_finalize`, `_free_struct` | the 4.2 liveness walk split into the steps 4.6 calls (`stop_gc_world` disables the GC, `finalize` flushes) |
| `mono_runtime_invoke_array` | unboxes value-type arguments from the `object[]` and calls `nr_invoke` |
| `mono_custom_attrs_construct`, `mono_reflection_get_custom_attrs_by_type` | attribute objects from the generated tables; `mono_custom_attrs_from_assembly` reports none |
| `mono_method_full_name`, `mono_field_get_parent`, `mono_class_get_type_token`, `mono_object_isinst`, `mono_array_element_size`, `mono_array_addr_with_size`, `mono_gc_wbarrier_generic_store` | direct |
| `unity_mono_method_is_generic`, `unity_mono_method_is_inflated` | always false: only compiled (closed) methods have descriptors |
| `mono_is_debugger_attached`, `mono_jit_info_table_find`, `mono_debug_lookup_source_location`, `mono_loader_get_last_error` | nothing to report |

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

## Scripted input (`NR_INPUT`)

With `NR_INPUT=<file>`, the host patches the player's imports of `GetCursorPos`,
`GetAsyncKeyState` and `GetKeyState` and reads commands from the file as it grows, one per
line, in client pixels: `move X Y`, `click X Y`, `rclick X Y`, `down`, `up`, `key VK`,
`wait MS`, `shot PATH`. The commands post the matching window messages and set the state those imports
report, so a game on a hidden or virtual monitor can be played by a script or by appending
lines by hand, and the real mouse is never moved. Unity 4 takes the pointer position from
`GetCursorPos`, which is why posting messages from outside the process doesn't work.
`runtime/unity_input.c`.

`shot PATH` makes the game save its next frame as a PNG: the next `mono_runtime_invoke` on
the main thread calls the compiled `UnityEngine.Application.CaptureScreenshot`. On a hidden
desktop (`CreateDesktop`) this is the only capture that works: a GDI grab of the window
returns whatever is on the real screen. D3D9 can't create a device there either, so run the
player with `-force-d3d11` (Unity 4.3+).

## Muting (`NR_MUTE`)

`NR_MUTE=1` mutes the process's own audio session (`ISimpleAudioVolume` on the default
endpoint, before Unity opens a stream), so test runs are silent without touching the system
volume.
