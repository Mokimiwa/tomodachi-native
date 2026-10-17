// 多模塊裝載（扮演 Horizon 的 rtld）：映射、佔線程槽、門登記、跨模塊重定位、按名接管、段權限。
// 代碼區（原 text 與補丁區）一拷進來就設成只讀+執行，此後裝載器只寫 data：iOS 上這兩塊要作為簽名代碼段原樣內嵌。
// Multi-module loading (playing the role of Horizon's rtld): mapping, claiming thread slots, gate registration, cross-module relocation, take-over by name, segment permissions.
// The code area (original text and patch area) is set to read-only+execute as soon as it is copied in, and from then on the loader only writes data: on iOS these two blocks must be embedded as is as signed code segments.
#include "dm.h"
#include <TargetConditionals.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include "hle.h"

module_t g_mod[MAX_MOD];
int g_nmod;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
pthread_key_t g_keyres[2], g_keyaspect, g_keyaspectui;   // g_keyaspect：寬高比上限參數位（KASPECT）          // 分辨率參數位（改寫器 PARAMS，KRES0 掌機 / KRES1 主機）；產物沒有即 0、不佔 | g_keyaspect: aspect ratio upper-limit parameter slot (KASPECT)          // resolution parameter slots (rewriter PARAMS, KRES0 handheld / KRES1 docked); 0 and not allocated if the build output lacks them
enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_KRES0, KEY_KRES1, KEY_KASPECT, KEY_KASPECTUI, KEY_N };

uint64_t resolve_import(module_t *m, const char *name);
void gate_register(uint64_t ret_addr, const char *kind, int arg, uint64_t orig);

static pthread_key_t key_value(unsigned i) {
    switch (i) {
    case KEY_K18: return g_key18; case KEY_KTLS: return g_keytls; case KEY_KTLR: return g_keytlr;
    case KEY_KGATE: return g_keygate; case KEY_KRES0: return g_keyres[0]; case KEY_KRES1: return g_keyres[1];
    case KEY_KASPECT: return g_keyaspect; default: return g_keyaspectui;
    }
}
static void create_keys(void) {
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||
        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die("創建宿主鍵失敗");
    // 參數位鍵一律建（不按產物有沒有用到判斷：iOS 包裡沒有 params.tsv，按文件判斷會漏建、鍵頁寫成 0 → 遊戲讀到 TSD 0 號槽）
    // Parameter slot keys are always created (not decided by whether the build output uses them: an iOS app has no params.tsv, so deciding by file would miss creating them and the key page would be written as 0 → the game reads TSD slot 0)
    if (pthread_key_create(&g_keyres[0], NULL) || pthread_key_create(&g_keyres[1], NULL) ||
        pthread_key_create(&g_keyaspect, NULL) || pthread_key_create(&g_keyaspectui, NULL)) die("創建參數位鍵失敗");
    LOG("鍵號 K18=%lu KTLS=%lu KTLR=%lu KGATE=%lu KRES0=%lu KRES1=%lu KASPECT=%lu KASPECTUI=%lu",
        g_key18, g_keytls, g_keytlr, g_keygate, g_keyres[0], g_keyres[1], g_keyaspect, g_keyaspectui);
}

// K 槽校驗：pthread TSD 佈局固定，槽 = tpidrro_el0 + 8*key
// K slot check: the pthread TSD layout is fixed, slot = tpidrro_el0 + 8*key
static void tsd_check(void) {
    pthread_setspecific(g_key18, (void *)0x1234);
    uint64_t base; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);
    if (((uint64_t *)(base & ~7ull))[off / 8] != 0x1234) die("TSD 佈局不符");
    pthread_setspecific(g_key18, 0);
}

static void build_symhash(module_t *m);
static void parse_dynamic(module_t *m) {
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) die("%s: no MOD0", m->name);
    m->dyn = (const Elf64_Dyn *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym *)(m->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: m->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
    }
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym));
    build_symhash(m);
}

