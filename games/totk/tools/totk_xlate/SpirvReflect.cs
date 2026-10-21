// TOTK (Windows port, docs/windows-port-plan.md C10): reflection of the translator's SPIR-V module, read straight from the
// SPIR-V words (no SPIRV-Cross, no MSL). Used by the --target spirv service mode (Service.cs HandleSpirv) to describe each
// resource variable with its Vulkan descriptor set / binding / descriptor type, plus the facts the MSL chain used to
// recover from MSL text: vertex inputs, fragment outputs, driver constant bank reads (c0 / c2 offsets), stores through
// storage buffers, FP64 use, depth writes, compute local size.
using System;
using System.Collections.Generic;
using System.IO;

sealed class SpirvModule
{
    public sealed class Var
    {
        public uint Id, TypePtr, Pointee; public int StorageClass; public string Name = "";
        public int Set = -1, Binding = -1, Location = -1, BuiltIn = -1; public bool Stored, NonWritable;
    }
    public sealed class Ty { public int Op; public uint[] W = Array.Empty<uint>(); }

    public readonly Dictionary<uint, Var> Vars = new();
    public readonly Dictionary<uint, Ty> Types = new();
    public readonly Dictionary<uint, uint> Consts = new();          // 32-bit integer constants
    public readonly Dictionary<uint, string> Names = new();
    public readonly Dictionary<uint, List<(int dec, uint[] args)>> Decos = new();
    public readonly Dictionary<uint, uint> ChainRoot = new();       // access chain result -> root variable
    public readonly List<(uint root, uint[] idx)> Chains = new();   // every access chain from a variable (for driver bank reads)
    public readonly List<int> Capabilities = new();
    public readonly List<string> Extensions = new();
    public int ExecModel = -1; public string EntryName = "main"; public uint EntryId;
    public int[] LocalSize; public bool UsesFp64, DepthReplacing;

    static string Str(uint[] w, int at, out int next)
    {
        var bytes = new List<byte>(); int i = at;
        for (; i < w.Length; i++)
        {
            uint x = w[i]; bool end = false;
            for (int k = 0; k < 4; k++) { byte b = (byte)(x >> (8 * k)); if (b == 0) { end = true; break; } bytes.Add(b); }
            if (end) { i++; break; }
        }
        next = i; return System.Text.Encoding.UTF8.GetString(bytes.ToArray());
    }

