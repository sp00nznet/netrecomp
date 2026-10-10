// Lowers one concrete method's IL to a C function.
//
// Every eval-stack slot becomes a C variable named by depth and C type
// (s2i, s0p, s1v17...), so values never move at branches: IL guarantees the
// same stack shape on every path into a join. Exception regions become MSVC
// structured exception handling (__try/__except/__finally): `leave` is a goto
// out of the __try, which runs the __finally blocks the way the CLI does.
// See docs/recompiler.md.
using System.Globalization;
using System.Text;
using Mono.Cecil;
using Mono.Cecil.Cil;

readonly record struct SV(SK K, TT? T);

sealed class EmitError(string msg) : Exception(msg);

sealed class MethodEmitter
{
    readonly Gen g; readonly World w; readonly Layouts lay; readonly MI m;
    readonly MethodBody body;
    readonly bool hasThis;
    readonly TT[] argT; readonly TT[] locT;
    readonly Dictionary<int, SV[]> stackIn = new();
    readonly Dictionary<string, string> vars = new();      // stack var name -> C type
    readonly HashSet<int> labels = new();
    StringBuilder o = new();
    SV[] st = Array.Empty<SV>();
    int sp;
    bool emitting;
    int tmpN;
    readonly List<Block> blocks = new();
    readonly Stack<string> excVar = new();
    readonly Dictionary<ExceptionHandler, string> finEnd = new();

    sealed class Block { public int TryStart, TryEnd; public List<ExceptionHandler> H = new(); public int End => H.Max(h => h.HandlerEnd?.Offset ?? int.MaxValue); }

    public MethodEmitter(Gen g, MI m)
    {
        this.g = g; w = g.W; lay = g.Lay; this.m = m;
        body = m.Def.Body;
        hasThis = m.Def.HasThis;
        var at = new List<TT>();
        if (hasThis) at.Add(m.Owner.IsValueType ? w.ByRef(m.Owner) : m.Owner);
        for (int i = 0; i < m.Def.Parameters.Count; i++) at.Add(w.ParamType(m, i));
        argT = at.ToArray();
        locT = body.Variables.Select(v => w.Conv(v.VariableType, m.Owner.Args, m.MArgs)).ToArray();
    }

    // ---- stack ----
    static string Suffix(SV v, Layouts lay) => v.K switch
    {
        SK.I4 => "i", SK.I => "i", SK.I8 => "l", SK.R4 => "f", SK.R8 => "d", SK.P => "p",
        _ => "v" + lay.Id(v.T!),
    };
    string CTypeOf(SV v) => v.K switch
    {
        SK.I4 or SK.I => "int32_t", SK.I8 => "int64_t", SK.R4 => "float", SK.R8 => "double", SK.P => "void*",
        _ => lay.C(v.T!),
    };
    string Var(int depth, SV v)
    {
        var n = "s" + depth + Suffix(v, lay);
        vars[n] = CTypeOf(v);
        return n;
    }
    string Push(SV v, string expr)
    {
        if (sp == st.Length) Array.Resize(ref st, sp + 4);
        st[sp] = v;
        var n = Var(sp, v);
        sp++;
        L($"{n} = {expr};");
        return n;
    }
    (SV v, string n) Pop()
    {
        if (sp == 0) throw new EmitError("stack underflow");
        sp--;
        return (st[sp], Var(sp, st[sp]));
    }
    SV Kind(TT t) => new(lay.Kind(t), t);
    void L(string s) { if (emitting) o.Append("    ").Append(s).Append('\n'); }

    // Converts a stack value to a C value of type t (store, argument, return).
    string As(TT t, (SV v, string n) x)
    {
        var c = lay.C(t);
        var k = lay.Kind(t);
        if (k == SK.VT) return x.n;
        if (k == SK.P) return x.v.K == SK.P ? x.n : $"(void*)(intptr_t){x.n}";
        if (x.v.K == SK.P) return $"({c})(intptr_t){x.n}";
        return $"({c}){x.n}";
    }

    // ---- driver ----
    public string Emit()
    {
        foreach (var h in body.ExceptionHandlers)
        {
            if (h.HandlerType == ExceptionHandlerType.Filter) throw new EmitError("filter block");
            var b = blocks.FirstOrDefault(x => x.TryStart == h.TryStart.Offset && x.TryEnd == h.TryEnd.Offset);
            if (b == null) blocks.Add(b = new Block { TryStart = h.TryStart.Offset, TryEnd = h.TryEnd.Offset });
            b.H.Add(h);
        }
        Analyse();
        emitting = true;
        o = new StringBuilder();
        var instrs = body.Instructions;
        EmitRange(0, instrs.Count > 0 ? instrs[^1].Offset + instrs[^1].GetSize() : 0, new HashSet<Block>());

        var sb = new StringBuilder();
        sb.Append(g.Proto(m)).Append("\n{\n");
        for (int i = 0; i < locT.Length; i++)
        {
            var c = lay.C(locT[i]);
            sb.Append($"    {c} l{i}").Append(lay.Kind(locT[i]) == SK.VT ? " = {0};\n" : " = 0;\n");
        }
        foreach (var kv in vars.OrderBy(k => k.Key)) sb.Append($"    {kv.Value} {kv.Key};\n");
        foreach (var b in blocks) foreach (var h in b.H) if (h.HandlerType == ExceptionHandlerType.Catch || h.HandlerType == ExceptionHandlerType.Fault) sb.Append($"    void* ex{h.HandlerStart.Offset} = 0;\n");
        if (g.CctorCheckOnEntry(m)) sb.Append($"    NR_CCTOR({g.Cctor(m.Owner)});\n");
        sb.Append(o);
        sb.Append("}\n");
        return sb.ToString();
    }

    void Analyse()
    {
        var instrs = body.Instructions;
        var work = new Stack<(Instruction, SV[])>();
        if (instrs.Count > 0) work.Push((instrs[0], Array.Empty<SV>()));
        foreach (var h in body.ExceptionHandlers)
            work.Push((h.HandlerStart, h.HandlerType == ExceptionHandlerType.Catch ? new[] { new SV(SK.P, w.Cor("System", "Exception")) } : Array.Empty<SV>()));
        while (work.Count > 0)
        {
            var (ins, s) = work.Pop();
            if (stackIn.TryGetValue(ins.Offset, out var prev))
            {
                if (prev.Length != s.Length) throw new EmitError($"stack depth mismatch at IL_{ins.Offset:x4}");
                bool changed = false;
                for (int i = 0; i < s.Length; i++)
                {
                    var m = Merge(prev[i], s[i], ins.Offset);
                    if (m != prev[i]) { prev[i] = m; changed = true; }
                }
                if (!changed) continue;
                s = prev;
            }
            stackIn[ins.Offset] = s;
            st = (SV[])s.Clone(); sp = s.Length;
            var next = Step(ins);
            foreach (var t in next) work.Push((t, st.Take(sp).ToArray()));
        }
    }

