# Contributing to netrecomp

netrecomp is the .NET IL toolkit in the sp00nznet recomp family (pcrecomp, gmrecomp,
xboxrecomp, ps3recomp, ...). It shares their house style: no game data in the repo, and
generated code is never committed.

## Setup

Follow Getting Started in the [README](README.md). Before opening a PR, build the translator
and run the conformance tests against a game's `Managed` folder:

```
dotnet build -c Release tools\netrecomp
py -3 tools\conformance.py --corlib "<game>\<Game>_Data\Managed"
```

The pass count must not drop below `conformance/baseline.json`.

## What helps most

- **Try it on another Unity 4 or .NET 2.0-era game** and report where it stops: the lift
  summary, the first failing icall or exception with `NR_DEBUG=1`, and the Unity
  `output_log.txt`.
- **Conformance tests.** A small C# program in `conformance/tests/` that shows a runtime or
  lowering bug against the real CLR is the best bug report, and it stays as a regression test.
- **Fixes found on a title.** Describe the behaviour generically ("OrdinalIgnoreCase is
  ignored by EndsWith"), say which title hit it, and keep anything title-specific in the
  title's repo.

## Code style

- Translator: C# for .NET 8, Mono.Cecil. Runtime: C17, built with clang-cl for x86.
- 4-space indentation. Comments explain why, not what.
- Don't emit `__finally` or exception filters that write through out-parameters: clang-cl's
  x86 funclets miscompile them in stack-realigned functions (see `docs/recompiler.md`).

## Game data

Never commit game assemblies, generated C, decompiled source, game assets or saves. The
conformance harness needs a game's corlib and skips with a message when it has none.

## Where your code comes from

Contributions must be your own work, or come from a source whose licence is compatible with
MIT (MIT, BSD, zlib, Apache-2.0, public domain).

- **Don't port code from GPL or LGPL projects.** For this toolkit that means the old Mono
  runtime (the C runtime Unity 4 forked was LGPL), Wine, and GPL decompilers or recompilers.
  Rewriting line by line doesn't change that. Reading one to understand behaviour is fine;
  carrying its code or structure across is not.
- **MIT sources are fine with attribution:** the .NET runtime (dotnet/runtime), current Mono
  class libraries, Mono.Cecil. Note them in `NOTICE`.
- **If something came from elsewhere, say so in the PR**, with a link, and keep any copyright
  header.
- **AI-assisted contributions are welcome**, provided a human understood and verified the
  change. If generated code looks like it reproduces an existing project, check where it came
  from before submitting it.

## Where to ask

[The sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu).

## Licence

MIT. By submitting a contribution you agree that it is released under the same licence.