static void *module_sym_linear(module_t *m, const char *name) {
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (s->st_shndx && !strcmp(m->strtab + s->st_name, name)) return m->base + s->st_value;
    }
    return NULL;
}
// 符號名哈希表：按下標升序插入、同名已在則不覆蓋 ⇒ 查詢結果與線性掃描（首個已定義同名符號）逐條相同。
// ① 宿主裝載 91–99 ms 裡重定位 + 強制接管佔 77–86 ms，主因是每條按名解析的重定位都線性掃兩個模塊的符號表
// Symbol name hash table: inserted in ascending index order, not overwritten if the same name is already there ⇒ lookup results are identical, entry by entry, to a linear scan (the first defined symbol with that name).
// Of the 91–99 ms of ① host loading, relocation + forced take-over took 77–86 ms, mainly because every by-name relocation scanned the symbol tables of two modules linearly
static uint32_t sym_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static void build_symhash(module_t *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->symh = calloc(cap, sizeof *m->symh); m->symh_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i]; if (!s->st_shndx) continue;
        const char *nm = m->strtab + s->st_name;
        for (unsigned j = sym_hash(nm) & m->symh_mask;; j = (j + 1) & m->symh_mask) {
            if (!m->symh[j]) { m->symh[j] = i; break; }
            if (!strcmp(m->strtab + m->symtab[m->symh[j]].st_name, nm)) break;   // 同名已有更小下標：保留它 | The same name already exists with a smaller index: keep that one
        }
    }
}
static unsigned long g_symchk_n, g_symchk_bad;
void *module_sym(module_t *m, const char *name) {
    unsigned long nq; (void)nq;
    if (!m->symh) return module_sym_linear(m, name);
    void *r = NULL;
    for (unsigned j = sym_hash(name) & m->symh_mask; m->symh[j]; j = (j + 1) & m->symh_mask) {
        const Elf64_Sym *s = &m->symtab[m->symh[j]];
        if (!strcmp(m->strtab + s->st_name, name)) { r = m->base + s->st_value; break; }
    }
    static int chk = -1; if (chk < 0) chk = getenv("DM_SYMCHECK") != NULL;
    if (chk) { void *l = module_sym_linear(m, name); g_symchk_n++; nq = g_symchk_n;
               if (l != r) { g_symchk_bad++; LOG("【紅】DM_SYMCHECK：%s 在 %s 哈希 %p ≠ 線性 %p", name, m->name, r, l); } }
    return r;
}
void *guest_sym(const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return p; }
    return NULL;
}
// 補丁區在首址前的模塊（patch_off < 0）也認補丁區裡的地址，與補丁區在映像後的模塊一致（崩潰報告、GC 暫停判斷「在客體代碼裡」）
// For modules with the patch area before the base (patch_off < 0) addresses in the patch area count too, as for modules whose patch area follows the image (crash reports, the GC pause's "in guest code" test)
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++) {
        const uint8_t *b = g_mod[i].base;
        if ((const uint8_t *)p >= b && (const uint8_t *)p < b + g_mod[i].size) return &g_mod[i];
        if (g_mod[i].patch_off < 0 && (const uint8_t *)p >= b + g_mod[i].patch_off && (const uint8_t *)p < b + g_mod[i].patch_off + g_mod[i].patch_len) return &g_mod[i];
    }
    return NULL;
}

// 跨模塊符號解析：先在已裝模塊裡找定義，找不到再交給 HLE / 樁
// Cross-module symbol resolution: first look for a definition in the loaded modules, and only if not found hand over to HLE / stubs
static uint64_t resolve_sym(module_t *m, const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return (uint64_t)p; }
    return resolve_import(m, name);
}

// 強制接管表（hle.h HLE_FORCE，__DATA,swforce 段）：命中即覆蓋模塊內定義
// Forced take-over table (hle.h HLE_FORCE, the __DATA,swforce section): a hit overrides the definition inside the module
static const hle_force_ent *g_force; static unsigned g_nforce; static unsigned *g_force_hits;
static void force_init(void) {
    unsigned long sz; g_force = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &sz);
    g_nforce = g_force ? (unsigned)(sz / sizeof *g_force) : 0;
    g_force_hits = calloc(g_nforce + 1, sizeof *g_force_hits);
    for (unsigned i = 0; i < g_nforce; i++)
        for (unsigned j = i + 1; j < g_nforce; j++)
            if (!strcmp(g_force[i].name, g_force[j].name)) die("強制接管清單重復：%s", g_force[i].name);
}
static void *force_lookup(const char *name) {
    for (unsigned i = 0; i < g_nforce; i++)
        if (!strcmp(g_force[i].name, name)) {
            if (!g_force_hits[i]++ && g_flag.imports) LOG("force %s  （%s）", name, g_force[i].why);
            void *hlecount_wrap(const char *, const char *, void *); return hlecount_wrap("FORCE", name, g_force[i].fn);   // DM_HLECOUNT 時為計數樁 | A counting stub when DM_HLECOUNT is set
        }
    return NULL;
}
// 通用宿主：清單服務多個遊戲，某作沒引用的條目只記一筆；DM_FORCE_STRICT=1 時照舊報紅中止（核對清單寫錯用）
// Generic host: the list serves several games, so entries a game does not reference are only noted; with DM_FORCE_STRICT=1 it still fails loudly (for checking a mistyped list)
static void force_check(void) {
    unsigned unused = 0; int strict = getenv("DM_FORCE_STRICT") != NULL;
    for (unsigned i = 0; i < g_nforce; i++)
        if (!g_force_hits[i]) { if (strict || g_flag.imports) LOG("強制接管條目無任何引用：%s", g_force[i].name); unused++; }
    if (unused && strict) die("強制接管清單有誤");
    LOG("force: %u 條命中，%u 條本作沒引用", g_nforce - unused, unused);
}