    void EmitRange(int start, int end, HashSet<Block> open)
    {
        var instrs = body.Instructions;
        int i = instrs.TakeWhile(x => x.Offset < start).Count();
        while (i < instrs.Count && instrs[i].Offset < end)
        {
            var ins = instrs[i];
            var b = blocks.Where(x => x.TryStart == ins.Offset && !open.Contains(x)).OrderByDescending(x => x.TryEnd).FirstOrDefault();
            if (b != null)
            {
                // Labels on the try's first instruction go outside every __try
                // that starts there: C can't jump into a __try.
                if (labels.Contains(ins.Offset) && !open.Any(x => x.TryStart == ins.Offset)) o.Append($"L{ins.Offset:x4}:;\n");
                EmitBlock(b, open);
                int e = b.End;
                while (i < instrs.Count && instrs[i].Offset < e) i++;
                continue;
            }
            if (stackIn.ContainsKey(ins.Offset))
            {
                if (labels.Contains(ins.Offset) && !open.Any(x => x.TryStart == ins.Offset)) o.Append($"L{ins.Offset:x4}:;\n");
                st = (SV[])stackIn[ins.Offset].Clone(); sp = st.Length;
                var succ = Step(ins);
                if (ins.Next != null && succ.Contains(ins.Next)) EdgeMoves(ins.Next);
            }
            i++;
        }
    }

    void EmitBlock(Block b, HashSet<Block> open)
    {
        var inner = new HashSet<Block>(open) { b };
        // Handlers of one try must follow it back to back.
        int at = b.TryEnd;
        foreach (var h in b.H.OrderBy(h => h.HandlerStart.Offset))
        {
            if (h.HandlerStart.Offset != at) throw new EmitError("non-contiguous handler");
            at = h.HandlerEnd?.Offset ?? int.MaxValue;
        }
        var fin = b.H.Where(h => h.HandlerType is ExceptionHandlerType.Finally or ExceptionHandlerType.Fault).ToList();
        var cat = b.H.Where(h => h.HandlerType == ExceptionHandlerType.Catch).ToList();
        if (fin.Count > 0 && cat.Count > 0) throw new EmitError("catch and finally on one try");
        if (fin.Count > 1) throw new EmitError("two finally handlers on one try");
        o.Append("__try {\n");
        EmitRange(b.TryStart, b.TryEnd, inner);
        o.Append("}\n");
        if (fin.Count == 1 && fin[0].HandlerType == ExceptionHandlerType.Finally)
        {
            var h = fin[0];
            finEnd[h] = $"F{h.HandlerStart.Offset:x4}";
            o.Append("__finally {\n");
            EmitRange(h.HandlerStart.Offset, h.HandlerEnd.Offset, inner);
            o.Append($"{finEnd[h]}:;\n}}\n");
        }
        else if (fin.Count == 1)
        {
            // fault: runs only on exception, then the exception continues.
            var h = fin[0];
            var ev = $"ex{h.HandlerStart.Offset}";
            o.Append($"__except (nr_catch(GetExceptionInformation(), 0, &{ev})) {{\n");
            finEnd[h] = $"F{h.HandlerStart.Offset:x4}";
            excVar.Push(ev);
            EmitRange(h.HandlerStart.Offset, h.HandlerEnd.Offset, inner);
            excVar.Pop();
            o.Append($"{finEnd[h]}:;\n    nr_throw({ev});\n}}\n");
        }
        else
        {
            // One __except for all catch clauses: the filter records which matched.
            var ev = $"ex{cat[0].HandlerStart.Offset}";
            var which = $"w{cat[0].HandlerStart.Offset}";
            vars[which] = "int32_t";
            var ks = string.Join(", ", cat.Select(h => "&" + g.Klass(w.Conv(h.CatchType, m.Owner.Args, m.MArgs))));
            o.Append($"__except (nr_catch_any(GetExceptionInformation(), (NrClass*[]){{ {ks} }}, {cat.Count}, &{ev}, &{which})) {{\n");
            for (int k = 0; k < cat.Count; k++)
            {
                var h = cat[k];
                var hv = $"ex{h.HandlerStart.Offset}";
                o.Append(k == 0 ? $"if ({which} == 0) {{\n" : $"}} else if ({which} == {k}) {{\n    {hv} = {ev};\n");
                excVar.Push(hv);
                // The handler's IL starts with the exception on the stack.
                st = new[] { new SV(SK.P, null) }; sp = 1;
                o.Append($"    s0p = {hv};\n"); vars["s0p"] = "void*";
                EmitRange(h.HandlerStart.Offset, h.HandlerEnd.Offset, inner);
                excVar.Pop();
            }
            o.Append("}\n}\n");
        }
    }