    public SpirvModule(byte[] code)
    {
        if (code.Length < 20 || code.Length % 4 != 0) throw new InvalidDataException("SPIR-V: bad length");
        var all = new uint[code.Length / 4];
        Buffer.BlockCopy(code, 0, all, 0, code.Length);
        if (all[0] != 0x07230203) throw new InvalidDataException("SPIR-V: bad magic");
        for (int p = 5; p < all.Length;)
        {
            int n = (int)(all[p] >> 16), op = (int)(all[p] & 0xffff);
            if (n == 0 || p + n > all.Length) throw new InvalidDataException($"SPIR-V: bad instruction at word {p}");
            var w = new uint[n - 1]; Array.Copy(all, p + 1, w, 0, n - 1);
            switch (op)
            {
                case 17: Capabilities.Add((int)w[0]); break;                                  // OpCapability
                case 10: Extensions.Add(Str(w, 0, out _)); break;                             // OpExtension
                case 15: ExecModel = (int)w[0]; EntryId = w[1]; EntryName = Str(w, 2, out _); break;   // OpEntryPoint
                case 16:                                                                      // OpExecutionMode
                    if (w[1] == 17 && w.Length >= 5) LocalSize = new[] { (int)w[2], (int)w[3], (int)w[4] };
                    if (w[1] == 12) DepthReplacing = true;
                    break;
                case 5: Names[w[0]] = Str(w, 1, out _); break;                                // OpName
                case 71:                                                                      // OpDecorate
                    if (!Decos.TryGetValue(w[0], out var l)) Decos[w[0]] = l = new();
                    l.Add(((int)w[1], w[2..])); break;
                case 22: if (w[1] == 64) UsesFp64 = true; goto case 21;                       // OpTypeFloat
                case 19: case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 32:
                    Types[w[0]] = new Ty { Op = op, W = w }; break;                           // OpType*
                case 43: if (w.Length >= 3) Consts[w[1]] = w[2]; break;                      // OpConstant
                case 59:                                                                      // OpVariable
                    Vars[w[1]] = new Var { Id = w[1], TypePtr = w[0], StorageClass = (int)w[2] };
                    ChainRoot[w[1]] = w[1]; break;
                case 65: case 66: case 67: case 70:                                          // Op(InBounds)(Ptr)AccessChain, OpCopyObject handled below
                    if (ChainRoot.TryGetValue(w[2], out uint root))
                    {
                        ChainRoot[w[1]] = root;
                        if (w[2] == root) Chains.Add((root, w[3..]));
                    }
                    break;
                case 83: if (ChainRoot.TryGetValue(w[2], out uint r2)) ChainRoot[w[1]] = r2; break;   // OpCopyObject
                case 62: if (ChainRoot.TryGetValue(w[0], out uint r3)) StoreTo(r3); break;           // OpStore
                case 63: if (ChainRoot.TryGetValue(w[0], out uint r4)) StoreTo(r4); break;           // OpCopyMemory target
                case 228: if (ChainRoot.TryGetValue(w[0], out uint r5)) StoreTo(r5); break;          // OpAtomicStore
                default:
                    if (op >= 227 && op <= 242 && op != 228 && w.Length >= 3 && ChainRoot.TryGetValue(w[2], out uint r6)) StoreTo(r6);   // atomics (read-modify-write)
                    else if ((op == 318 || op == 319) && w.Length >= 3 && ChainRoot.TryGetValue(w[2], out uint r7)) StoreTo(r7);   // OpAtomicFMinEXT / FMaxEXT
                    else if (op == 6035 && w.Length >= 3 && ChainRoot.TryGetValue(w[2], out uint r8)) StoreTo(r8);   // OpAtomicFAddEXT
                    break;
            }
            p += n;
        }
        foreach (var v in Vars.Values)
        {
            if (Names.TryGetValue(v.Id, out string nm)) v.Name = nm;
            if (Types.TryGetValue(v.TypePtr, out Ty pt) && pt.Op == 32) v.Pointee = pt.W[2];
            if (Decos.TryGetValue(v.Id, out var ds))
                foreach (var (dec, a) in ds)
                    switch (dec)
                    {
                        case 34: v.Set = (int)a[0]; break;
                        case 33: v.Binding = (int)a[0]; break;
                        case 30: v.Location = (int)a[0]; break;
                        case 11: v.BuiltIn = (int)a[0]; break;
                        case 24: v.NonWritable = true; break;
                    }
        }
    }
    void StoreTo(uint root) { if (Vars.TryGetValue(root, out Var v)) v.Stored = true; }

    public bool HasDeco(uint id, int dec) => Decos.TryGetValue(id, out var l) && l.Exists(d => d.dec == dec);
    public Ty Type(uint id) => Types.TryGetValue(id, out Ty t) ? t : null;

    /// Strips arrays: element type id and total element count (0 = runtime array).
    public uint Elem(uint ty, out int count)
    {
        count = 1;
        for (Ty t = Type(ty); t != null && (t.Op == 28 || t.Op == 29); t = Type(ty))
        {
            if (t.Op == 29) count = 0; else count *= Consts.TryGetValue(t.W[2], out uint c) ? (int)c : 1;
            ty = t.W[1];
        }
        return ty;
    }

    /// Scalar class of a numeric type (through vectors): "float", "int", "uint", or "?".
    public string ScalarClass(uint ty)
    {
        Ty t = Type(ty);
        while (t != null && t.Op == 23) t = Type(t.W[1]);
        if (t == null) return "?";
        return t.Op == 22 ? "float" : t.Op == 21 ? (t.W[2] != 0 ? "int" : "uint") : "?";
    }
}
