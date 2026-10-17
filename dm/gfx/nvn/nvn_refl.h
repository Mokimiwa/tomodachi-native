// 著色器反射 json → 定長 C 數組（A：PC 鏈反射；B：Switch 原版翻譯的反射，route = "B"，tools/dmsh_build.py 出）。
// B 新增種類：SBUFP = 存儲緩衝設備指針（NVN 存儲緩衝槽）；SUPPORT = Ryujinx 輔助常量；C1 = 著色器自帶常數段（c[1]）；
//            DRV0 / DRV2 = NVN 驅動常量段 c[0]（存儲緩衝地址表）/ c[2]（貼圖與採樣器句柄表）
// PC 著色器反射 json → 定長 C 數組（取著色器時轉一次；每次繪製只遍歷數組，不碰 NSDictionary / NSString）。
// 單測 tests/t_refl.m 拿真實反射手算期望。
// Shader reflection json → fixed-length C arrays (A: PC chain reflection; B: reflection produced by translating the original Switch shaders, route = "B", output of tools/dmsh_build.py).
// Kinds added by B: SBUFP = storage buffer device pointer (NVN storage buffer slot); SUPPORT = Ryujinx auxiliary constants; C1 = the shader's own constant segment (c[1]);
//            DRV0 / DRV2 = NVN driver constant segments c[0] (storage buffer address table) / c[2] (texture and sampler handle table)
// PC shader reflection json → fixed-length C arrays (converted once when the shader is fetched; each draw only walks the arrays and does not touch NSDictionary / NSString).
// Unit test tests/t_refl.m computes the expectations by hand from real reflection.
#pragma once
#include <stdint.h>

enum { NVN_RB_CBUF = 1, NVN_RB_SAMP, NVN_RB_TEX, NVN_RB_IMG, NVN_RB_TBUF,
       NVN_RB_SBUFP, NVN_RB_SUPPORT, NVN_RB_C1, NVN_RB_DRV0, NVN_RB_DRV2, NVN_RB_SAMPT, NVN_RB_UBOP, NVN_RB_TEXU, NVN_RB_SAMPU };
// TEXU / SAMPU：無綁定貼圖 / 採樣器——句柄由遊戲寫在 uniform 槽 reg 的字節偏移 off 處（32 位：低 20 位貼圖號、高 12 位採樣器號），回放時現讀
// TEXU / SAMPU: bindless texture / sampler - the game writes the handle at byte offset off of uniform slot reg (32 bits: low 20 bits texture ID, high 12 bits sampler ID); read at replay time   // UBOP：計算階段 uniform 槽 r 的設備指針（Maxwell 計算階段經 c0 地址表 + 全局內存讀 uniform）   // SAMPT：採樣器取自貼圖槽 r 的合併句柄（高位 = 採樣器號）   // 綁定種類（json 的 type）；IMG = 可寫貼圖（D3D u_r，NVN 圖像槽 r）；TBUF = 類型化緩衝（D3D t_r Buffer<T>，NVN 貼圖槽 r 的緩衝貼圖） | UBOP: device pointer of compute-stage uniform slot r (the Maxwell compute stage reads uniforms via the c0 address table + global memory)   // SAMPT: the sampler is taken from the combined handle of texture slot r (high bits = sampler ID)   // Binding kind (type in the json); IMG = writable texture (D3D u_r, NVN image slot r); TBUF = typed buffer (D3D t_r Buffer<T>, buffer texture in NVN texture slot r)
enum { NVN_DPF_FLOAT = 0, NVN_DPF_UINT, NVN_DPF_SINT, NVN_DPF_DEPTH };            // 啞貼圖像素類（由 msl_type 定） | Dummy texture pixel class (determined by msl_type)
enum { NVN_CT_FLOAT = 0, NVN_CT_UINT, NVN_CT_INT };                               // 頂點輸入分量類型 | Vertex input component type
#define NVN_REFL_MAXB 64
#define NVN_REFL_MAXV 32
// 參數緩衝槽上限（數組綁定把一項攤成多槽：朋友收集 GameField 的像素著色器 32 張貼圖 + 32 個採樣器的數組，到 69 槽）
// Upper limit of argument buffer slots (an array binding spreads one entry over several slots: a Tomodachi Life GameField pixel shader has arrays of 32 textures + 32 samplers, reaching 69 slots)
#define NVN_REFL_MAXA 256