    // ---- one instruction ----
    // Returns the successors (analysis); in emit mode also writes C.
    IEnumerable<Instruction> Step(Instruction ins)
    {
        var op = ins.OpCode;
        var nx = ins.Next;
        var code = op.Code;
        switch (code)
        {
            case Code.Nop: case Code.Break: case Code.Volatile: case Code.Tail: case Code.Readonly: case Code.Unaligned:
            case Code.No: break;
            case Code.Constrained: break;   // consumed by the following callvirt

            case Code.Ldarg_0: case Code.Ldarg_1: case Code.Ldarg_2: case Code.Ldarg_3:
                LdArg(code - Code.Ldarg_0); break;
            case Code.Ldarg_S: case Code.Ldarg: LdArg(ArgNo(ins)); break;
            case Code.Ldarga_S: case Code.Ldarga: { var a = ArgNo(ins); Push(new SV(SK.P, w.ByRef(argT[a])), $"&a{a}"); break; }
            case Code.Starg_S: case Code.Starg: { var a = ArgNo(ins); var v = Pop(); L($"a{a} = {As(argT[a], v)};"); break; }
            case Code.Ldloc_0: case Code.Ldloc_1: case Code.Ldloc_2: case Code.Ldloc_3: LdLoc(code - Code.Ldloc_0); break;
            case Code.Ldloc_S: case Code.Ldloc: LdLoc(((VariableDefinition)ins.Operand).Index); break;
            case Code.Ldloca_S: case Code.Ldloca: { var i = ((VariableDefinition)ins.Operand).Index; Push(new SV(SK.P, w.ByRef(locT[i])), $"&l{i}"); break; }
            case Code.Stloc_0: case Code.Stloc_1: case Code.Stloc_2: case Code.Stloc_3: StLoc(code - Code.Stloc_0); break;
            case Code.Stloc_S: case Code.Stloc: StLoc(((VariableDefinition)ins.Operand).Index); break;

            case Code.Ldnull: Push(new SV(SK.P, null), "0"); break;
            case Code.Ldc_I4_M1: case Code.Ldc_I4_0: case Code.Ldc_I4_1: case Code.Ldc_I4_2: case Code.Ldc_I4_3:
            case Code.Ldc_I4_4: case Code.Ldc_I4_5: case Code.Ldc_I4_6: case Code.Ldc_I4_7: case Code.Ldc_I4_8:
                Push(new SV(SK.I4, null), (code - Code.Ldc_I4_0).ToString()); break;
            case Code.Ldc_I4_S: Push(new SV(SK.I4, null), I4((sbyte)ins.Operand)); break;
            case Code.Ldc_I4: Push(new SV(SK.I4, null), I4((int)ins.Operand)); break;
            case Code.Ldc_I8: Push(new SV(SK.I8, null), $"(int64_t)0x{(ulong)(long)ins.Operand:x}ull"); break;
            case Code.Ldc_R4: Push(new SV(SK.R4, null), R4((float)ins.Operand)); break;
            case Code.Ldc_R8: Push(new SV(SK.R8, null), R8((double)ins.Operand)); break;
            case Code.Ldstr: Push(new SV(SK.P, w.Cor("System", "String")), g.Str((string)ins.Operand)); break;

            case Code.Dup: { var v = Pop(); Push(v.v, v.n); Push(v.v, v.n); break; }
            case Code.Pop: Pop(); break;

            case Code.Add: case Code.Sub: case Code.Mul: case Code.Div: case Code.Div_Un: case Code.Rem: case Code.Rem_Un:
            case Code.And: case Code.Or: case Code.Xor:
            case Code.Add_Ovf: case Code.Add_Ovf_Un: case Code.Sub_Ovf: case Code.Sub_Ovf_Un: case Code.Mul_Ovf: case Code.Mul_Ovf_Un:
                Binary(code); break;
            case Code.Shl: case Code.Shr: case Code.Shr_Un: Shift(code); break;
            case Code.Neg:
            {
                var a = Pop();
                Push(a.v, a.v.K switch { SK.I4 or SK.I => $"(int32_t)(0u - (uint32_t){a.n})", SK.I8 => $"(int64_t)(0ull - (uint64_t){a.n})", _ => $"-{a.n}" });
                break;
            }
            case Code.Not: { var a = Pop(); Push(a.v, $"~{a.n}"); break; }

            case Code.Ceq: case Code.Cgt: case Code.Cgt_Un: case Code.Clt: case Code.Clt_Un:
            {
                var b = Pop(); var a = Pop();
                Push(new SV(SK.I4, null), Compare(code, a, b));
                break;
            }

            case Code.Br: case Code.Br_S: case Code.Leave: case Code.Leave_S:
            {
                var t = (Instruction)ins.Operand;
                if (code is Code.Leave or Code.Leave_S) sp = 0;
                Goto(t);
                return new[] { t };
            }
            case Code.Brtrue: case Code.Brtrue_S: case Code.Brfalse: case Code.Brfalse_S:
            {
                var a = Pop(); var t = (Instruction)ins.Operand;
                var neg = code is Code.Brfalse or Code.Brfalse_S;
                EdgeMoves(t);
                L($"if ({(neg ? "!" : "")}{a.n}) goto L{t.Offset:x4};"); labels.Add(t.Offset);
                return new[] { t, nx };
            }
            case Code.Beq: case Code.Beq_S: case Code.Bne_Un: case Code.Bne_Un_S:
            case Code.Bge: case Code.Bge_S: case Code.Bge_Un: case Code.Bge_Un_S:
            case Code.Bgt: case Code.Bgt_S: case Code.Bgt_Un: case Code.Bgt_Un_S:
            case Code.Ble: case Code.Ble_S: case Code.Ble_Un: case Code.Ble_Un_S:
            case Code.Blt: case Code.Blt_S: case Code.Blt_Un: case Code.Blt_Un_S:
            {
                var b = Pop(); var a = Pop(); var t = (Instruction)ins.Operand;
                EdgeMoves(t);
                L($"if ({BranchCond(code, a, b)}) goto L{t.Offset:x4};"); labels.Add(t.Offset);
                return new[] { t, nx };
            }
            case Code.Switch:
            {
                var a = Pop(); var ts = (Instruction[])ins.Operand;
                foreach (var t in ts) { EdgeMoves(t); labels.Add(t.Offset); }
                if (emitting)
                {
                    var sb = new StringBuilder($"switch ((uint32_t){a.n}) {{");
                    for (int i = 0; i < ts.Length; i++) { sb.Append($" case {i}: goto L{ts[i].Offset:x4};"); labels.Add(ts[i].Offset); }
                    L(sb.Append(" }").ToString());
                }
                return ts.Append(nx).ToArray();
            }
            case Code.Ret:
            {
                var rt = w.RetType(m);
                if (Layouts.IsVoid(rt)) L("return;"); else { var v = Pop(); L($"return {As(rt, v)};"); }
                return Array.Empty<Instruction>();
            }
            case Code.Throw: { var v = Pop(); L($"nr_throw({v.n});"); return Array.Empty<Instruction>(); }
            case Code.Rethrow:
                L($"nr_throw({(excVar.Count > 0 ? excVar.Peek() : "0")});");
                return Array.Empty<Instruction>();
            case Code.Endfinally:
            {
                var h = body.ExceptionHandlers.Where(h => h.HandlerStart.Offset <= ins.Offset && ins.Offset < (h.HandlerEnd?.Offset ?? int.MaxValue))
                    .OrderBy(h => (h.HandlerEnd?.Offset ?? int.MaxValue) - h.HandlerStart.Offset).First();
                bool last = ins.Next == null || ins.Next.Offset == (h.HandlerEnd?.Offset ?? -1);
                if (!last && emitting) L($"goto F{h.HandlerStart.Offset:x4};");
                return Array.Empty<Instruction>();
            }

            case Code.Conv_I1: case Code.Conv_I2: case Code.Conv_I4: case Code.Conv_U1: case Code.Conv_U2: case Code.Conv_U4:
            case Code.Conv_I8: case Code.Conv_U8: case Code.Conv_I: case Code.Conv_U:
            case Code.Conv_R4: case Code.Conv_R8: case Code.Conv_R_Un:
                Conv(code); break;
            case Code.Conv_Ovf_I1: case Code.Conv_Ovf_I2: case Code.Conv_Ovf_I4: case Code.Conv_Ovf_I8:
            case Code.Conv_Ovf_U1: case Code.Conv_Ovf_U2: case Code.Conv_Ovf_U4: case Code.Conv_Ovf_U8:
            case Code.Conv_Ovf_I: case Code.Conv_Ovf_U:
            case Code.Conv_Ovf_I1_Un: case Code.Conv_Ovf_I2_Un: case Code.Conv_Ovf_I4_Un: case Code.Conv_Ovf_I8_Un:
            case Code.Conv_Ovf_U1_Un: case Code.Conv_Ovf_U2_Un: case Code.Conv_Ovf_U4_Un: case Code.Conv_Ovf_U8_Un:
            case Code.Conv_Ovf_I_Un: case Code.Conv_Ovf_U_Un:
                ConvOvf(op.Name); break;
            case Code.Ckfinite: { var a = Pop(); Push(a.v, $"nr_ckfinite({a.n})"); break; }

            case Code.Ldind_I1: LdInd("int8_t", SK.I4); break;
            case Code.Ldind_U1: LdInd("uint8_t", SK.I4); break;
            case Code.Ldind_I2: LdInd("int16_t", SK.I4); break;
            case Code.Ldind_U2: LdInd("uint16_t", SK.I4); break;
            case Code.Ldind_I4: LdInd("int32_t", SK.I4); break;
            case Code.Ldind_U4: LdInd("uint32_t", SK.I4); break;
            case Code.Ldind_I8: LdInd("int64_t", SK.I8); break;
            case Code.Ldind_I: LdInd("intptr_t", SK.I); break;
            case Code.Ldind_R4: LdInd("float", SK.R4); break;
            case Code.Ldind_R8: LdInd("double", SK.R8); break;
            case Code.Ldind_Ref: LdInd("void*", SK.P); break;
            case Code.Stind_I1: StInd("int8_t"); break;
            case Code.Stind_I2: StInd("int16_t"); break;
            case Code.Stind_I4: StInd("int32_t"); break;
            case Code.Stind_I8: StInd("int64_t"); break;
            case Code.Stind_I: StInd("intptr_t"); break;
            case Code.Stind_R4: StInd("float"); break;
            case Code.Stind_R8: StInd("double"); break;
            case Code.Stind_Ref: StInd("void*"); break;
            case Code.Ldobj: { var t = Ty(ins); var p = Pop(); Push(Kind(t), $"*({lay.C(t)}*){p.n}"); break; }
            case Code.Stobj: { var t = Ty(ins); var v = Pop(); var p = Pop(); L($"*({lay.C(t)}*){p.n} = {As(t, v)};"); break; }
            case Code.Cpobj: { var t = Ty(ins); var s = Pop(); var d = Pop(); L($"*({lay.C(t)}*){d.n} = *({lay.C(t)}*){s.n};"); break; }
            case Code.Initobj: { var t = Ty(ins); var p = Pop(); L($"memset((void*){p.n}, 0, sizeof({lay.C(t)}));"); break; }
            case Code.Sizeof: { var t = Ty(ins); Push(new SV(SK.I4, null), $"(int32_t)sizeof({lay.C(t)})"); break; }
            case Code.Cpblk: { var n = Pop(); var s = Pop(); var d = Pop(); L($"memmove((void*){d.n}, (void*){s.n}, (uint32_t){n.n});"); break; }
            case Code.Initblk: { var n = Pop(); var v = Pop(); var d = Pop(); L($"memset((void*){d.n}, {v.n}, (uint32_t){n.n});"); break; }
            case Code.Localloc: { var n = Pop(); Push(new SV(SK.I, null), $"(int32_t)(intptr_t)nr_zalloca(_alloca((uint32_t){n.n}), (uint32_t){n.n})"); break; }

            case Code.Ldfld: case Code.Ldflda: case Code.Stfld: Field(code, (FieldReference)ins.Operand); break;
            case Code.Ldsfld: case Code.Ldsflda: case Code.Stsfld: StaticField(code, (FieldReference)ins.Operand); break;

            case Code.Call: case Code.Callvirt:
                Call(ins, code == Code.Callvirt, ins.Previous?.OpCode.Code == Code.Constrained ? Ty(ins.Previous) : null); break;
            case Code.Newobj: NewObj(ins); break;
            case Code.Ldftn:
            {
                var t = Meth(ins);
                Push(new SV(SK.I, null), $"(int32_t)(intptr_t){g.FnPtr(t)}");
                break;
            }
            case Code.Ldvirtftn:
            {
                var t = Meth(ins); var obj = Pop();
                Push(new SV(SK.I, null), $"(int32_t)(intptr_t){VirtualFn(t, obj.n)}");
                break;
            }

            case Code.Castclass: { var t = Ty(ins); var a = Pop(); Push(new SV(SK.P, t), $"nr_castclass({a.n}, &{g.Klass(t)})"); break; }
            case Code.Isinst: { var t = Ty(ins); var a = Pop(); Push(new SV(SK.P, t), $"nr_isinst({a.n}, &{g.Klass(t)})"); break; }
            case Code.Box: Box(Ty(ins)); break;
            case Code.Unbox: { var t = Ty(ins); var a = Pop(); Push(new SV(SK.P, w.ByRef(t)), $"nr_unbox({a.n}, &{g.Klass(t)})"); break; }
            case Code.Unbox_Any: UnboxAny(Ty(ins)); break;

            case Code.Newarr:
            {
                var t = Ty(ins); var n = Pop();
                var at = w.SzArray(t);
                g.Construct(at);
                Push(new SV(SK.P, at), $"nr_newarr(&{g.Klass(at)}, {n.n})");
                break;
            }
            case Code.Ldlen: { var a = Pop(); Push(new SV(SK.I, null), $"(int32_t)((NrArray*){a.n})->max_length"); break; }
            case Code.Ldelem_I1: LdElem("int8_t", SK.I4); break;
            case Code.Ldelem_U1: LdElem("uint8_t", SK.I4); break;
            case Code.Ldelem_I2: LdElem("int16_t", SK.I4); break;
            case Code.Ldelem_U2: LdElem("uint16_t", SK.I4); break;
            case Code.Ldelem_I4: LdElem("int32_t", SK.I4); break;
            case Code.Ldelem_U4: LdElem("uint32_t", SK.I4); break;
            case Code.Ldelem_I8: LdElem("int64_t", SK.I8); break;
            case Code.Ldelem_I: LdElem("intptr_t", SK.I); break;
            case Code.Ldelem_R4: LdElem("float", SK.R4); break;
            case Code.Ldelem_R8: LdElem("double", SK.R8); break;
            case Code.Ldelem_Ref: LdElem("void*", SK.P); break;
            case Code.Ldelem_Any: { var t = Ty(ins); LdElem(lay.C(t), lay.Kind(t), t); break; }
            case Code.Ldelema:
            {
                var t = Ty(ins); var i = Pop(); var a = Pop();
                Push(new SV(SK.P, w.ByRef(t)), $"&(({lay.C(t)}*)NR_ADATA({a.n}))[NR_AIDX({a.n}, {i.n})]");
                break;
            }
            case Code.Stelem_I1: StElem("int8_t"); break;
            case Code.Stelem_I2: StElem("int16_t"); break;
            case Code.Stelem_I4: StElem("int32_t"); break;
            case Code.Stelem_I8: StElem("int64_t"); break;
            case Code.Stelem_I: StElem("intptr_t"); break;
            case Code.Stelem_R4: StElem("float"); break;
            case Code.Stelem_R8: StElem("double"); break;
            case Code.Stelem_Ref: StElem("void*"); break;
            case Code.Stelem_Any: { var t = Ty(ins); StElem(lay.C(t), t); break; }

            case Code.Ldtoken: Ldtoken(ins.Operand); break;

            default: throw new EmitError("unsupported opcode " + op.Name);
        }
        return nx == null ? Array.Empty<Instruction>() : new[] { nx };
    }