// 讀改寫器表頭（段邊界、槽號）；表頭以外有任何行即舊版產物（裝載期改代碼），拒絕
// Read the rewriter table header (segment boundaries, slot numbers); any row beyond the header means an old-format build output (code modified at load time), which is rejected
static void read_layout(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    static const char *kn[8] = {"K18", "KTLS", "KTLR", "KGATE", "KRES0", "KRES1", "KASPECT", "KASPECTUI"};
    char line[256], key[32]; unsigned long v; unsigned seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %31s %lx", key, &v) != 2) die("%s: 產物是舊改寫器出的，換成當前代號（dm/PRODUCT）的產物", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = strtoll(line + 2 + strlen(key), NULL, 16);   // 可為負（補丁區在首址前） | may be negative (patch area before the base)
        else if (!strcmp(key, "IMAGE_SIZE")) m->image_size = v;
        else if (!strcmp(key, "FILE_IMAGE_LEN")) m->file_image_len = v;
        else if (!strcmp(key, "KEY_PAGE_OFF")) m->key_page_off = v;
        else if (!strcmp(key, "KEY_PAGE_SIZE")) m->key_page_size = v;
        else if (!strcmp(key, "TEXT_END")) m->text_end = v;
        else if (!strcmp(key, "RO_OFF")) m->ro_off = v;
        else if (!strcmp(key, "DATA_OFF")) m->data_off = v;
        else if (!strcmp(key, "BUILD_ID")) sscanf(line, "# %*s %32s", m->build_id);
        else for (int k = 0; k < 8; k++) {
            size_t n = strlen(kn[k]);
            if (!strncmp(key, "KEY_", 4) && !strncmp(key + 4, kn[k], n) && !strcmp(key + 4 + n, "_IDX")) {
                m->key_idx[k] = (unsigned)strtoul(strrchr(line, '\t') + 1, 0, 10); seen |= 1u << k;
            }
        }
    }
    fclose(f);
    if (!m->patch_off || m->key_page_off % 0x4000 || m->key_page_size != 0x4000 || seen != 0xff)
        die("%s: 產物是舊改寫器出的，換成當前代號（dm/PRODUCT）的產物", path);
    if (m->patch_off < 0 && (!m->image_size || !m->file_image_len || m->file_image_len % 0x4000 || m->patch_off % 0x4000 || m->key_page_off + m->key_page_size > m->image_size))
        die("%s: 補丁區在首址前，但 IMAGE_SIZE / FILE_IMAGE_LEN 缺失或不對 | patch area before the base, but IMAGE_SIZE / FILE_IMAGE_LEN missing or wrong", path);
}

static void install_key_page(module_t *m) {
    if (m->key_page_off + m->key_page_size > m->size) die("%s: 鍵頁越界", m->name);
    uint64_t *page = (uint64_t *)(m->base + m->key_page_off);
    memset(page, 0, m->key_page_size);
    for (unsigned i = 0; i < 4; i++) page[m->key_idx[i]] = (uint64_t)key_value(i) * 8;
    page[m->key_idx[KEY_KRES0]] = (uint64_t)g_keyres[0] * 8; page[m->key_idx[KEY_KRES1]] = (uint64_t)g_keyres[1] * 8;
    page[m->key_idx[KEY_KASPECT]] = (uint64_t)g_keyaspect * 8; page[m->key_idx[KEY_KASPECTUI]] = (uint64_t)g_keyaspectui * 8;
    for (int k = 0; k < 8; k++) if (!page[m->key_idx[k]]) die("%s: 鍵頁第 %d 項為 0（宿主鍵沒建）——遊戲會讀到 TSD 0 號槽", m->name, k);
    if (mprotect(m->base + m->key_page_off, m->key_page_size, PROT_READ)) die("%s 鍵頁 mprotect", m->name);
}

static void register_gates(module_t *m, const char *dir) {
    char path[1024], line[256]; snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    int ng = 0;
    if (f) {
        unsigned long ret, orig; char kind[16]; int arg;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%lx %15s %d %lx", &ret, kind, &arg, &orig) == 4) { gate_register((uint64_t)m->base + ret, kind, arg, orig); ng++; }
        fclose(f);
    }
    LOG("%s: gates %d patch_off %#llx keys %lu/%lu/%lu/%lu", m->name, ng, m->patch_off, g_key18, g_keytls, g_keytlr, g_keygate);
}