typedef struct {
    uint8_t kind, rw;        // rw：存儲緩衝 access 為 read_write / write / device | rw: storage buffer access is read_write / write / device
    uint8_t dtt, dpf;        // 貼圖槽未綁時的啞貼圖：MTLTextureType 數值、像素類 | Dummy texture when the texture slot is unbound: MTLTextureType value, pixel class
    uint16_t reg, aid;       // D3D 寄存器號、參數緩衝內序號 | D3D register number, index within the argument buffer
    uint16_t off;            // TEXU / SAMPU：句柄在 uniform 緩衝裡的字節偏移 | TEXU / SAMPU: byte offset of the handle within the uniform buffer
    uint16_t cnt;            // 數組綁定的元素數（非數組 1）：佔參數緩衝 aid … aid + cnt − 1 | element count of an array binding (1 if not an array): occupies argument buffer aid … aid + cnt − 1
} nvn_rbind;
typedef struct {
    int nb, abi, nv;         // 綁定數、參數緩衝槽、頂點輸入數 | Binding count, argument buffer slot, vertex input count
    nvn_rbind b[NVN_REFL_MAXB];
    struct { uint8_t loc, ct, nvn; } v[NVN_REFL_MAXV];   // loc：Metal 頂點屬性槽（DXBC 輸入寄存器）；nvn：NVN 頂點屬性號（HLSL 語義 ATTRn 的 n） | loc: Metal vertex attribute slot (DXBC input register); nvn: NVN vertex attribute number (the n of HLSL semantic ATTRn)
    uint16_t tg[3];          // 計算線程組大小（無 = 0） | Compute thread group size (none = 0)
    uint8_t route_b, has_c1; // B 路線；有 c1 常數段 | Route B; has a c1 constant segment
    uint8_t bindless;        // 有 TEXU / SAMPU 綁定（讀哪張貼圖要到回放時才知道） | Has TEXU / SAMPU bindings (which texture is read is known only at replay time)
    uint8_t drv_read[3];     // B：運行時讀驅動段 c0 / c2（下標 0、2；出包時由 MSL 靜態統計，tools/dmsh_build.py drv_reads） | B: reads driver segments c0 / c2 at runtime (indices 0, 2; gathered statically from the MSL at packaging time, tools/dmsh_build.py drv_reads)
    uint16_t c1_len;         // c1 常數段長度（B；全庫 0 / 256 / 512 / 768） | Length of the c1 constant segment (B; 0 / 256 / 512 / 768 across the whole library)
    // 全覆蓋首繪證明用（dmsh_build.py raster_facts；缺字段 = 不可證）：頂點——位置 xyz 原樣取自屬性 pos_attr（-1 = 否），w 為常數 1（pos_w1）或取同屬性 .w；
    // 片元——可能丟棄、寫 sample_mask、寫出的顏色號位圖
    // For the fully-covering-first-draw proof (dmsh_build.py raster_facts; missing field = not provable): vertex - position xyz taken as-is from attribute pos_attr (-1 = no), w is the constant 1 (pos_w1) or taken from the same attribute's .w;
    // fragment - may discard, writes sample_mask, bitmap of the color numbers written
    int8_t pos_attr; uint8_t pos_w1, may_discard, writes_smask, color_mask, color_known;   // color_known：出包時記下了顏色號列表（color_mask 可信） | color_known: the color list was recorded at packaging (color_mask can be trusted)
    // 合併 B 第 2 步（可讀 tile 版本 <鍵>_bf，dmsh_fetch.py）：片元——可改採樣 nfc 次（第 i 次：貼圖槽 fc_reg、插值輸出 fc_locn）、
    // 混用的貼圖槽位圖 fc_mixed（這些槽不得改讀 tile）、輸出 nfo 個（各自的顏色號 fo_color）；頂點——插值輸出 locn 的 .xy 原樣取自屬性 uv_attr[locn]（-1 = 否）
    // Merge B step 2 (readable-tile version <key>_bf, dmsh_fetch.py): fragment - nfc redirectable samples (the i-th: texture slot fc_reg, interpolated output fc_locn),
    // bitmap fc_mixed of texture slots with mixed use (these slots must not be redirected to read the tile), nfo outputs (each with color number fo_color); vertex - .xy of interpolated output locn is taken as-is from attribute uv_attr[locn] (-1 = no)
    uint8_t nfc, fc_reg[32], fc_locn[32], nfo, fo_color[4]; uint64_t fc_mixed[2]; int8_t uv_attr[32];
    // 頂點——插值輸出 locn 的 .x / .y = fma(屬性 uv_aff_attr[locn] 的 .x / .y, 縮放, 偏移)，縮放 / 偏移引用 uniform：(槽 << 8) | (向量 << 2) | 分量
    // Vertex - .x / .y of interpolated output locn = fma(.x / .y of attribute uv_aff_attr[locn], scale, offset), where scale / offset reference a uniform: (slot << 8) | (vector << 2) | component
    int8_t uv_aff_attr[32]; uint16_t uv_aff[32][4];   // [sx, ox, sy, oy]
    uint8_t c1[4096];        // c1 常數段原樣（B） | c1 constant segment as-is (B)
} nvn_refl;

#ifdef __OBJC__
#import <Foundation/Foundation.h>
// 成功 0；失敗返回非 0 並把原因寫進 err（未知綁定類型、超定長上限、字段缺失）
// 0 on success; on failure returns non-0 and writes the reason into err (unknown binding type, fixed-length limit exceeded, missing field)
int nvn_refl_from_json(NSDictionary *j, nvn_refl *out, char *err, int errlen);
#endif