    // ---- helpers ----
    int ArgNo(Instruction ins)
    {
        var p = (ParameterDefinition)ins.Operand;
        return p == body.ThisParameter ? 0 : p.Index + (hasThis ? 1 : 0);
    }
    void LdArg(int a) => Push(Kind(argT[a]), $"a{a}");
    void LdLoc(int i) => Push(Kind(locT[i]), $"l{i}");
    void StLoc(int i) { var v = Pop(); L($"l{i} = {As(locT[i], v)};"); }
    void Goto(Instruction t) { EdgeMoves(t); L($"goto L{t.Offset:x4};"); labels.Add(t.Offset); }

    // Joins where paths push different kinds of the same width (a fixed
    // statement's native int vs managed pointer) settle on native int; each
    // edge into such a join converts into the join's variables.
    SV Merge(SV a, SV b, int at)
    {
        if (Suffix(a, lay) == Suffix(b, lay)) return a.K == SK.I4 && b.K == SK.I ? b : a;
        if (a.K is SK.I4 or SK.I or SK.P && b.K is SK.I4 or SK.I or SK.P) return new SV(SK.I, null);
        throw new EmitError($"stack type mismatch at IL_{at:x4}: {a.K} vs {b.K}");
    }

    void EdgeMoves(Instruction t)
    {
        if (!emitting || !stackIn.TryGetValue(t.Offset, out var want)) return;
        for (int i = 0; i < sp && i < want.Length; i++)
            if (Suffix(st[i], lay) != Suffix(want[i], lay))
                L($"{Var(i, want[i])} = ({CTypeOf(want[i])})(intptr_t){Var(i, st[i])};");
    }
    TT Ty(Instruction ins) => w.Conv((TypeReference)ins.Operand, m.Owner.Args, m.MArgs);
    MI Meth(Instruction ins) => w.ConvMethod((MethodReference)ins.Operand, m.Owner.Args, m.MArgs) ?? throw new EmitError("array method as ldftn");