// DT_RELR 壓縮的相對重定位：SDK 20 的模塊（朋友收集 main / sdk）把全部相對重定位放在這裡，RELA 裡一條 RELATIVE 都沒有；
// 不處理時數據裡的指針都還是模塊內偏移，main 的全局構造讀 0x36ad628 這種小地址就崩。
// 編碼：偶數項 = 一個槽的偏移，下一位置 = 它 + 8；奇數項 = 位圖，第 k 位（k = 1..63）管 位置 + 8 (k − 1)，之後位置前進 63 × 8。槽值原地帶加數：槽 += 基址
// DT_RELR packed relative relocations: SDK 20 modules (Tomodachi Life main / sdk) keep all their relative relocations here, with not one RELATIVE in RELA;
// left unprocessed, pointers in data are still module offsets, and main's global constructors crash reading small addresses like 0x36ad628.
// Encoding: an even entry = the offset of one slot, and the next position = it + 8; an odd entry = a bitmap whose bit k (k = 1..63) covers position + 8 (k − 1), after which the position advances by 63 × 8. The slot holds its addend in place: slot += base
static size_t relr_walk(const module_t *m, void (*fn)(const module_t *, uint64_t off, void *), void *ctx) {
    uint64_t relr = 0, relrsz = 0;
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) {
        if (d->d_tag == DT_RELR) relr = d->d_un.d_ptr;
        else if (d->d_tag == DT_RELRSZ) relrsz = d->d_un.d_val;
    }
    if (!relr || !relrsz) return 0;
    const uint64_t *e = (const uint64_t *)(m->base + relr);
    size_t n = 0; uint64_t at = 0;
    for (uint64_t i = 0; i < relrsz / 8; i++) {
        uint64_t v = e[i];
        if (!(v & 1)) { fn(m, v, ctx); n++; at = v + 8; continue; }
        for (int k = 1; k < 64; k++) if (v >> k & 1) { fn(m, at + 8 * (uint64_t)(k - 1), ctx); n++; }
        at += 63 * 8;
    }
    return n;
}
static void relr_apply(const module_t *m, uint64_t off, void *ctx) { (void)ctx; *(uint64_t *)(m->base + off) += (uint64_t)m->base; }

static void relocate(module_t *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
    }
    int cnt[3] = {0}, nint = 0;
    const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
    uint64_t sizes[2] = {relasz, pltsz};
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
            const Elf64_Rela *r = &tabs[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
            if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)m->base + r->r_addend; cnt[0]++; continue; }
            const Elf64_Sym *s = &m->symtab[si];
            uint64_t v = (uint64_t)force_lookup(m->strtab + s->st_name);
            // 本模塊自己有定義的符號也按裝載順序全局查（ELF 缺省語義，Horizon 的 rtld 同樣）：先裝的模塊的定義覆蓋後裝的，局部 / 非缺省可見性的才綁自己。
            // Engage 的 main 自定義 operator new / delete（Unity 的內存管理器），sdk 也有一份（轉 malloc / free）：原先 sdk 綁自己的 delete，
            // main 的 new 出來的塊被 sdk 裡 libc++ 的 string 釋放進 SDK 堆，CentralHeap::Free 報 NotNMallocPtr
            // Symbols the module defines itself are also looked up globally in load order (the ELF default; Horizon's rtld does the same): the definition of an earlier-loaded module overrides a later one; only local / non-default visibility binds to itself.
            // Engage's main defines its own operator new / delete (Unity's memory manager) and sdk has a copy too (forwarding to malloc / free): previously sdk bound its own delete,
            // so blocks from main's new were freed by sdk's libc++ string code into the SDK heap, and CentralHeap::Free reported NotNMallocPtr
            if (!v && s->st_shndx) {
                uint64_t self = (uint64_t)m->base + s->st_value;
                v = ELF64_ST_BIND(s->st_info) == STB_LOCAL || ELF64_ST_VISIBILITY(s->st_other) != STV_DEFAULT ? self : resolve_sym(m, m->strtab + s->st_name);
                if (!v) v = self;
                if (v != self && nint++ < 8) LOG("%s: %s 綁到先裝模塊的定義（%s） | bound to an earlier module's definition", m->name, m->strtab + s->st_name, module_of((void *)v) ? module_of((void *)v)->name : "?");
            }
            if (!v) v = resolve_sym(m, m->strtab + s->st_name);
            if (type == R_AARCH64_JUMP_SLOT) { *where = v; cnt[2]++; }
            else { *where = v + r->r_addend; cnt[1]++; }       // ABS64 / GLOB_DAT
        }
    cnt[0] += (int)relr_walk(m, relr_apply, NULL);
    LOG("%s: reloc relative=%d abs/glob=%d jump=%d%s", m->name, cnt[0], cnt[1], cnt[2], nint ? "（含綁到先裝模塊的定義） | (some bound to an earlier module's definition)" : "");
    if (nint > 8) LOG("%s: 共 %d 處綁到先裝模塊的定義 | %d bound to an earlier module's definition in all", m->name, nint, nint);
}

