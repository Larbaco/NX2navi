// Whether a setting does anything on the running game build (cfg.h cfg_supported). The menu hides a row whose key it
// answers 0 for (ui/option_rows.cpp), and the start-up log names the keys a player has set that are inert.
// The answers follow the generated patch sets of recomp/patches-v*.txt (rt_patch_set is -1 for a set the build lacks)
// wherever a setting needs one; the few that depend on the game itself check rt_game_version.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"

static int has(const char *set) { return rt_patch_set(set) >= 0; }

int cfg_supported(const char *key) {
    char k[64];
    snprintf(k, sizeof k, "%s", key);
    const char *name = k;
    if (!strncasecmp(name, "NX2NAVI_", 8)) name += 8;
    if (!strcasecmp(name, "FOV")) return has("fov");
    if (!strcasecmp(name, "OUTPUT_RES") || !strcasecmp(name, "RENDER_SCALE")) return res_adjustable();
    if (!strcasecmp(name, "SHADOW_RES")) return has("shadow");
    if (!strcasecmp(name, "SHADOW_RANGE")) return has("cascade");
    if (!strcasecmp(name, "REALTIME")) return has("realtime");
    if (!strcasecmp(name, "CLOTH_REALTIME")) return has("cloth");
    if (!strcasecmp(name, "HIGHFPS_FIX")) return has("bounce") || has("fade");
    if (!strcasecmp(name, "MENU_CURSOR")) return RT_V143;   // menu_cursor.c enabled()
    // The camera speed rewrites the player camera's pack of the 1.x Tears-of-the-Kingdom-shaped RomFS (camera_speed.c);
    // Ocarina's has no such file, so the factor changes nothing there.
    if (!strcasecmp(name, "STICK_SENS")) return rt_game_version != 111;
    return 1;
}

// One line per key the player set (nx2navi.ini or the environment) that this game build ignores.
static void note_unsupported(const char *key, const char *value, int from_env, void *ctx) {
    (void)value, (void)from_env, (void)ctx;
    if (!cfg_supported(key)) rt_log("cfg: %s has no effect on this game version", key);
}

void cfg_log_unsupported(void) { cfg_each_active(note_unsupported, NULL); }