    static string I4(int v) => v == int.MinValue ? "(int32_t)0x80000000u" : v.ToString(CultureInfo.InvariantCulture);
    static string R4(float f)
    {
        if (float.IsNaN(f) || float.IsInfinity(f) || (f == 0 && float.IsNegative(f))) return $"nr_f32(0x{BitConverter.SingleToUInt32Bits(f):x8}u)";
        var s = f.ToString("R", CultureInfo.InvariantCulture);
        if (!s.Contains('.') && !s.Contains('E')) s += ".0";
        return s + "f";
    }
    static string R8(double d)
    {
        if (double.IsNaN(d) || double.IsInfinity(d) || (d == 0 && double.IsNegative(d))) return $"nr_f64(0x{BitConverter.DoubleToUInt64Bits(d):x16}ull)";
        var s = d.ToString("R", CultureInfo.InvariantCulture);
        if (!s.Contains('.') && !s.Contains('E')) s += ".0";
        return s;
    }

    static bool IsInt(SK k) => k is SK.I4 or SK.I or SK.I8 or SK.P;
    static bool IsF(SK k) => k is SK.R4 or SK.R8;

    void Binary(Code code)
    {
        var b = Pop(); var a = Pop();
        SK rk;
        if (IsF(a.v.K) || IsF(b.v.K)) rk = (a.v.K == SK.R8 || b.v.K == SK.R8) ? SK.R8 : SK.R4;
        else if (a.v.K == SK.I8 || b.v.K == SK.I8) rk = SK.I8;
        else if (a.v.K == SK.P && b.v.K == SK.P) rk = code == Code.Sub ? SK.I : SK.P;
        else if (a.v.K == SK.P || b.v.K == SK.P) rk = SK.P;
        else if (a.v.K == SK.I || b.v.K == SK.I) rk = SK.I;
        else rk = SK.I4;
        string x, y;
        if (rk == SK.P || (rk == SK.I && (a.v.K == SK.P || b.v.K == SK.P))) { x = $"(intptr_t){a.n}"; y = $"(intptr_t){b.n}"; }
        else { x = a.n; y = b.n; }
        string e;
        if (IsF(rk))
        {
            var c = rk == SK.R8 ? "double" : "float";
            e = code switch
            {
                Code.Add => $"({c}){x} + ({c}){y}", Code.Sub => $"({c}){x} - ({c}){y}", Code.Mul => $"({c}){x} * ({c}){y}",
                Code.Div => $"({c}){x} / ({c}){y}", Code.Rem => rk == SK.R8 ? $"fmod({x}, {y})" : $"fmodf({x}, {y})",
                _ => throw new EmitError("float " + code),
            };
        }
        else
        {
            var w8 = rk == SK.I8;
            var s = w8 ? "int64_t" : "int32_t"; var u = w8 ? "uint64_t" : "uint32_t"; var sfx = w8 ? "i8" : "i4";
            e = code switch
            {
                Code.Add => $"({s})(({u}){x} + ({u}){y})",
                Code.Sub => $"({s})(({u}){x} - ({u}){y})",
                Code.Mul => $"({s})(({u}){x} * ({u}){y})",
                Code.Div => $"nr_div_{sfx}({x}, {y})", Code.Div_Un => $"({s})nr_divu_{sfx}({x}, {y})",
                Code.Rem => $"nr_rem_{sfx}({x}, {y})", Code.Rem_Un => $"({s})nr_remu_{sfx}({x}, {y})",
                Code.And => $"{x} & {y}", Code.Or => $"{x} | {y}", Code.Xor => $"{x} ^ {y}",
                Code.Add_Ovf => $"nr_add_ovf_{sfx}({x}, {y})", Code.Add_Ovf_Un => $"nr_addu_ovf_{sfx}({x}, {y})",
                Code.Sub_Ovf => $"nr_sub_ovf_{sfx}({x}, {y})", Code.Sub_Ovf_Un => $"nr_subu_ovf_{sfx}({x}, {y})",
                Code.Mul_Ovf => $"nr_mul_ovf_{sfx}({x}, {y})", Code.Mul_Ovf_Un => $"nr_mulu_ovf_{sfx}({x}, {y})",
                _ => throw new EmitError("int " + code),
            };
            if (rk == SK.P) e = $"(void*)({e})";
        }
        Push(new SV(rk, null), e);
    }