// DM_LOADHASH=1（診斷）：裝載完逐模塊出地址無關 sha256，證「提前起跑 / 不提前」裝載結果逐字節相同。
// 模塊基址與宿主 ASLR 每局不同，只有重定位槽的值隨之變：複製映像，按同一張重定位表把每個槽換成與地址無關的記號——
// RELATIVE → (0, 加數)；符號目標落在某模塊 → (1 + 模塊號, 模塊內偏移)；否則（宿主強制接管 / 宿主符號）→ (名字 FNV, 加數)；其餘字節原樣。
// DM_LOADHASH=1 (diagnostics): after loading, output an address-independent sha256 per module, to prove that the load results of "early start / no early start" are byte-identical.
// Module base addresses and host ASLR differ every run, and only the values of the relocation slots change with them: copy the image and, using the same relocation table, replace every slot with an address-independent token --
// RELATIVE → (0, addend); symbol target falls in some module → (1 + module number, offset within the module); otherwise (host forced take-over / host symbol) → (name FNV, addend); all other bytes as is.
#include <CommonCrypto/CommonDigest.h>
static void relr_hash_head(const module_t *m, uint64_t off, void *cp) { (void)m; memset((uint8_t *)cp + off, 0, 8); }
static void relr_hash_tail(const module_t *m, uint64_t off, void *c) {
    uint64_t tail = *(const uint64_t *)(m->base + off) - (uint64_t)m->base;
    CC_SHA256_Update((CC_SHA256_CTX *)c, &tail, 8);
}
void loader_hash_log(void) {
    if (!getenv("DM_LOADHASH")) return;
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        uint8_t *cp = malloc(m->size); memcpy(cp, m->base, m->size);
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
        for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
            case DT_RELA: rela = d->d_un.d_ptr; break; case DT_RELASZ: relasz = d->d_un.d_val; break;
            case DT_JMPREL: jmprel = d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        }
        const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
        uint64_t sizes[2] = {relasz, pltsz}; unsigned nslot = 0;
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tok[1] = {0};
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset); int hit = 0;
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tok[0] = 1 + j; hit = 1; break; }
                    if (!hit) { const char *nm = m->strtab + m->symtab[ELF64_R_SYM(r->r_info)].st_name; uint64_t h = 0xcbf29ce484222325ull;
                                for (; *nm; nm++) h = (h ^ (uint8_t)*nm) * 0x100000001b3ull; tok[0] = h | 1ull << 63; }
                }
                memcpy(cp + r->r_offset, &tok[0], 8);                // 槽 8 字節換成記號頭；記號尾在下面按槽序追加 | The 8 slot bytes are replaced by the token head; the token tail is appended below in slot order
                nslot++;
            }
        nslot += (unsigned)relr_walk(m, relr_hash_head, cp);         // DT_RELR 槽同 RELATIVE：記號頭 0 | DT_RELR slots like RELATIVE: token head 0
        CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, cp, (CC_LONG)m->size);
        relr_walk(m, relr_hash_tail, &c);                             // 記號尾 = 槽值 − 基址 | token tail = slot value − base
        for (int t = 0; t < 2; t++)                                // 記號尾（加數 / 偏移）按槽序追加，與槽頭一起唯一確定槽值 | Token tails (addend / offset) are appended in slot order, and together with the slot head uniquely determine the slot value
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tail = (uint64_t)r->r_addend;
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset);
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tail = v - (uint64_t)g_mod[j].base; break; }
                }
                CC_SHA256_Update(&c, &tail, 8);
            }
        uint8_t d[32]; CC_SHA256_Final(d, &c); free(cp);
        char hx[65]; for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        LOG("DM_LOADHASH %s 大小 %#zx 重定位槽 %u sha256 %s", m->name, m->size, nslot, hx);
    }
}

