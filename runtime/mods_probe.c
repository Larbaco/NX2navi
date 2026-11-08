// --probe-mods: the mods folder scan, its order and switches (mods.c). Builds a mods folder of its own next to nx2navi.exe,
// scans it and removes it. Needs no game files.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#endif

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL mods: %s\n", what); } } while (0)
#define EQ(a, b) ((a) && !strcmp((a), (b)))

// root/rel as a file of a few bytes (rel ends in '/': a folder), with every folder above it.
static void put(const char *root, const char *rel) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    char *slash = strrchr(p, '/');
    *slash = 0;
    plat_file_mkdir_p(p);
    *slash = '/';
    if (slash[1]) plat_file_write(p, "probe", 5);
}

// root/rel as a file holding text.
static void put_text(const char *root, const char *rel, const char *text) {
    put(root, rel);
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    plat_file_write(p, text, strlen(text));
}

// link (a path) as a symbolic link to the folder target; 0 when the system refuses (Windows without developer mode).
static int dir_link(const char *target, const char *link) {
#ifdef _WIN32
    wchar_t t[1024], l[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, target, -1, t, 1024) || !MultiByteToWideChar(CP_UTF8, 0, link, -1, l, 1024))
        return 0;
    return CreateSymbolicLinkW(l, t, SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
#else
    (void)target, (void)link;
    return 0;
#endif
}

static int remove_cb(const char *name, int kind, uint64_t size, void *ctx);
static void remove_tree(const char *dir) {
    plat_fs_list(dir, remove_cb, (void *)dir);
    plat_fs_rmdir(dir);
}
static int remove_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    char sub[2048];
    snprintf(sub, sizeof sub, "%s/%s", (const char *)ctx, name);
    if (kind == PLAT_FS_DIR) remove_tree(sub);
    else if (kind != PLAT_FS_LINK || plat_fs_rmdir(sub) != PLAT_FS_OK) plat_fs_unlink(sub);   // a link: never followed
    return 1;
}

// s ends in tail, either path separator matching either.
static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), t = strlen(tail);
    if (n < t) return 0;
    for (size_t i = 0; i < t; i++) {
        char a = s[n - t + i], b = tail[i];
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        if (a != b) return 0;
    }
    return 1;
}