    void Shift(Code code)
    {
        var b = Pop(); var a = Pop();
        var w8 = a.v.K == SK.I8;
        var mask = w8 ? 63 : 31;
        var s = w8 ? "int64_t" : "int32_t"; var u = w8 ? "uint64_t" : "uint32_t";
        var e = code switch
        {
            Code.Shl => $"({s})(({u}){a.n} << ({b.n} & {mask}))",
            Code.Shr => $"({s}){a.n} >> ({b.n} & {mask})",
            _ => $"({s})(({u}){a.n} >> ({b.n} & {mask}))",
        };
        Push(new SV(a.v.K == SK.P ? SK.I : a.v.K, null), e);
    }

    (string x, string y) Operands((SV v, string n) a, (SV v, string n) b, bool un)
    {
        if (IsF(a.v.K) || IsF(b.v.K)) return (a.n, b.n);
        if (a.v.K == SK.I8 || b.v.K == SK.I8)
            return un ? ($"(uint64_t){a.n}", $"(uint64_t){b.n}") : ($"(int64_t){a.n}", $"(int64_t){b.n}");
        if (a.v.K == SK.P || b.v.K == SK.P || un) return ($"(uintptr_t){a.n}", $"(uintptr_t){b.n}");
        return (a.n, b.n);
    }

    string Compare(Code code, (SV v, string n) a, (SV v, string n) b)
    {
        bool f = IsF(a.v.K) || IsF(b.v.K);
        var (x, y) = Operands(a, b, code is Code.Cgt_Un or Code.Clt_Un);
        return code switch
        {
            Code.Ceq => $"{x} == {y}",
            Code.Cgt => $"{x} > {y}",
            Code.Clt => $"{x} < {y}",
            Code.Cgt_Un => f ? $"!({x} <= {y})" : $"{x} > {y}",
            _ => f ? $"!({x} >= {y})" : $"{x} < {y}",
        };
    }

    string BranchCond(Code code, (SV v, string n) a, (SV v, string n) b)
    {
        bool f = IsF(a.v.K) || IsF(b.v.K);
        var name = code.ToString().Replace("_S", "");
        bool un = name.EndsWith("_Un");
        var (x, y) = Operands(a, b, un && !f);
        return name switch
        {
            "Beq" => $"{x} == {y}",
            "Bne_Un" => $"{x} != {y}",
            "Bge" => $"{x} >= {y}", "Bgt" => $"{x} > {y}", "Ble" => $"{x} <= {y}", "Blt" => $"{x} < {y}",
            "Bge_Un" => f ? $"!({x} < {y})" : $"{x} >= {y}",
            "Bgt_Un" => f ? $"!({x} <= {y})" : $"{x} > {y}",
            "Ble_Un" => f ? $"!({x} > {y})" : $"{x} <= {y}",
            "Blt_Un" => f ? $"!({x} >= {y})" : $"{x} < {y}",
            _ => throw new EmitError("branch " + name),
        };
    }

    void Conv(Code code)
    {
        var a = Pop();
        var k = a.v.K;
        var n = k == SK.P ? $"(intptr_t){a.n}" : a.n;
        bool f = IsF(k);
        switch (code)
        {
            case Code.Conv_I1: Push(new SV(SK.I4, null), f ? $"(int8_t)nr_d2i4({n})" : $"(int8_t){n}"); break;
            case Code.Conv_I2: Push(new SV(SK.I4, null), f ? $"(int16_t)nr_d2i4({n})" : $"(int16_t){n}"); break;
            case Code.Conv_I4: Push(new SV(SK.I4, null), f ? $"nr_d2i4({n})" : $"(int32_t){n}"); break;
            case Code.Conv_U1: Push(new SV(SK.I4, null), f ? $"(uint8_t)nr_d2i4({n})" : $"(uint8_t){n}"); break;
            case Code.Conv_U2: Push(new SV(SK.I4, null), f ? $"(uint16_t)nr_d2i4({n})" : $"(uint16_t){n}"); break;
            case Code.Conv_U4: Push(new SV(SK.I4, null), f ? $"(int32_t)(uint32_t)nr_d2i8({n})" : $"(int32_t)(uint32_t){n}"); break;
            case Code.Conv_I8: Push(new SV(SK.I8, null), f ? $"nr_d2i8({n})" : $"(int64_t){n}"); break;
            case Code.Conv_U8: Push(new SV(SK.I8, null), f ? $"(int64_t)nr_d2u8({n})" : k == SK.I8 ? n : $"(int64_t)(uint32_t){n}"); break;
            case Code.Conv_I: Push(new SV(SK.I, null), f ? $"nr_d2i4({n})" : $"(int32_t){n}"); break;
            case Code.Conv_U: Push(new SV(SK.I, null), f ? $"(int32_t)(uint32_t)nr_d2i8({n})" : $"(int32_t)(uint32_t){n}"); break;
            case Code.Conv_R4: Push(new SV(SK.R4, null), $"(float){n}"); break;
            case Code.Conv_R8: Push(new SV(SK.R8, null), $"(double){n}"); break;
            case Code.Conv_R_Un: Push(new SV(SK.R8, null), k == SK.I8 ? $"(double)(uint64_t){n}" : f ? $"(double){n}" : $"(double)(uint32_t){n}"); break;
        }
    }

    void ConvOvf(string name)
    {
        // conv.ovf.i4.un -> nr_ovf_i4_un_from_{i4|i8|r8}
        var a = Pop();
        var parts = name.Split('.');
        var to = parts[2];
        var un = parts.Length > 3 ? "_un" : "";
        var from = a.v.K switch { SK.I8 => "i8", SK.R4 or SK.R8 => "r8", _ => "i4" };
        var rk = to is "i8" or "u8" ? SK.I8 : to is "i" or "u" ? SK.I : SK.I4;
        var arg = a.v.K == SK.P ? $"(intptr_t){a.n}" : a.n;
        Push(new SV(rk, null), $"nr_ovf_{to}{un}_{from}({arg})");
    }