// 可選剪口的選項名表（apply_options 填；iOS 不填，恆為空） | option names of the optional cuts (filled by apply_options; not filled on iOS, always empty)
static char g_opt_known[16][32] __attribute__((unused)); static int g_nopt_known;
int dm_option_known(const char *name) { for (int i = 0; i < g_nopt_known; i++) if (!strcmp(g_opt_known[i], name)) return 1; return 0; }
#if TARGET_OS_IPHONE
// iOS 不許運行時裝入的代碼執行，且蘋果只許可執行文件有一個帶執行權限的段（ITMS-90999）：映像由 dm/ios/tools/gen_embed.py 編進本可執行文件——
// 代碼兩塊（原 text、補丁區）是 __TEXT 裡的節，W（ro 餘部 + data + bss）是單獨的 RW 段。三塊在文件裡不相鄰；這裡保留一塊連續地址，
// 按原相對位置 vm_remap 回去：代碼頁 copy=FALSE（與 __TEXT 共享同一批已簽名頁，r-x），W copy=TRUE（寫時複製，用到才分頁）。
// 運行時佈局與原映像逐字節相同。iOS 上把已簽名頁 remap 到新地址再執行，libffi 的跳板表（FFI_EXEC_TRAMPOLINE_TABLE）就是這麼做的
// iOS does not allow code loaded at run time to execute, and Apple allows an executable only one segment with execute permission (ITMS-90999): the image is compiled into this executable by dm/ios/tools/gen_embed.py --
// the two code blocks (original text, patch area) are sections in __TEXT, and W (the rest of ro + data + bss) is a separate RW segment. The three blocks are not adjacent in the file; here one contiguous address range is reserved,
// and they are vm_remap'ed back at their original relative positions: code pages with copy=FALSE (sharing the same signed pages with __TEXT, r-x), W with copy=TRUE (copy-on-write, paged only when used).
// The run-time layout is byte-identical to the original image. Remapping signed pages to a new address and then executing them on iOS is exactly what libffi's trampoline table (FFI_EXEC_TRAMPOLINE_TABLE) does
#include <mach/mach.h>
static void remap_at(module_t *m, vm_address_t to, const void *from, unsigned long size, boolean_t copy, vm_prot_t need) {
    vm_address_t a = to; vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, copy, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || a != to) die("%s vm_remap %p → %#lx（%#lx 字節）失敗：%d", m->name, from, (unsigned long)to, size, kr);
    if ((cur & need) != need) die("%s vm_remap 後權限 %#x，缺 %#x", m->name, cur, need);
}
#ifdef DM_SHELL
// 殼（ios/tools/shell_build.sh 出的、不含任何遊戲內容的 App）：模塊映像不在主可執行文件裡，在使用者自己用遊戲文件做出來的
// Frameworks/Guest.framework/Guest 裡——一個只裝段的動態庫（dm/tools/guest_dylib.py 寫出），節名與內嵌做法相同，下面只換一個映像頭去取。
// Shell (the App built by ios/tools/shell_build.sh, containing no game content at all): the module images are not in the main executable but in the
// Frameworks/Guest.framework/Guest that users build themselves from their game files -- a dynamic library that holds only segments (written by dm/tools/guest_dylib.py), with the same section names as the embedded approach; below only the image header to fetch from is swapped.
#include <dlfcn.h>
#include <mach-o/dyld.h>
static const struct mach_header_64 *image_mh(void) {
    static const struct mach_header_64 *mh; if (mh) return mh;
    char exe[4096]; uint32_t n = sizeof exe; if (_NSGetExecutablePath(exe, &n)) die("取不到可執行文件路徑");
    char *sl = strrchr(exe, '/'); if (!sl || (size_t)(sl - exe) + 40 > sizeof exe) die("可執行文件路徑不對：%s", exe); strcpy(sl, "/Frameworks/Guest.framework/Guest");
    if (!dlopen(exe, RTLD_NOW)) die("這個安裝包裡還沒有遊戲程序（%s）：先用 dm/tools/d2rm_pack.py 把你自己的遊戲文件做進安裝包再裝。系統的說法：%s", exe, dlerror());
    for (uint32_t i = 0; i < _dyld_image_count(); i++) if (strstr(_dyld_get_image_name(i), "/Guest.framework/Guest")) mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
    if (!mh) die("Guest 動態庫已加載但找不到它的映像頭");
    fprintf(stderr, "boot: 模塊映像取自 Guest 動態庫 %s\n", exe); return mh;
}
#define IMAGE_MH image_mh()
#else
#define IMAGE_MH (&_mh_execute_header)
#endif
static void map_image(module_t *m, const char *dir) {
    (void)dir;
    char sx0[17], sx1[17], dm[17]; snprintf(sx0, sizeof sx0, "__dm%sx0", m->name); snprintf(sx1, sizeof sx1, "__dm%sx1", m->name); snprintf(dm, sizeof dm, "__DM%sW", m->name);
    unsigned long z0, zw, z1;
    const uint8_t *x0 = getsectiondata(IMAGE_MH, "__TEXT", sx0, &z0), *w = getsectiondata(IMAGE_MH, dm, "__img", &zw), *x1 = getsectiondata(IMAGE_MH, "__TEXT", sx1, &z1);
    if (!x0 || !w || !x1) die("內嵌節 %s / %s / %s 缺失", sx0, dm, sx1);
    // 補丁區在首址前（patch_off < 0）：W 只到 data 尾（FILE_IMAGE_LEN），bss 與鍵頁是 vm_allocate 給的零頁，不進可執行文件
    // Patch area before the base (patch_off < 0): W ends at the end of data (FILE_IMAGE_LEN); bss and the key page are zero pages from vm_allocate and are not in the executable
    int front = m->patch_off < 0;
    if (z0 + zw != (front ? m->file_image_len : (uint64_t)m->patch_off) || (z0 & 0x3FFF) || (zw & 0x3FFF) || (z1 & 0x3FFF) || ((uintptr_t)x0 & 0x3FFF) || ((uintptr_t)w & 0x3FFF) || ((uintptr_t)x1 & 0x3FFF))
        die("%s 內嵌節佈局不符（x0 %#lx w %#lx x1 %#lx，PATCH_OFF %#llx）", m->name, z0, zw, z1, (unsigned long long)m->patch_off);
    m->size = front ? m->image_size : m->patch_off + z1;
    uint64_t pre = front ? (uint64_t)-m->patch_off : 0;
    if (front) m->patch_len = z1;
    vm_address_t base = 0;
    if (vm_allocate(mach_task_self(), &base, pre + m->size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) die("%s 保留 %#llx 字節地址失敗", m->name, (unsigned long long)(pre + m->size));
    base += pre;
    remap_at(m, base, x0, z0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    remap_at(m, base + z0, w, zw, TRUE, VM_PROT_READ | VM_PROT_WRITE);
    remap_at(m, base + m->patch_off, x1, z1, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    m->base = (uint8_t *)base; install_key_page(m);
}
#else
static uint8_t *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open %s", path);
    struct stat st; fstat(fd, &st);
    uint8_t *p = malloc(st.st_size);
    if (read(fd, p, st.st_size) != st.st_size) die("read %s", path);
    close(fd); *n = st.st_size; return p;
}

// 拷入匿名頁後立即封代碼區（頁 16 KB）：[0, text_end) 與補丁區只讀+執行，此後對代碼區的任何寫入都會當場崩；
// 之間（ro 與 data 同頁只能一起）可寫
// Seal the code area immediately after copying into anonymous pages (16 KB pages): [0, text_end) and the patch area are read-only+execute, and from then on any write to the code area crashes on the spot;
// what lies between (ro and data on the same page can only go together) is writable
// 可選剪口（patch_nso.py OPTIONS → <名>.options.tsv，列：選項 偏移 原字節 新字節 理由）：DM_OPTIONS=逗號分隔的選項名，在代碼區封頁之前套上，
// 每條先核原字節（不符報紅、不套）。表裡出現過的選項名都記下，前端據此決定顯示哪些開關（dm_option_known）
// Optional cuts (patch_nso.py OPTIONS -> <name>.options.tsv, columns: option, offset, original bytes, new bytes, reason): DM_OPTIONS = comma-separated option names, applied before the code area is sealed,
// each one checking the original bytes first (a mismatch is reported and not applied). Every option name seen in the table is remembered, and the front end shows only those switches (dm_option_known)
static int option_wanted(const char *name) {
    const char *e = getenv("DM_OPTIONS"); size_t n = strlen(name);
    for (const char *q = e; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) if (!strncmp(q, name, n) && (q[n] == ',' || !q[n])) return 1;
    return 0;
}
static int hexbytes(const char *h, uint8_t *out, size_t cap) {
    size_t n = strlen(h); if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) { unsigned v; if (sscanf(h + 2 * i, "%2x", &v) != 1) return -1; out[i] = (uint8_t)v; }
    return (int)(n / 2);
}
static void apply_options(module_t *m, const char *dir) {
    char path[1024], line[1024]; snprintf(path, sizeof path, "%s/%s.options.tsv", dir, m->name);
    FILE *f = fopen(path, "r"); if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char name[32], ob[64], nb[64]; unsigned long long off; uint8_t o[32], w[32];
        if (line[0] == '#' || sscanf(line, "%31[^\t]\t%llx\t%63[^\t]\t%63[^\t\n]", name, &off, ob, nb) != 4) continue;
        if (!dm_option_known(name) && g_nopt_known < 16) strlcpy(g_opt_known[g_nopt_known++], name, sizeof g_opt_known[0]);
        if (!option_wanted(name)) continue;
        int n = hexbytes(ob, o, sizeof o);
        if (n <= 0 || hexbytes(nb, w, sizeof w) != n || off + (unsigned)n > m->size) { LOG("【紅】可選剪口 %s：%s+%#llx 這一行格式不對，不套", name, m->name, off); continue; }
        if (memcmp(m->base + off, o, (size_t)n)) { LOG("【紅】可選剪口 %s：%s+%#llx 原字節不符（映像與表不是同一次出包？），不套", name, m->name, off); continue; }
        memcpy(m->base + off, w, (size_t)n);
        LOG("可選剪口 %s：%s+%#llx 已套", name, m->name, off);
    }
    fclose(f);
}
static void map_image(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    size_t sz; uint8_t *raw = slurp(path, &sz);
    if (m->patch_off < 0) {
        // 補丁區在首址前：文件 = 映像前段（text + ro + data）+ 補丁區；bss 與鍵頁是匿名零頁。補丁區尾到首址之間不映射（PROT_NONE）（上游 a9c56fd）
        // Patch area before the base: file = the first part of the image (text + ro + data) + patch area; bss and the key page are anonymous zero pages. The gap from the patch area's end to the base is left inaccessible (PROT_NONE) (upstream a9c56fd)
        uint64_t pre = (uint64_t)-m->patch_off, fl = m->file_image_len;
        if (sz < fl) die("%s 比 FILE_IMAGE_LEN %#llx 短", path, (unsigned long long)fl);
        m->size = m->image_size; m->patch_len = (sz - fl + 0x3FFF) & ~0x3FFFull;
        if (m->patch_len > pre) die("%s 補丁區 %#llx 字節放不進首址前的 %#llx", path, (unsigned long long)m->patch_len, (unsigned long long)pre);
        uint8_t *a = mmap(NULL, pre + m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (a == MAP_FAILED) die("%s mmap %#llx", m->name, (unsigned long long)(pre + m->size));
        m->base = a + pre;
        memcpy(m->base, raw, fl); memcpy(a, raw + fl, sz - fl); free(raw); install_key_page(m);
        apply_options(m, dir);   // 封頁之前 | before sealing
        uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
        if (mprotect(m->base, rx, PROT_READ | PROT_EXEC)) die("%s mprotect text", m->name);
        if (mprotect(a, m->patch_len, PROT_READ | PROT_EXEC)) die("%s mprotect patch", m->name);
        if (pre > m->patch_len && mprotect(a + m->patch_len, pre - m->patch_len, PROT_NONE)) die("%s mprotect gap", m->name);
        return;
    }
    m->size = (sz + 0x3FFF) & ~0x3FFFull;
    m->base = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m->base == MAP_FAILED) die("%s mmap", m->name);
    memcpy(m->base, raw, sz); free(raw); install_key_page(m);
    apply_options(m, dir);   // 封頁之前 | before sealing
    uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
    if (mprotect(m->base, rx, PROT_READ | PROT_EXEC)) die("%s mprotect text", m->name);
    uint64_t po = m->patch_off & ~0x3FFFull;
    if (mprotect(m->base + po, m->size - po, PROT_READ | PROT_EXEC)) die("%s mprotect patch", m->name);
}
#endif

