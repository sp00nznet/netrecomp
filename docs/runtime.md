# The runtime

## Object model

mono's 32-bit layout, because Unity's engine reads it directly:

| Thing | Layout |
|---|---|
| object | `{ NrClass* klass; void* sync; fields... }` |
| string | header, `int32 length` at +8, UTF-16 chars at +12, NUL-terminated |
| array | header, `bounds*` at +8, `uint32 max_length` at +12, data at +16 |
| boxed value | header, value at +8 |

`NrClass` doubles as mono's `MonoClass` and `MonoVTable`: an object's header points at it,
and its first word points back at itself (mono code reads `vtable->klass`). Class records
are generated (`meta.c`); array classes the program never named are built at run time with
`System.Array`'s vtable.

Field offsets reported through reflection and the mono API include the 8-byte header for
value types too, as mono's did.

## Garbage collection

bdwgc, conservative, non-moving, with thread-local allocation and parallel marking. Roots are
the runtime's own data sections (statics, string literals, the GC handle table) plus thread
stacks; `GC_no_dls` keeps it from scanning every module's data, which would include the whole
engine. Objects without references are allocated atomic (never scanned). Finalizers run on a
dedicated thread. Threads that run managed code must be registered: threads the runtime starts
are (gc.h turns `CreateThread` into `GC_CreateThread`), and engine threads come in through
`mono_thread_attach`.

GC handles are indices into a scanned table; weak handles store a hidden pointer plus a
disappearing link, so the table doesn't keep their targets alive.

## Exceptions

A managed throw is `RaiseException(0xE04E5258, 0, 1, &object)`. Catch filters
(`nr_catch_any`) turn the record back into the object, and turn access violations,
integer divide-by-zero and overflow into `NullReferenceException`, `DivideByZeroException`
and `ArithmeticException`. A fault seen by several filters becomes one object (cached per
exception record). Because these are ordinary SEH exceptions they unwind through native
engine frames the way mono's did.

`NR_DEBUG=1` prints every throw and fault with a symbolized native stack, the words around
the faulting ESP (module offsets and GC object classes), and what memory an address outside
every module lies in. `nr_debug_watch(addr)` arms a hardware write watchpoint on the calling
thread and logs each writer; it is for temporary probes in generated code.

## Internal calls

Generated code calls an icall through a wrapper that resolves it once by mono's key,
`"Namespace.Type::Method(sig)"` and then without the signature. The runtime registers its
corlib icalls at start-up; Unity registers its own through `mono_add_internal_call`. A
missing icall logs its name and resolves to a stub that throws `MissingMethodException`,
so one gap fails one call, not the build.

Choices that differ from mono, each marked `ponytail:` in the source:

- `Encoding.Default` is UTF-8 without a BOM. mono reported the ANSI code page, which loads
  the I18N assemblies by reflection.
- Culture data: only the invariant culture. Comparison is ordinal or `CompareStringW`
  (invariant locale); sort keys are ordinal (UTF-16 big endian).
- Custom attributes are rebuilt on every query by generated factories. Properties carry
  none yet, and attribute types in compiler/debugger/interop/security namespaces are dropped.
- `MakeGenericType` works for instantiations that were compiled, and throws
  `NotSupportedException` otherwise.
- One thread per `BeginInvoke`; no `IAsyncResult`. A socket's async call
  (`Socket/SocketAsyncCall`) does its recv/send in the runtime first, as mono's IO pool did.
- `Decimal` arithmetic and parsing use OLE Automation's `VarDec*` (oleaut32).

## Sockets and DNS

`nr_net.c`: `System.Net.Sockets.Socket` (TCP and UDP, blocking and not, bind/listen/accept,
`Poll`, `Select`, options, endpoints) and `System.Net.Dns` on WinSock. Errors come back as WSA
codes, which are `SocketError`'s values. `NR_NET_ALLOW=127.0.0.1[,...]` makes connect and
sendto refuse every other address (automated runs use it so nothing leaves the machine).
Reverse lookups answer with the numeric address. `conformance/tests/sockets.cs` covers it on
loopback. Unity's own `WWW` is native and doesn't go through these.

## Reflection

Answered from the class records: types, fields, methods, constructors, properties
(synthesized from `get_`/`set_` methods), enums (from literal fields), assemblies
(one record per image) and custom attributes. Invoking a method nothing compiled throws
`MissingMethodException`, with two shared-code fallbacks:

- **Generic methods.** Every generic method definition has a descriptor; its compiled
  instances carry their type arguments. `MakeGenericMethod` returns the exact instance, or
  the closest compiled one the reference-type arguments are assignable to. The translator
  adds a canonical instance (`object`, or the class constraint, per parameter) for each
  generic method a body looks up by name next to a `MakeGenericMethod` call
  (`typeof(X).GetMethod("Name")`); abstract and interface ones go through the generic
  virtual table, and `Invoke` on them dispatches to the receiver's instance. The ceiling:
  a shared body sees `typeof(T)` as its compiled argument.
- **Generic classes.** A public method of `C<Derived>` that only reflection calls runs
  `C<object>`'s body (same layout for reference types); the translator compiles the public
  methods of each such canonical instance. Interface dispatch accepts covariant
  reference-type instances (`Derived[]` as `IEnumerable<Base>`). Parameter names aren't recorded (`arg0`, `arg1`...).

The Char and NumberFormatter tables mono's runtime supplied are generated by the translator
(`chartab.c`, `fmttab.c`) from Unicode data and from arithmetic, not taken from mono.