    void LdInd(string c, SK k) { var p = Pop(); Push(new SV(k, null), $"*({c}*){p.n}"); }
    void StInd(string c) { var v = Pop(); var p = Pop(); L($"*({c}*){p.n} = ({c}){(v.v.K == SK.P && c != "void*" ? "(intptr_t)" : "")}{v.n};"); }

    void LdElem(string c, SK k, TT? t = null)
    {
        var i = Pop(); var a = Pop();
        Push(new SV(k, t), $"(({c}*)NR_ADATA({a.n}))[NR_AIDX({a.n}, {i.n})]");
    }
    void StElem(string c, TT? t = null)
    {
        var v = Pop(); var i = Pop(); var a = Pop();
        if (c == "void*" || (t != null && lay.Kind(t) == SK.P)) { L($"nr_stelem_ref({a.n}, {i.n}, {v.n});"); return; }
        var val = t != null ? As(t, v) : $"({c}){(v.v.K == SK.P ? "(intptr_t)" : "")}{v.n}";
        L($"(({c}*)NR_ADATA({a.n}))[NR_AIDX({a.n}, {i.n})] = {val};");
    }

    void Field(Code code, FieldReference fr)
    {
        var f = w.Field(fr, m.Owner.Args, m.MArgs);
        if (f.Def.IsStatic) { StaticField(code == Code.Ldfld ? Code.Ldsfld : code == Code.Ldflda ? Code.Ldsflda : Code.Stsfld, fr, true); return; }
        (SV v, string n) val = default;
        if (code == Code.Stfld) val = Pop();
        var obj = Pop();
        string acc;
        if (Layouts.IsPrim(f.Owner) || f.Owner.IsEnum)
        {
            // Int32.m_value, IntPtr.m_value, an enum's value__: the value is the field.
            var c = lay.C(f.Type);
            acc = obj.v.K is SK.P ? $"(*({c}*){obj.n})" : obj.v.K == SK.I && code != Code.Ldfld ? $"(*({c}*)(intptr_t){obj.n})" : obj.n;
            if (obj.v.K == SK.I && code == Code.Ldfld && f.Owner.Def.FullName is not ("System.IntPtr" or "System.UIntPtr")) acc = $"(*({c}*)(intptr_t){obj.n})";
            switch (code)
            {
                case Code.Ldfld: Push(Kind(f.Type), acc); break;
                case Code.Ldflda: Push(new SV(SK.P, w.ByRef(f.Type)), obj.v.K == SK.P ? obj.n : $"(void*)(intptr_t){obj.n}"); break;
                default: L($"{acc} = {As(f.Type, val)};"); break;
            }
            return;
        }
        var fn = g.FieldC(f);
        if (obj.v.K == SK.VT) acc = $"{obj.n}.{fn}";
        else
        {
            var sn = f.Owner.IsValueType ? lay.C(f.Owner) : g.ObjStruct(f.Owner);
            acc = $"(({sn}*){(obj.v.K == SK.P ? "" : "(intptr_t)")}{obj.n})->{fn}";
        }
        switch (code)
        {
            case Code.Ldfld: Push(Kind(f.Type), acc); break;
            case Code.Ldflda: Push(new SV(SK.P, w.ByRef(f.Type)), "&" + acc); break;
            default: L($"{acc} = {As(f.Type, val)};"); break;
        }
    }

    void StaticField(Code code, FieldReference fr, bool popObj = false)
    {
        var f = w.Field(fr, m.Owner.Args, m.MArgs);
        (SV v, string n) val = default;
        if (code == Code.Stsfld) val = Pop();
        if (popObj) Pop();
        if (g.NeedsCctorCheck(f.Owner, m)) L($"NR_CCTOR({g.Cctor(f.Owner)});");
        if (f.Def.HasFieldRVA && code == Code.Ldsflda) { Push(new SV(SK.I, null), $"(int32_t)(intptr_t){g.Rva(f)}"); return; }
        var acc = $"{g.Statics(f.Owner)}.{g.FieldC(f)}";
        switch (code)
        {
            case Code.Ldsfld: Push(Kind(f.Type), acc); break;
            case Code.Ldsflda: Push(new SV(SK.P, w.ByRef(f.Type)), "&" + acc); break;
            default: L($"{acc} = {As(f.Type, val)};"); break;
        }
    }

    void Box(TT t)
    {
        var a = Pop();
        if (!t.IsValueType) { Push(new SV(SK.P, t), a.n); return; }
        g.Construct(t);
        if (g.IsNullable(t))
        {
            var inner = t.Args[0];
            g.Construct(inner);
            Push(new SV(SK.P, inner), $"nr_box_nullable(&{g.Klass(inner)}, &{a.n}, {g.NullableHasValueOffset(t)}, {g.NullableValueOffset(t)})");
            return;
        }
        var tv = $"bx{tmpN++}";
        vars[tv] = lay.C(t);
        L($"{tv} = {As(t, a)};");
        Push(new SV(SK.P, t), $"nr_box(&{g.Klass(t)}, &{tv})");
    }

    void UnboxAny(TT t)
    {
        var a = Pop();
        if (!t.IsValueType) { Push(new SV(SK.P, t), $"nr_castclass({a.n}, &{g.Klass(t)})"); return; }
        if (g.IsNullable(t))
        {
            var tv = $"ub{tmpN++}";
            vars[tv] = lay.C(t);
            L($"nr_unbox_nullable({a.n}, &{g.Klass(t.Args[0])}, &{tv}, sizeof({tv}), {g.NullableHasValueOffset(t)}, {g.NullableValueOffset(t)});");
            Push(Kind(t), tv);
            return;
        }
        Push(Kind(t), $"*({lay.C(t)}*)nr_unbox({a.n}, &{g.Klass(t)})");
    }

    void Ldtoken(object operand)
    {
        switch (operand)
        {
            case TypeReference tr:
            {
                var t = w.Conv(tr, m.Owner.Args, m.MArgs);
                var h = w.Cor("System", "RuntimeTypeHandle");
                Push(Kind(h), $"(({lay.C(h)}){{ (intptr_t)nr_class_type(&{g.Klass(t)}) }})");
                break;
            }
            case FieldReference fr:
            {
                var f = w.Field(fr, m.Owner.Args, m.MArgs);
                var h = w.Cor("System", "RuntimeFieldHandle");
                Push(Kind(h), $"(({lay.C(h)}){{ (intptr_t)&{g.FieldHandle(f)} }})");
                break;
            }
            case MethodReference mr:
            {
                var t = w.ConvMethod(mr, m.Owner.Args, m.MArgs) ?? throw new EmitError("ldtoken array method");
                var h = w.Cor("System", "RuntimeMethodHandle");
                Push(Kind(h), $"(({lay.C(h)}){{ (intptr_t)&{g.MethodDesc(t)} }})");
                break;
            }
            default: throw new EmitError("ldtoken " + operand);
        }
    }

