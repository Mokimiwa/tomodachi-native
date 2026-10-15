// main 程序本體內嵌的 4 個 Maxwell 計算著色器（PC 版無對應）→ 手寫 MSL，逐條按 envydis 反匯編等價。
// 出處：main 原映像 .rodata 控制段 @0x1df10e8 / 0x1df1a94 / 0x1df2440 / 0x1df2dec（envydis -m gm107）。
// 共同骨架（Maxwell 原文）：
//   id = (ctaid.x << 5) + tid.x                       // 線程組 32×1×1
//   if (id >= c3[0x0]) exit                           // 元素個數 = uniform 緩衝 0 的第 0 個字（u32 比較）
//   src = ssbo0（c0[0x310]），dst = ssbo1（c0[0x320]），地址 = 基址 + 32 位偏移
// 各核差別（NVN 計數器報告 16 字節 = {計數 u64, 時間戳 u64}）：
//   q_val0   dst[id] = *(u64*)(src + (id<<4))                         取計數
//   q_val8   dst[id] = *(u64*)(src + (id<<4) + 8)                     取時間戳
//   q_dlt0   dst[id] = *(u64*)(src + (id<<5) + 16) - *(u64*)(src + (id<<5))        前後兩報告的計數差
//   q_dlt8   dst[id] = *(u64*)(src + (id<<5) + 24) - *(u64*)(src + (id<<5) + 8)    前後兩報告的時間戳差
// The 4 Maxwell compute shaders embedded in the main program itself (no counterpart in the PC version) -> hand-written MSL, equivalent instruction by instruction to the envydis disassembly.
// Source: control segments in the .rodata of the original main image @0x1df10e8 / 0x1df1a94 / 0x1df2440 / 0x1df2dec (envydis -m gm107).
// Common skeleton (original Maxwell):
//   id = (ctaid.x << 5) + tid.x                       // thread group 32x1x1
//   if (id >= c3[0x0]) exit                           // element count = word 0 of uniform buffer 0 (u32 compare)
//   src = ssbo0 (c0[0x310]), dst = ssbo1 (c0[0x320]), address = base + 32-bit offset
// Differences between kernels (an NVN counter report is 16 bytes = {count u64, timestamp u64}):
//   q_val0   dst[id] = *(u64*)(src + (id<<4))                         take the count
//   q_val8   dst[id] = *(u64*)(src + (id<<4) + 8)                     take the timestamp
//   q_dlt0   dst[id] = *(u64*)(src + (id<<5) + 16) - *(u64*)(src + (id<<5))        count difference between two consecutive reports
//   q_dlt8   dst[id] = *(u64*)(src + (id<<5) + 24) - *(u64*)(src + (id<<5) + 8)    timestamp difference between two consecutive reports
#pragma once

static const char *const k_nvn_builtin_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#define HEAD uint id = (cta.x << 5) + tid.x; if (id >= n[0]) return;\n"
    "#define LD(o) (*(device const ulong *)(src + (o)))\n"
    "#define ARGS constant uint *n [[buffer(0)]], device const uchar *src [[buffer(1)]], device uchar *dst [[buffer(2)]], "
    "uint3 cta [[threadgroup_position_in_grid]], uint3 tid [[thread_position_in_threadgroup]]\n"
    "#define ST(v) (*(device ulong *)(dst + (id << 3)) = (v))\n"
    "kernel void q_val0(ARGS) { HEAD ST(LD(id << 4)); }\n"
    "kernel void q_val8(ARGS) { HEAD ST(LD((id << 4) + 8u)); }\n"
    "kernel void q_dlt0(ARGS) { HEAD uint b = id << 5; ST(LD(b + 16u) - LD(b)); }\n"
    "kernel void q_dlt8(ARGS) { HEAD uint b = (id << 5) + 16u; ST(LD(b + 8u) - LD(b - 8u)); }\n";

// Switch 控制段哈希（sha256(控制段 0x880 字節) 前 16 位）→ 內核名
// Switch control segment hash (first 16 hex digits of sha256(0x880 bytes of the control segment)) -> kernel name
static const struct { const char *ctl; const char *fn; } k_nvn_builtin_map[] = {
    {"d0f769bb916bbba9", "q_val0"},
    {"16479cc6d75c2b28", "q_val8"},
    {"0687e2ef95133577", "q_dlt0"},
    {"4df5909e6dabee05", "q_dlt8"},
};
