// Horizon 路徑大小寫單測：期望手寫；在默認不分大小寫的 APFS 上跑真實文件操作。
// 同名不同大小寫必須共存，所有路徑組件精確匹配，目錄列舉仍給客體名字，Commit/重掛保留名字與內容。
// Horizon path case unit test: expectations are hand-written; runs real file operations on the default case-insensitive APFS.
// Names differing only in case must coexist, all path components match exactly, directory listing still gives the guest names, and Commit/remount preserve names and content.
#ifndef HLE_FS_SOURCE
#define HLE_FS_SOURCE "../hle_fs.c"
#endif
#include <sys/xattr.h>
#include <errno.h>
static int fail_name_write;
static int fake_setxattr(const char *p, const char *name, const void *v, size_t n, u_int32_t pos, int opt) {
    if (fail_name_write) { errno = ENOSPC; return -1; }
    return setxattr(p, name, v, n, pos, opt);
}
#define setxattr fake_setxattr
#include HLE_FS_SOURCE
#undef setxattr
struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
void dm_prof_add(int id, uint64_t ns, uint64_t b) { (void)id; (void)ns; (void)b; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
static int checks, fails;
#define CHECK(c, text) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "✓" : "✗", text); } while (0)
static int content(const char *path, const char *want) {
    uint64_t h; if (h_fs_OpenFile(&h, path, MODE_READ)) return 0;
    char b[64] = {0}; size_t n = 0; Result r = h_fs_ReadFile(&n, h, 0, b, sizeof b);
    h_fs_CloseFile(h); return !r && n == strlen(want) && !memcmp(b, want, n);
}
static int write_text(const char *path, const char *text) {
    uint64_t h; if (h_fs_OpenFile(&h, path, MODE_WRITE)) return 0;
    Result r = h_fs_WriteFile(h, 0, text, strlen(text), NULL); h_fs_CloseFile(h); return !r;
}
static int listed(const char *path, const char *name, int type) {
    uint64_t h; if (h_fs_OpenDirectory(&h, path, 3)) return 0;
    DirectoryEntry e[32]; int64_t n = 0; int found = 0;
    h_fs_ReadDirectory(&n, e, h, 32); h_fs_CloseDirectory(h);
    for (int i = 0; i < n; i++) if (!strcmp(e[i].name, name) && e[i].type == type) found++;
    return found == 1;
}
static int entry_count(void) {
    uint64_t h; if (h_fs_OpenDirectory(&h, "save:/", 3)) return -1;
    DirectoryEntry e[32]; int64_t n = 0;
    h_fs_ReadDirectory(&n, e, h, 32); h_fs_CloseDirectory(h); return (int)n;
}
static void *create_race(void *p) { return (void *)(uintptr_t)h_fs_CreateFile(p, 0); }
module_t g_mod[MAX_MOD]; int g_nmod;   // 模塊表（MountSave 按 main 構建號挑存檔補丁）：這份單測不裝模塊 | Module table (MountSave picks save fixes by main build ID): this unit test loads no modules
int dm_custom_lang_explicit(void) { return -1; }   // 語言開關（dm_custom.c）：這份單測不涉及，當作玩家沒點過 | Language switch (dm_custom.c): not involved in this unit test, treated as the player never having tapped it
int main(void) {
    char root[1024], save[1024], p[1100];
    const char *tmp = getenv("TMPDIR");
    if (tmp) snprintf(root, sizeof root, "%s/case.XXXXXX", tmp);
    else snprintf(root, sizeof root, "%s%s/run/platform/case.XXXXXX", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    snprintf(save, sizeof save, "%s/save", root); mkdir(save, 0755);
    snprintf(p, sizeof p, "%s/Legacy", save); FILE *f = fopen(p, "w"); fputs("old", f); fclose(f);
    setenv("DM_SAVE", save, 1); CHECK(h_fs_MountSave("save", NULL) == RES_OK, "掛載原有存檔");
    uint64_t h = 0; int32_t type = -1;
    CHECK(content("save:/Legacy", "old"), "原有大小寫名字可讀");
    Result r = h_fs_OpenFile(&h, "save:/legacy", 1); if (!r) h_fs_CloseFile(h);
    CHECK(r == RES_PATH_NOT_FOUND, "錯誤大小寫 OpenFile → PathNotFound");
    CHECK(h_fs_GetEntryType(&type, "save:/legacy") == RES_PATH_NOT_FOUND, "錯誤大小寫 GetEntryType → PathNotFound");
    CHECK(h_fs_DeleteFile("save:/legacy") == RES_PATH_NOT_FOUND, "錯誤大小寫 DeleteFile → PathNotFound");
    CHECK(content("save:/Legacy", "old"), "錯名刪除不影響原文件");
    CHECK(h_fs_CreateFile("save:/legacy", 0) == RES_OK, "Legacy 與 legacy 可以共存");
    CHECK(write_text("save:/legacy", "lower"), "寫 legacy");
    CHECK(content("save:/Legacy", "old") && content("save:/legacy", "lower"), "兩個名字讀出獨立內容 old/lower");
    CHECK(h_fs_CreateFile("save:/legacy", 0) == RES_PATH_EXISTS, "精確重復創建 → PathExists");
    CHECK(listed("save:/", "Legacy", ENT_FILE) && listed("save:/", "legacy", ENT_FILE), "目錄列舉保留 Legacy/legacy");
    CHECK(h_fs_CreateDirectory("save:/Dir") == RES_OK, "創建 Dir");
    CHECK(h_fs_CreateFile("save:/dir/x", 0) == RES_PATH_NOT_FOUND, "錯誤大小寫中間目錄拒絕創建");
    CHECK(h_fs_CreateDirectory("save:/dir") == RES_OK, "Dir 與 dir 可以共存");
    CHECK(h_fs_CreateFile("save:/Dir/X", 0) == RES_OK && write_text("save:/Dir/X", "upper"), "Dir/X 獨立內容 upper");
    CHECK(h_fs_CreateFile("save:/dir/X", 0) == RES_OK && write_text("save:/dir/X", "lower"), "dir/X 獨立內容 lower");
    CHECK(content("save:/Dir/X", "upper") && content("save:/dir/X", "lower"), "中間目錄大小寫讀不混淆");
    r = h_fs_OpenDirectory(&h, "save:/DIR", 3); if (!r) h_fs_CloseDirectory(h);
    CHECK(r == RES_PATH_NOT_FOUND, "錯誤大小寫 OpenDirectory → PathNotFound");
    CHECK(h_fs_DeleteDirRec("save:/DIR") == RES_PATH_NOT_FOUND, "錯誤大小寫遞歸刪除 → PathNotFound");
    CHECK(listed("save:/", "Dir", ENT_DIR) && listed("save:/", "dir", ENT_DIR), "目錄列舉保留 Dir/dir");
    CHECK(h_fs_RenameFile("save:/Legacy", "save:/legacy") == RES_PATH_EXISTS, "改名到已存在精確名字 → PathExists");
    CHECK(h_fs_RenameFile("save:/LEGACY", "save:/other") == RES_PATH_NOT_FOUND, "錯名來源改名 → PathNotFound");
    CHECK(h_fs_RenameFile("save:/Legacy", "save:/LEGACY") == RES_OK, "僅改大小寫的改名成功");
    CHECK(content("save:/LEGACY", "old") && content("save:/legacy", "lower"), "改名不覆蓋另一個客體文件");
    CHECK(h_fs_GetEntryType(&type, "save:/Legacy") == RES_PATH_NOT_FOUND, "舊客體名字消失");
    CHECK(h_fs_Commit("save") == RES_OK, "提交兩個大小寫文件和目錄");
    g_nmnt = 0; CHECK(h_fs_MountSave("save", NULL) == RES_OK, "重新掛載已提交存檔");
    CHECK(content("save:/LEGACY", "old") && content("save:/legacy", "lower"), "提交重掛保留獨立名字/內容");
    CHECK(content("save:/Dir/X", "upper") && content("save:/dir/X", "lower"), "提交重掛保留獨立目錄");
    CHECK(listed("save:/", "LEGACY", ENT_FILE) && listed("save:/", "legacy", ENT_FILE), "提交重掛列舉不漏映射");
    CHECK(h_fs_DeleteFile("save:/legacy") == RES_OK && content("save:/LEGACY", "old"), "刪除映射文件不傷另一個名字");
    CHECK(h_fs_DeleteDirRec("save:/dir") == RES_OK && content("save:/Dir/X", "upper"), "刪除映射目錄不傷另一個目錄");
    CHECK(h_fs_CreateFile("save:/legacy", 0) == RES_OK && write_text("save:/legacy", "again"), "刪除後可重新創建同名文件");
    CHECK(h_fs_RenameFile("save:/legacy", "save:/plain") == RES_OK && content("save:/plain", "again"), "映射文件改到普通名字可讀");
    CHECK(listed("save:/", "plain", ENT_FILE) && !listed("save:/", "legacy", ENT_FILE), "改名更新列舉，舊名字消失");
    CHECK(mount("rom", save, 1) == RES_OK && content("rom:/LEGACY", "old"), "只讀掛載讀取映射名字");
    CHECK(h_fs_GetEntryType(&type, "rom:/Legacy") == RES_PATH_NOT_FOUND, "只讀掛載也按精確大小寫查找");
    CHECK(h_fs_CreateDirectory("save:/Trail/") == RES_OK && listed("save:/", "Trail", ENT_DIR), "創建目錄尾斜槓不變成空名字");
    CHECK(h_fs_CreateFile("save:/.dm-case-1", 0) == RES_OK && write_text("save:/.dm-case-1", "real"), "客體真實別名前綴名字可以創建");
    CHECK(content("save:/.dm-case-1", "real") && content("save:/LEGACY", "old"), "物理別名不冒充客體真實名字");
    CHECK(listed("save:/", ".dm-case-1", ENT_FILE), "客體真實別名前綴名字按原名列舉");
    CHECK(h_fs_CreateFile("save:/plain", 0) == RES_PATH_EXISTS, "普通名字重復創建仍拒絕");
    int before = entry_count(); fail_name_write = 1;
    CHECK(h_fs_CreateFile("save:/PLAIN", 0) == RES_FAIL, "別名元數據盤滿：創建文件返回失敗");
    CHECK(entry_count() == before && h_fs_GetEntryType(&type, "save:/PLAIN") == RES_PATH_NOT_FOUND, "失敗創建不遺留文件或客體名字");
    CHECK(h_fs_CreateDirectory("save:/trail") == RES_FAIL && entry_count() == before, "別名目錄元數據失敗：移除剛建目錄");
    CHECK(h_fs_RenameFile("save:/plain", "save:/PLAIN") == RES_FAIL, "改名元數據失敗返回失敗");
    CHECK(content("save:/plain", "again") && h_fs_GetEntryType(&type, "save:/PLAIN") == RES_PATH_NOT_FOUND && entry_count() == before, "失敗改名完整回退原名和內容");
    fail_name_write = 0;
    CHECK(h_fs_CreateFile("save:/PLAIN", -1) == RES_FAIL && entry_count() == before, "別名文件定長失敗不遺留無元數據物理名");
    CHECK(h_fs_CreateFile("save:/Ä", 0) == RES_OK && h_fs_CreateFile("save:/ä", 0) == RES_OK, "非 ASCII 大小寫同樣共存");
    CHECK(write_text("save:/Ä", "upper") && write_text("save:/ä", "lower") && content("save:/Ä", "upper") && content("save:/ä", "lower"), "非 ASCII 名字內容獨立");
    pthread_t t[2]; void *r0, *r1;
    pthread_create(&t[0], NULL, create_race, "save:/Race"); pthread_create(&t[1], NULL, create_race, "save:/race");
    pthread_join(t[0], &r0); pthread_join(t[1], &r1);
    CHECK(!r0 && !r1 && listed("save:/", "Race", ENT_FILE) && listed("save:/", "race", ENT_FILE), "併發創建大小寫兩個名字都獨立發佈");
    rm_tree(root);
    printf("== 大小寫 %d 項，失敗 %d ==\n", checks, fails); return fails != 0;
}