int probe_mods(void) {
    char root[1024];
    if (!plat_exe_path(root, sizeof root)) return 1;
    char *cut = strrchr(root, '/'), *back = strrchr(root, '\\');
    if (back > cut) cut = back;
    if (!cut) return 1;
    strcpy(cut + 1, "mods-probe");
    remove_tree(root);
    plat_file_mkdir_p(root);
    static const char *const files[] = {
        "A/romfs/x.bin", "B/atmosphere/contents/0400486024DEC000/romfs/x.bin",
        "C/contents/0400486024DEC000/romfs/y.bin", "D/exefs/main.pchtxt", "E/exefs/subsdk9", "F/romfs/",
        "G/romfs/z.bin", "G/exefs/1234.ips", "Ultra \xD0\x96/romfs/u.bin",
        "H/romfs/h.bin", "H/exefs/subsdk9", "I/romfs/i.bin", "I/exefs/main", "J/cheats/0123456789ABCDEF.txt",
        "K/romfslite/k.bin", "L/romfs/l.bin", "L/cheats/0123456789ABCDEF.txt", "L/exefs/main.npdm",
    };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) put(root, files[i]);
    // an IPS file for this game's main (any file counts as one when main cannot be read), in Atmosphere's layout
    char ips[160];
    snprintf(ips, sizeof ips, "M/atmosphere/exefs_patches/x/%s.ips", *mods_main_build_id() ? mods_main_build_id() : "0");
    put(root, ips);
    // pchtxt files for main as tools/exefs_patches.py reads them: a BOM and an indented build id line (N); a build id
    // line with more than hex on it (O) and one after @stop (P) name no build, so another version's
    const char *bid = *mods_main_build_id() ? mods_main_build_id() : "0";
    char text[256];
    snprintf(text, sizeof text, "\xEF\xBB\xBF// a title\r\n  @nsobid-%s   // main\r\n@enabled\r\n", bid);
    put_text(root, "N/exefs/bom.pchtxt", text);
    snprintf(text, sizeof text, "@nsobid-%s v2\n", bid);
    put_text(root, "O/exefs/junk.pchtxt", text);
    snprintf(text, sizeof text, "@stop\n@nsobid-%s\n", bid);
    put_text(root, "P/exefs/stop.pchtxt", text);
    // a link to a folder outside the mods folder is a mod (Q); left out when the system cannot make one
    char outside[1100], link[1100];
    snprintf(outside, sizeof outside, "%s-linked", root);
    remove_tree(outside);
    put(outside, "romfs/q.bin");
    snprintf(link, sizeof link, "%s/Q", root);
    int linked = dir_link(outside, link);
    if (!linked) printf("mods: no symbolic link made (developer mode off?); the linked folder check is skipped\n");

    cfg_set("NX2NAVI_MODS_DIR", "mods-probe");   // relative: under the folder of nx2navi.exe
    cfg_set("NX2NAVI_MODS_ORDER", "B|A|Gone");
    cfg_set("NX2NAVI_MODS_DISABLED", "C");
    mods_scan();

    CHECK(ends_with(mods_dir(), "/mods-probe"), "a relative mods_dir is under the folder of nx2navi.exe");
    static const char *const order[] = {"B", "A", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P",
                                        "Q", "Ultra \xD0\x96"};
    enum { N = sizeof order / sizeof *order };
    // without the link Q is missing: the names after it move up one
    int n_want = linked ? N : N - 1, in_order = mods_count() == n_want;
    CHECK(in_order, linked ? "eighteen mod folders" : "seventeen mod folders");
    for (int i = 0, k = 0; in_order && i < N; i++) {
        if (!linked && EQ(order[i], "Q")) continue;
        in_order = EQ(mods_at(k)->name, order[i]);
        k++;
    }
    CHECK(in_order, "order: mods_order first, the rest by name");
    if (in_order) {
        ModInfo *a = mods_at(1), *b = mods_at(0), *c = mods_at(2), *d = mods_at(3), *e = mods_at(4), *f = mods_at(5),
                *g = mods_at(6), *hm = mods_at(7), *im = mods_at(8), *jm = mods_at(9), *km = mods_at(10),
                *lm = mods_at(11), *mm = mods_at(12), *nm = mods_at(13), *om = mods_at(14), *pm = mods_at(15),
                *u = mods_at(n_want - 1);
        CHECK(hm->kind == (MOD_ROMFS | MOD_SUBSDK) && hm->problem == MOD_PLUGIN, "RomFS + subsdk plugin: unusable");
        CHECK(im->kind == (MOD_ROMFS | MOD_SUBSDK) && im->problem == MOD_PLUGIN, "RomFS + replaced exefs/main");
        CHECK(ends_with(b->romfs, "B/atmosphere/contents/0400486024DEC000/romfs"), "Atmosphere RomFS root");
        CHECK(ends_with(c->romfs, "C/contents/0400486024DEC000/romfs"), "contents RomFS root");
        CHECK(ends_with(a->romfs, "A/romfs") && a->kind == MOD_ROMFS && a->problem == MOD_OK, "plain RomFS mod");
        CHECK(!c->enabled && a->enabled && b->enabled && g->enabled && u->enabled, "mods_disabled switches C off");
        CHECK(d->kind == MOD_EXEFS && d->problem == MOD_EXEFS_OTHER && d->other_version == 1,
              "ExeFS patch mod without a build id: another version's");
        CHECK(e->problem == MOD_PLUGIN, "subsdk-only mod is a plugin mod");
        CHECK(f->problem == MOD_EMPTY, "empty RomFS folder");
        CHECK(g->kind == (MOD_ROMFS | MOD_EXEFS) && g->problem == MOD_OK && g->other_version == 1,
              "RomFS + another version's ExeFS patch: the RomFS is used");
        CHECK(mm->kind == MOD_EXEFS && mm->problem == MOD_EXEFS_LATER && mm->pending == 1,
              "an ExeFS patch for main the build lacks waits for the rebuild");
        CHECK(jm->kind == MOD_CHEATS && jm->problem == MOD_OK && ends_with(jm->cheats, "J/cheats"),
              "cheats-only mod");
        CHECK(km->kind == MOD_ROMFS && km->problem == MOD_OK && ends_with(km->romfs, "K/romfslite"), "merged romfslite");
        CHECK(lm->kind == (MOD_ROMFS | MOD_CHEATS) && lm->problem == MOD_OK, "RomFS + cheats, main.npdm is no code");
        CHECK(u->kind == MOD_ROMFS && ends_with(u->romfs, "Ultra \xD0\x96/romfs"), "Unicode folder name");
        CHECK(nm->kind == MOD_EXEFS && nm->problem == MOD_EXEFS_LATER && nm->pending == 1 && !nm->other_version,
              "pchtxt with a BOM and an indented build id line is for main");
        CHECK(om->problem == MOD_EXEFS_OTHER && om->other_version == 1 && !om->pending,
              "pchtxt build id line with more than hex: another version's");
        CHECK(pm->problem == MOD_EXEFS_OTHER && pm->other_version == 1, "pchtxt build id after @stop is not read");
        if (linked) {
            const ModInfo *q = mods_at(16);
            CHECK(q->kind == MOD_ROMFS && q->problem == MOD_OK && ends_with(q->romfs, "Q/romfs"),
                  "a linked mod folder is a mod");
        }
        CHECK(!mods_changed(), "nothing changed after the scan");

        mods_move(1, -1);
        CHECK(EQ(mods_at(0)->name, "A") && EQ(mods_at(1)->name, "B"), "move up");
        CHECK(EQ(cfg_get("NX2NAVI_MODS_ORDER"), linked ? "A|B|C|D|E|F|G|H|I|J|K|L|M|N|O|P|Q|Ultra \xD0\x96"
                                                       : "A|B|C|D|E|F|G|H|I|J|K|L|M|N|O|P|Ultra \xD0\x96"),
              "order written without the gone name");
        mods_move(0, -1);
        CHECK(EQ(mods_at(0)->name, "A"), "move up at the top does nothing");
        mods_move(n_want - 1, 1);
        CHECK(EQ(mods_at(n_want - 1)->name, "Ultra \xD0\x96"), "move down at the bottom does nothing");
        mods_set_enabled(2, 1);
        const char *dis = cfg_get("NX2NAVI_MODS_DISABLED");
        CHECK(mods_at(2)->enabled && (!dis || !*dis), "switching C on empties mods_disabled");
        mods_set_enabled(0, 0);
        CHECK(EQ(cfg_get("NX2NAVI_MODS_DISABLED"), "A"), "switching A off writes it");
        CHECK(mods_changed(), "changes seen");

        // the ExeFS patch files waiting for the rebuild follow the mods' start state, wherever the menu moved them:
        // M and N wait (both on at the start); M moved onto C's place (off at the start) and switched off still waits
        uint32_t key = 0;
        CHECK(mods_exefs_pending(&key) == 2, "two ExeFS patch files wait for the rebuild");
        for (int i = 12; i > 2; i--) mods_move(i, -1);
        CHECK(EQ(mods_at(2)->name, "M"), "M moved up to the third place");
        mods_set_enabled(2, 0);
        key = 0;
        CHECK(mods_exefs_pending(&key) == 2, "the start state goes with its mod when the menu moves it");
    }
    remove_tree(root);
    remove_tree(outside);
    printf("mods: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
