// Switch 原版著色器（Maxwell 機器碼）→ SPIR-V（Ryujinx.Graphics.Shader 翻譯器），供 B 路線離線出 metallib。
// 與 Ryujinx.ShaderTools 的區別：翻譯器的「查詢」按真實情況作答，而不是一律缺省值——
//   貼圖類型 / 圖像格式 / 頂點屬性類型 / 片元輸出類型 / 計算線程組大小取自作業文件（由映射表配對的 PC 反射給出，同槽號 = 同資源）；
//   窗口原點左上、深度 -1..1（馬力歐實測 nvnDeviceSetWindowOriginMode 左上 / SetDepthMode 近=-w，即 GL 口徑）：Metal 裁剪要求 0 ≤ z ≤ w，
//   頂點著色器末尾按 Ryujinx 的辦法改寫 z' = z/2 + w/2（DepthMode + 宿主不支持深度裁剪控制）；不改的話 z = -w 的全屏拷貝整個被裁掉，窗口全黑。
// 用法：dmsh_xlate <作業.json> <輸出目錄>
//   作業：[{ "key", "stage": "vs|ps|cs", "code": 機器碼文件（圖形 = 0x50 字節 SPH + 指令，計算 = 指令），
//            "tex": { "槽": "2d|2darray|cube|cubearray|3d|buffer|1d|1darray|2dms|2dmsarray", … }, "img": { "槽": { "type", "fmt" } },
//            "attr": { "位置": "float|sint|uint" }, "fout": { "目標": "float|sint|uint" }, "local": [x, y, z] }]
//   輸出：<key>.spv 與 <key>.res.json（存儲緩衝槽在 c0 的偏移等，貼圖句柄 → 槽號的解碼在出 metallib 的腳本裡做）
// Original Switch shaders (Maxwell machine code) → SPIR-V (the Ryujinx.Graphics.Shader translator), for route B's offline metallib build.
// Difference from Ryujinx.ShaderTools: the translator's "queries" are answered truthfully rather than always with defaults -
//   texture type / image format / vertex attribute type / fragment output type / compute thread group size come from the job file (given by the PC reflection paired via the mapping table; same slot number = same resource);
//   window origin upper-left, depth -1..1 (measured on Mario: nvnDeviceSetWindowOriginMode upper-left / SetDepthMode near=-w, i.e. the GL convention): Metal clipping requires 0 ≤ z ≤ w,
//   so vertex shaders rewrite z' = z/2 + w/2 at the end, Ryujinx's way (DepthMode + host without depth clip control); without it the full-screen copy at z = -w is clipped away entirely and the window is black.
// Usage: dmsh_xlate <job.json> <output dir>
//   Job: [{ "key", "stage": "vs|ps|cs", "code": machine code file (graphics = 0x50-byte SPH + instructions, compute = instructions),
//           "tex": { "slot": "2d|2darray|cube|cubearray|3d|buffer|1d|1darray|2dms|2dmsarray", ... }, "img": { "slot": { "type", "fmt" } },
//           "attr": { "location": "float|sint|uint" }, "fout": { "target": "float|sint|uint" }, "local": [x, y, z] }]
//   Output: <key>.spv and <key>.res.json (offsets of storage buffer slots in c0, etc.; decoding texture handle → slot number is done in the script that builds the metallib)
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class SwshXlate
{
    const int TexBase = 0x20, SampBase = 0x568;   // NVN 貼圖句柄在 c[2] 的字節基址（每槽 8 字節）：本作（NVN 55）0x20，全庫實測見 dm/gfx/tools/dmsh_build.py TEX_BASE；分離採樣器沿用 D2R 的 0x568（本作不用） | byte base of NVN texture handles in c[2] (8 bytes per slot): 0x20 for this game (NVN 55), measured over the whole library, see TEX_BASE in dm/gfx/tools/dmsh_build.py; separate samplers keep D2R's 0x568 (unused by this game)

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1; public readonly List<string> Notes = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job) { _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); }
        // c1 常數段（著色器包控制段前 0x100 字節）：間接跳轉 BRX 的跳轉表在這裡（LDC c[1]）；不提供時翻譯器解不出跳轉目標、控制流出錯（粒子著色器死循環）
        // c1 constant segment (the 0x100 bytes before the control segment in the shader package): the jump table of indirect jump BRX lives here (LDC c[1]); without it the translator cannot resolve jump targets and control flow goes wrong (particle shaders loop forever)
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 讀越界 +{offset:X}（c1 長 {_c1.Length}）"); return 0; }
        // 全部資源放進第 0 組，各佔不同綁定號（spirv-cross 出成一個參數緩衝）；常量段號 index 的緩衝由翻譯器給 binding = index+1 的慣例不依賴
        // All resources go into set 0, each with a distinct binding number (spirv-cross emits them as one argument buffer); the translator's convention of giving the buffer of constant segment number index binding = index+1 is not relied on
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        public int QuerySamplerArrayLengthFromPool() => 32;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 32;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        int TexSlot(int handle, int cbufSlot)
        {
            (int off0, _, _) = TextureHandle.UnpackOffsets(handle);
            int bytes = off0 * 4;
            return (cbufSlot < 0 || (cbufSlot & 0xffff) == 2) && bytes >= TexBase && bytes < SampBase && (bytes - TexBase) % 8 == 0 ? (bytes - TexBase) / 8 : -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            string t = slot >= 0 ? Get("tex", slot) ?? Get("img", slot) : null;
            if (t == null) { Notes.Add($"貼圖句柄 {handle:X}（常量段 {cbufSlot}，槽 {slot}）無 PC 類型，按 2D"); return SamplerType.Texture2D; }
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"圖像句柄 {handle:X} 槽 {slot} 無 PC 格式，按 R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;   // 配對 PC 的 groupshared 聲明之和 | sum of the paired PC shader's groupshared declarations
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            return new GpuGraphicsState(false, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, true /* 深度 -1..1（GL）| depth -1..1 (GL) */, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* 不做 Y 取反 | no Y flip */, true /* 原點左上 | origin upper-left */, false);
        }
        public bool QueryHostSupportsTextureShadowLod() => true;
        public bool QueryHostSupportsDepthClipControl() => false;   // Metal 沒有 -1..1 裁剪控制：讓翻譯器在頂點著色器裡改寫 z（見文件頭） | Metal has no -1..1 clip control: have the translator rewrite z in the vertex shader (see the file header)
        // 作業 reduced = true：放寬精度——翻譯器只給強制精確的運算打 NoContraction（缺省全部打：保持 Maxwell 分開捨入的逐位結果，代價是 MSL 裡大量禁優化的 spvFMul 等）
        // Job reduced = true: relaxed precision - the translator marks NoContraction only on operations that must be exact (the default marks all: keeps the bit-exact results of Maxwell's separate rounding, at the cost of many optimization-disabled spvFMul etc. in the MSL)
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--source-sha")   // 嵌入的 Program.cs + csproj 的 sha256 前 16 位（dmsh_chain.py 同口徑） | first 16 digits of the sha256 of the embedded Program.cs + csproj (same convention as dmsh_chain.py)
        {
            var asm = typeof(SwshXlate).Assembly; var ms = new MemoryStream();
            foreach (var n in new[] { "src0", "src1" }) asm.GetManifestResourceStream(n).CopyTo(ms);
            Console.WriteLine(Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(ms.ToArray()))[..16].ToLowerInvariant());
            return 0;
        }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray(); string outDir = args[1]; Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string key = (string)job["key"];
            try
            {
                byte[] code = File.ReadAllBytes((string)job["code"]);
                var acc = new Accessor(code, job);
                // 作業 debug = false：關 DebugMode（DebugMode 會跳過 AST 優化器，產物滿是臨時變量；不改語義）
                // Job debug = false: turn off DebugMode (DebugMode skips the AST optimizer and the output is full of temporaries; semantics unchanged)
                var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | ((string)job["stage"] == "cs" ? TranslationFlags.Compute : 0);
                var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                ShaderProgram p = ctx.Translate();
                File.WriteAllBytes(Path.Combine(outDir, key + ".spv"), p.BinaryCode);
                var res = new JsonObject
                {
                    ["key"] = key, ["stage"] = (string)job["stage"], ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                    ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                    ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                };
                foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
                foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
                foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                    foreach (var t in list) res[name].AsArray().Add(new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
                File.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                ok++;
            }
            catch (Exception e) { bad++; Console.Error.WriteLine($"【紅】{key}：{e.GetType().Name}：{e.Message.Split('\n')[0]}"); if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace); }
        }
        Console.WriteLine($"翻譯 {ok} 成功，{bad} 失敗");
        return bad == 0 ? 0 : 1;
    }
}
