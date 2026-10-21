/* UTF-8 file API, pread/pwrite, mmap_ro, locks, listing, user dirs. */
#include "host_fs.h"
#include "test_common.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static int count_cb(const char *name, int is_dir, void *ctx) {
    int *n = (int *)ctx;
    if (!strcmp(name, "f\xC3\xBC.bin") && !is_dir) n[0]++;
    if (!strcmp(name, "sub") && is_dir) n[1]++;
    return 0;
}

int main(void) {
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    char root[1024], path[1200], path2[1200], sub[1200];
    char sep = host_fs_sep();
    /* "hostfs_tést_日本" in UTF-8 */
    snprintf(root, sizeof root, "%s%chostfs_t\xC3\xA9st_\xE6\x97\xA5\xE6\x9C\xAC", tmp, sep);
    snprintf(sub, sizeof sub, "%s%csub%cdeeper", root, sep, sep);
    snprintf(path, sizeof path, "%s%cf\xC3\xBC.bin", root, sep);
    snprintf(path2, sizeof path2, "%s%crenamed.bin", root, sep);

    REQUIRE(host_fs_mkdirs(sub) == 0);
    host_stat st;
    CHECK(host_fs_stat(sub, &st) == 0 && st.exists && st.is_dir);

    host_file *f = host_fs_open(path, HOST_FS_READ | HOST_FS_WRITE | HOST_FS_CREATE | HOST_FS_TRUNC);
    REQUIRE(f != NULL);
    char data[100000];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (char)(i * 7);
    CHECK(host_fs_write(f, data, sizeof data) == (int64_t)sizeof data);
    CHECK(host_fs_size(f) == (int64_t)sizeof data);
    CHECK(host_fs_pwrite(f, "XYZ", 3, 70000) == 3);
    char rb[16];
    CHECK(host_fs_pread(f, rb, 5, 69999) == 5 && rb[1] == 'X' && rb[3] == 'Z' && rb[0] == data[69999]);
    CHECK(host_fs_pread(f, rb, 16, sizeof data - 4) == 4); /* short read at EOF */
    CHECK(host_fs_flush(f) == 0);

    /* Locks: exclusive on one handle blocks a non-waiting exclusive on another. */
    host_file *g = host_fs_open(path, HOST_FS_READ);
    REQUIRE(g != NULL);
    CHECK(host_fs_lock(f, 1, 0) == 0);
    CHECK(host_fs_lock(g, 1, 0) == -1);
    CHECK(host_fs_unlock(f) == 0);
    CHECK(host_fs_lock(g, 1, 0) == 0);
    CHECK(host_fs_unlock(g) == 0);

    /* Read-only mapping at an unaligned offset. */
    host_mapping m;
    const char *mp = (const char *)host_fs_mmap_ro(g, 69990, 100, &m);
    CHECK(mp != NULL);
    if (mp) { CHECK(mp[10] == 'X' && mp[0] == data[69990] && m.size == 100); host_fs_munmap(&m); }
    CHECK(host_fs_read(g, rb, 4) == 4 && rb[3] == data[3]);
    host_fs_close(g);

    CHECK(host_fs_truncate(f, 10) == 0 && host_fs_size(f) == 10);
    host_fs_close(f);

    CHECK(host_fs_open(path, HOST_FS_READ | HOST_FS_CREATE | HOST_FS_EXCL) == NULL);
    int counts[2] = {0, 0};
    CHECK(host_fs_list(root, count_cb, counts) == 0);
    CHECK(counts[0] == 1 && counts[1] == 1);

    CHECK(host_fs_rename(path, path2) == 0);
    CHECK(host_fs_stat(path, &st) == 0 && !st.exists);
    CHECK(host_fs_stat(path2, &st) == 0 && st.exists && st.size == 10);
    CHECK(host_fs_remove(path2) == 0);
    CHECK(host_fs_remove(sub) == 0);
    snprintf(sub, sizeof sub, "%s%csub", root, sep);
    CHECK(host_fs_remove(sub) == 0);

    /* User dirs, redirected into the temp tree. */
    char ud[1024];
#ifdef _WIN32
    wchar_t wroot[1024];
    MultiByteToWideChar(CP_UTF8, 0, root, -1, wroot, 1024);
    SetEnvironmentVariableW(L"LOCALAPPDATA", wroot);
    int n = host_fs_user_dir(HOST_DIR_SAVES, "TOTK", ud, sizeof ud);
    CHECK(n > 0 && strstr(ud, "MacNativeGames\\TOTK\\saves") != NULL);
    CHECK(host_fs_stat(ud, &st) == 0 && st.is_dir);
    char tiny[8];
    CHECK(host_fs_user_dir(HOST_DIR_LOGS, "TOTK", tiny, sizeof tiny) == -1);
    CHECK(host_fs_remove(ud) == 0);
    snprintf(ud, sizeof ud, "%s\\MacNativeGames\\TOTK", root); host_fs_remove(ud);
    snprintf(ud, sizeof ud, "%s\\MacNativeGames", root); host_fs_remove(ud);
#else
    (void)ud;
#endif
    CHECK(host_fs_remove(root) == 0);
    TEST_DONE("test_host_fs");
}