void load_modules(const char *dir, const char **names, int n) {
    // 先全部映射並解析符號表，再統一重定位（跨模塊符號要先都在位）
    // DM_BOOTPROF：① 宿主裝載細分（毫秒），定 ③ 預重定位 / mmap 的上限
    // Map everything and parse the symbol tables first, then relocate all at once (cross-module symbols must all be in place first)
    // DM_BOOTPROF: breakdown of ① host loading (milliseconds), to set the upper bound for ③ pre-relocation / mmap
    uint64_t t0 = now_ns(), tm = 0, tp = 0, tg = 0, tr = 0, t;
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        strlcpy(m->name, names[i], sizeof m->name);
        read_layout(m, dir);
    }
    create_keys();
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        t = now_ns(); map_image(m, dir); tm += now_ns() - t;   // 返回時代碼區已是只讀+執行，此後裝載器只寫 data | On return the code area is already read-only+execute; from here on the loader only writes data
        if (!i) tsd_check();
        t = now_ns(); parse_dynamic(m); tp += now_ns() - t;
        t = now_ns(); register_gates(m, dir); tg += now_ns() - t;
        LOG("%s: base %p size %#zx", names[i], m->base, m->size);
        g_nmod++;
    }
    force_init();
    t = now_ns(); for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]); tr = now_ns() - t;
    force_check();
    { void hlecount_seal(void); hlecount_seal(); }   // DM_HLECOUNT：重定位完成，計數樁頁封成只讀可執行 | DM_HLECOUNT: relocation done, seal the counting stub pages as read-only executable
    if (getenv("DM_SYMCHECK")) LOG("DM_SYMCHECK：裝載期共核 %lu 次符號查詢，哈希與線性不一致 %lu", g_symchk_n, g_symchk_bad);
    if (g_flag.bootprof) LOG("① 細分：映射（讀 + 拷 + 封頁）%.1f ms、解析動態段 %.1f ms、門註冊 %.1f ms、重定位 + 強制接管 %.1f ms，合計 %.1f ms",
                             tm / 1e6, tp / 1e6, tg / 1e6, tr / 1e6, (now_ns() - t0) / 1e6);
}

// 扮演 rtld 的 InitModules（rtld+0x830）：按裝載鏈表正序調各模塊 DT_INIT。
// 時機由 nn::init::Start 決定：nninitStartup 與 _init_libc2 之後、nnMain 之前（Start 的第 4 個參數回調）。
// Plays the role of rtld's InitModules (rtld+0x830): call each module's DT_INIT in forward order of the load list.
// The timing is decided by nn::init::Start: after nninitStartup and _init_libc2, before nnMain (the 4th argument callback of Start).
void run_module_inits(void) {
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->init) continue;
        LOG("%s: DT_INIT +%#llx（init_array %llu 項）", m->name, m->init, m->init_arraysz / 8);
        ((void (*)(void))(m->base + m->init))();
    }
}