    // ---- calls ----
    List<string> Args(MI t, int n)
    {
        var a = new List<string>();
        for (int i = t.Def.Parameters.Count - 1; i >= 0; i--) a.Insert(0, As(w.ParamType(t, i), Pop()));
        return a;
    }

    void Call(Instruction ins, bool virt, TT? constrained)
    {
        var mr = (MethodReference)ins.Operand;
        var t = w.ConvMethod(mr, m.Owner.Args, m.MArgs);
        if (t == null) { ArrayMethod(mr); return; }
        var args = Args(t, t.Def.Parameters.Count);
        string? self = null;
        TT? selfT = null;
        if (t.Def.HasThis)
        {
            var s = Pop();
            self = s.n; selfT = s.v.T;
            if (s.v.K != SK.P) self = $"(void*)(intptr_t){s.n}";
        }
        var rt = w.RetType(t);
        string callee;
        if (constrained != null)
        {
            // constrained. T callvirt M: call T's own implementation on the
            // address, box when T inherits M, dereference when T is a class.
            if (!constrained.IsValueType) { self = $"*(void**){self}"; callee = Dispatch(t, virt, self!); }
            else
            {
                var impl = g.ValueTypeImpl(constrained, t);
                if (impl != null) callee = g.Fn(impl);
                else
                {
                    g.Construct(constrained);
                    self = $"nr_box(&{g.Klass(constrained)}, {self})";
                    var bx = $"cb{tmpN++}"; vars[bx] = "void*";
                    L($"{bx} = {self};"); self = bx;
                    callee = Dispatch(t, true, self);
                }
            }
        }
        else callee = Dispatch(t, virt, self);
        var all = self != null ? new[] { self }.Concat(args) : args;
        var call = $"{callee}({string.Join(", ", all)})";
        if (Layouts.IsVoid(rt)) L(call + ";");
        else Push(Kind(rt), call);
    }

    // Returns a C expression for the function to call.
    string Dispatch(MI t, bool virt, string? self)
    {
        var d = t.Def;
        if (!virt || !d.IsVirtual || d.IsFinal || (t.Owner.K == TK.Def && t.Owner.Def.IsSealed && !t.Owner.IsInterface))
        {
            if (virt && self != null && t.Def.HasThis) L($"NR_NULLCHK({self});");
            return g.Fn(t);
        }
        return $"(({g.FnPtrType(t)}){VirtualFn(t, self!)})";
    }

    string VirtualFn(MI t, string self)
    {
        if (t.Def.HasGenericParameters) return $"nr_gvm({self}, &{g.Gvm(t)})";
        if (!t.Def.IsVirtual) return g.FnPtr(t);
        if (t.Owner.IsInterface) return $"nr_iface({self}, &{g.Klass(t.Owner)}, {g.IfaceIndex(t)})";
        return $"NR_VT({self})[{g.Slot(t)}]";
    }

    void NewObj(Instruction ins)
    {
        var mr = (MethodReference)ins.Operand;
        var t = w.ConvMethod(mr, m.Owner.Args, m.MArgs);
        if (t == null)
        {
            // multi-dimensional array constructor
            var at = w.Conv(mr.DeclaringType, m.Owner.Args, m.MArgs);
            g.Construct(at);
            int n = mr.Parameters.Count;
            var dims = new List<string>();
            for (int i = 0; i < n; i++) dims.Insert(0, Pop().n);
            Push(new SV(SK.P, at), $"nr_new_mdarray(&{g.Klass(at)}, {n}, (int32_t[]){{ {string.Join(", ", dims)} }})");
            return;
        }
        var owner = t.Owner;
        if (owner.K == TK.Def && owner.Def.FullName == "System.String" && owner.Def.Module.Assembly.Name.Name == "mscorlib")
        {
            var cs = g.StringCreate(t);
            var args = Args(t, t.Def.Parameters.Count);
            Push(new SV(SK.P, owner), $"{g.Fn(cs)}({string.Join(", ", new[] { "0" }.Concat(args))})");
            return;
        }
        g.Construct(owner);
        if (g.IsDelegate(owner))
        {
            var fn = Pop(); var target = Pop();
            var desc = ins.Previous?.OpCode.Code is Code.Ldftn or Code.Ldvirtftn
                ? "&" + g.MethodDesc(Meth(ins.Previous)) : "0";
            // The new object takes the target's stack slot: build it in a temp.
            var dv = $"no{tmpN++}";
            vars[dv] = "void*";
            L($"{dv} = nr_new(&{g.Klass(owner)});");
            L($"nr_delegate_init({dv}, {target.n}, (void*)(intptr_t){fn.n}, {desc});");
            Push(new SV(SK.P, owner), dv);
            return;
        }
        var cargs = Args(t, t.Def.Parameters.Count);
        if (owner.IsValueType)
        {
            var tv = $"nv{tmpN++}";
            vars[tv] = lay.C(owner);
            L($"memset(&{tv}, 0, sizeof({tv}));");
            L($"{g.Fn(t)}({string.Join(", ", new[] { "&" + tv }.Concat(cargs))});");
            Push(Kind(owner), tv);
            return;
        }
        var ov = $"no{tmpN++}";
        vars[ov] = "void*";
        L($"{ov} = nr_new(&{g.Klass(owner)});");
        L($"{g.Fn(t)}({string.Join(", ", new[] { ov }.Concat(cargs))});");
        Push(new SV(SK.P, owner), ov);
    }

    // Get/Set/Address on multi-dimensional arrays.
    void ArrayMethod(MethodReference mr)
    {
        var at = w.Conv(mr.DeclaringType, m.Owner.Args, m.MArgs);
        var et = at.Elem;
        var c = lay.C(et);
        int rank = at.Rank;
        if (mr.Name == "Set")
        {
            var v = Pop();
            var idx = new List<string>(); for (int i = 0; i < rank; i++) idx.Insert(0, Pop().n);
            var a = Pop();
            L($"*({c}*)nr_mdarray_addr({a.n}, {rank}, (int32_t[]){{ {string.Join(", ", idx)} }}) = {As(et, v)};");
            return;
        }
        {
            var idx = new List<string>(); for (int i = 0; i < rank; i++) idx.Insert(0, Pop().n);
            var a = Pop();
            var addr = $"nr_mdarray_addr({a.n}, {rank}, (int32_t[]){{ {string.Join(", ", idx)} }})";
            if (mr.Name == "Get") Push(Kind(et), $"*({c}*){addr}");
            else if (mr.Name == "Address") Push(new SV(SK.P, w.ByRef(et)), addr);
            else throw new EmitError("array method " + mr.Name);
        }
    }
}
