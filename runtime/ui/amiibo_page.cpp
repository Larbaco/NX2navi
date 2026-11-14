// Amiibo / NFC menu page (Dear ImGui): browser and scan controls, the figure's data, the keys file and write-back.
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "ui/amiibo_page.h"
#include "ui/option_rows.h"

extern "C" {
#include "amiibo.h"
#include "amiibo_crypto.h"
#include "cfg.h"
#include "folder_pick.h"
#include "platform/platform.h"
#include "ui_text.h"
}

namespace fs = std::filesystem;

struct FileEntry {
    std::string name;
    bool is_dir;
};

static const char kDefaultAmiiboDir[] = "./amiibo";
static std::string g_current_dir = kDefaultAmiiboDir;
static std::vector<FileEntry> g_entries;
static int g_selected_entry = -1;
// The last action: a catalog id and the file or folder it names, formatted when drawn (the menu language can change)
static UiTextId g_status_id = UI_MENU_AMIIBO_READY;
static std::string g_status_arg;

static void set_status(UiTextId id, const std::string &arg = std::string()) {
    g_status_id = id;
    g_status_arg = arg;
}

// Erase game data asks twice: the first press arms the button, a second one within kEraseConfirmS seconds erases.
static const double kEraseConfirmS = 3.0;
static double g_erase_armed_at = -1.0;   // ImGui::GetTime() of the first press; negative while not armed

// Loads a dump and puts it on the reader; the status names the file.
static void tap(const std::string &dir, const std::string &name) {
    g_erase_armed_at = -1.0;   // a press armed for the previous figure does not erase this one
    std::string full_path = (fs::path(dir) / name).string();
    set_status(amiibo_scan_and_tap(full_path.c_str()) ? UI_MENU_AMIIBO_TAPPED : UI_MENU_AMIIBO_LOAD_FAILED, name);
}

static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static void set_current_dir(const std::string &path) {
    g_current_dir = path;
    cfg_set("NX2NAVI_AMIIBO_DIR", g_current_dir.c_str());
    opt_save_soon();   // nx2navi.ini follows a moment later, as after a row's change
}

// The host's folder dialog (folder_pick.h) answers on its own thread; the page takes the answer when it draws.
static PlatMutex g_pick_lock = PLAT_MUTEX_INIT;
static bool g_pick_open;
static bool g_pick_ready;
static std::string g_pick_path;

static void on_folder_picked(const char *path, void *ctx) {
    (void)ctx;
    plat_mutex_lock(&g_pick_lock);
    g_pick_ready = path != NULL;
    if (path) g_pick_path = path;
    g_pick_open = false;
    plat_mutex_unlock(&g_pick_lock);
}

static void refresh_dir(void) {
    g_entries.clear();
    g_selected_entry = -1;

    std::error_code ec;
    fs::path dir_path(g_current_dir);

    // A missing folder lists nothing and says so; it is not replaced by another one, and opening the tab writes no
    // setting (the folder is saved only when the player sets one).
    if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
        set_status(UI_MENU_AMIIBO_NO_DIR, g_current_dir);
        return;
    }
    if (g_status_id == UI_MENU_AMIIBO_NO_DIR) set_status(UI_MENU_AMIIBO_READY);

    // Add parent navigation if not at root
    if (dir_path.has_parent_path() && dir_path != dir_path.parent_path()) {
        g_entries.push_back({"..", true});
    }

    for (const auto &entry : fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        std::error_code status_ec;
        bool is_dir = entry.is_directory(status_ec);
        if (status_ec) continue;

        std::string filename = entry.path().filename().string();
        if (filename.empty() || filename == "." || filename == "..") continue;

        if (!is_dir) {
            std::string ext = entry.path().extension().string();
            if (!iequals(ext, ".bin") && !iequals(ext, ".nfc")) {
                continue;
            }
            // amiibo_load_file takes whole 540-byte dumps only, so other files of that name (the keys file
            // key_retail.bin among them) stay out of the list
            std::error_code size_ec;
            if (entry.file_size(size_ec) != (uintmax_t)AMIIBO_DUMP_SIZE || size_ec) continue;
        }

        g_entries.push_back({std::move(filename), is_dir});
    }

    std::sort(g_entries.begin(), g_entries.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.name == "..") return true;
        if (b.name == "..") return false;
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
    });
}

// A catalog format and its arguments, wrapped to the pane (a path or a long translation does not fit one line).
static void text_wrapped_fmt(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::TextWrapped("%s", buf);
}

// The loaded figure's own data (amiibo.h amiibo_get_figure_data) and the button that erases its game data.
static void draw_figure_data(void) {
    AmiiboFigureData fd;
    if (!amiibo_get_figure_data(&fd)) {
        g_erase_armed_at = -1.0;
        return;
    }
    fd.nickname[sizeof fd.nickname - 1] = 0;
    ImGui::Spacing();
    if (fd.registered)
        text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_NICKNAME), fd.nickname);
    else
        ImGui::TextWrapped("%s", menu_text(UI_MENU_AMIIBO_NOT_REGISTERED));
    UiTextId source = fd.source == AMIIBO_SOURCE_SAVED  ? UI_MENU_AMIIBO_SOURCE_SAVED
                      : fd.source == AMIIBO_SOURCE_DUMP ? UI_MENU_AMIIBO_SOURCE_DUMP
                                                        : UI_MENU_AMIIBO_SOURCE_NEW;
    text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_SOURCE), menu_text(source));
    ImGui::TextWrapped("%s", menu_text(fd.decrypted ? UI_MENU_AMIIBO_DECRYPTED : UI_MENU_AMIIBO_NOT_DECRYPTED));
    text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_WRITES), (unsigned)fd.write_counter, (unsigned)fd.write_year,
                     (unsigned)fd.write_month, (unsigned)fd.write_day);
    if (fd.has_app_area)
        text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_GAME_DATA), (unsigned)fd.app_area_size, (unsigned)fd.access_id);
    else
        ImGui::TextWrapped("%s", menu_text(UI_MENU_AMIIBO_NO_GAME_DATA));

    // The game data goes for good (the port's copy in the profile too), so the button asks for a second press. "###"
    // keeps one id under both labels: the controller's cursor stays on the button when its label changes.
    const double now = ImGui::GetTime();
    if (!fd.has_app_area || now - g_erase_armed_at >= kEraseConfirmS) g_erase_armed_at = -1.0;
    const bool armed = g_erase_armed_at >= 0.0;
    const std::string label =
        std::string(menu_text(armed ? UI_MENU_AMIIBO_ERASE_CONFIRM : UI_MENU_AMIIBO_ERASE)) + "###amiibo_erase";
    ImGui::Spacing();
    ImGui::BeginDisabled(!fd.has_app_area);
    if (armed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
    const bool pressed = ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0));
    if (armed) ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (pressed && !armed) {
        g_erase_armed_at = now;
    } else if (pressed) {
        g_erase_armed_at = -1.0;
        set_status(amiibo_erase_app_area() ? UI_MENU_AMIIBO_ERASED : UI_MENU_AMIIBO_ERASE_FAILED);
    }
}

// The keys file amiibo_reload_keys reads: NX2NAVI_AMIIBO_KEYS, else key_retail.bin in the amiibo folder
// (NX2NAVI_AMIIBO_DIR, else ./amiibo).
static std::string keys_path(void) {
    const char *keys = cfg_get("NX2NAVI_AMIIBO_KEYS");
    if (keys && *keys) return keys;
    const char *dir = cfg_get("NX2NAVI_AMIIBO_DIR");
    std::string path = dir && *dir ? dir : kDefaultAmiiboDir;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + "key_retail.bin";
}

// The keys: loaded or not, the file in effect, Reload keys; and the write-back setting (NX2NAVI_AMIIBO_WRITEBACK, a live
// setting), saved like a row's change (option_rows.h opt_apply).
static void draw_keys(void) {
    // amiibo.c reads the keys with the first dump; the tab's first draw reads them too, so the line below is right
    // before any tap
    static bool g_keys_read;
    if (!g_keys_read) {
        g_keys_read = true;
        if (!amiibo_keys_loaded()) amiibo_reload_keys();
    }
    const bool loaded = amiibo_keys_loaded();
    ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_KEYS));
    ImGui::SameLine();
    ImGui::TextColored(loaded ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s",
                       menu_text(loaded ? UI_MENU_AMIIBO_KEYS_LOADED : UI_MENU_AMIIBO_KEYS_MISSING));
    const std::string path = keys_path();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_KEYS_FILE), path.c_str());
    ImGui::PopStyleColor();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_RELOAD_KEYS), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
        set_status(amiibo_reload_keys() ? UI_MENU_AMIIBO_KEYS_RELOADED : UI_MENU_AMIIBO_KEYS_FAILED, path);

    ImGui::Spacing();
    const bool env = cfg_from_env("NX2NAVI_AMIIBO_WRITEBACK") != 0;
    bool writeback = cfg_bool("NX2NAVI_AMIIBO_WRITEBACK") != 0;
    ImGui::BeginDisabled(env);
    if (ImGui::Checkbox(menu_text(UI_MENU_AMIIBO_WRITEBACK), &writeback))
        opt_apply("NX2NAVI_AMIIBO_WRITEBACK", writeback ? "1" : "0");
    ImGui::EndDisabled();
    if (env)
        ImGui::TextDisabled("%s", menu_text(UI_MENU_SET_BY_ENV));
    else if (!loaded)
        ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_WRITEBACK_NEEDS_KEYS));
}

void tab_amiibo(float scale) {
    (void)scale;   // the rows take ImGui's own sizes, already scaled with the font
    static bool s_inited = false;
    if (!s_inited) {
        const char *cfg_dir = cfg_get("NX2NAVI_AMIIBO_DIR");
        if (cfg_dir && *cfg_dir) {
            g_current_dir = cfg_dir;
        } else {
            g_current_dir = kDefaultAmiiboDir;
        }
        refresh_dir();
        s_inited = true;
    }

    plat_mutex_lock(&g_pick_lock);
    bool picking = g_pick_open;
    std::string picked;
    if (g_pick_ready) picked.swap(g_pick_path);
    g_pick_ready = false;
    plat_mutex_unlock(&g_pick_lock);
    if (!picked.empty()) {
        set_current_dir(picked);
        refresh_dir();
    }

    // Two panes: Left (Directory Browser & Tag List), Right (Amiibo Tag Details & Controls)
    const float content_h = ImGui::GetContentRegionAvail().y - opt_bottom();   // Continue and Quit stay visible
    const float left_w = ImGui::GetContentRegionAvail().x * 0.58f;
    const float right_w = ImGui::GetContentRegionAvail().x - left_w - ImGui::GetStyle().ItemSpacing.x;

    // ---- Left Pane: Explorer & List ----
    ImGui::BeginChild("##amiibo_explorer", ImVec2(left_w, content_h), true);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "%s", menu_text(UI_MENU_AMIIBO_DIR));

    ImGui::TextWrapped("%s", g_current_dir.c_str());

    ImGui::Spacing();
    ImGui::BeginDisabled(picking);
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_OPEN))) {
        plat_mutex_lock(&g_pick_lock);
        g_pick_open = true;
        plat_mutex_unlock(&g_pick_lock);
        folder_pick(g_current_dir.c_str(), on_folder_picked, NULL);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_DEFAULT_DIR))) {
        set_current_dir(kDefaultAmiiboDir);
        refresh_dir();
    }
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_REFRESH))) {
        refresh_dir();
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_PICK));

    ImGui::BeginChild("##file_list", ImVec2(0, 0), true);
    if (g_status_id == UI_MENU_AMIIBO_NO_DIR) {
        char missing[512];
        snprintf(missing, sizeof missing, menu_text(UI_MENU_AMIIBO_NO_DIR), g_current_dir.c_str());
        ImGui::TextWrapped("%s", missing);
    }
    for (size_t i = 0; i < g_entries.size(); i++) {
        const auto &e = g_entries[i];
        std::string label = e.is_dir ? e.name + "/" : e.name;
        bool selected = ((int)i == g_selected_entry);

        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            g_selected_entry = (int)i;
            if (e.is_dir) {
                if (ImGui::IsMouseDoubleClicked(0) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
                    if (e.name == "..") {
                        fs::path cur(g_current_dir);
                        if (cur.has_parent_path() && cur != cur.parent_path()) {
                            set_current_dir(cur.parent_path().string());
                        } else {
                            set_current_dir(".");
                        }
                    } else {
                        set_current_dir((fs::path(g_current_dir) / e.name).string());
                    }
                    refresh_dir();
                    break;
                }
            } else {
                if (ImGui::IsMouseDoubleClicked(0)) tap(g_current_dir, e.name);
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Right Pane: Controls & Status ----
    ImGui::BeginChild("##amiibo_controls", ImVec2(right_w, content_h), true);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "%s", menu_text(UI_MENU_AMIIBO_READER));
    ImGui::Separator();

    AmiiboDeviceState state = amiibo_get_reader_state();
    UiTextId state_id = UI_MENU_AMIIBO_STATE_IDLE;
    ImVec4 state_col = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    switch (state) {
    case AMIIBO_DEV_STATE_INITIALIZED: state_id = UI_MENU_AMIIBO_STATE_IDLE; break;
    case AMIIBO_DEV_STATE_SEARCHING: state_id = UI_MENU_AMIIBO_STATE_SEARCHING; state_col = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); break;
    case AMIIBO_DEV_STATE_TAG_FOUND: state_id = UI_MENU_AMIIBO_STATE_FOUND; state_col = ImVec4(0.3f, 0.9f, 0.3f, 1.0f); break;
    case AMIIBO_DEV_STATE_TAG_MOUNTED: state_id = UI_MENU_AMIIBO_STATE_MOUNTED; state_col = ImVec4(0.2f, 0.9f, 1.0f, 1.0f); break;
    case AMIIBO_DEV_STATE_UNAVAILABLE: state_id = UI_MENU_AMIIBO_STATE_OFF; state_col = ImVec4(0.9f, 0.3f, 0.3f, 1.0f); break;
    default: break;
    }
    ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_STATE));
    ImGui::SameLine();
    ImGui::TextColored(state_col, "%s", menu_text(state_id));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_FILE));
    if (g_selected_entry >= 0 && g_selected_entry < (int)g_entries.size() && !g_entries[g_selected_entry].is_dir) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", g_entries[g_selected_entry].name.c_str());

        if (ImGui::Button(menu_text(UI_MENU_AMIIBO_TAP), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
            tap(g_current_dir, g_entries[g_selected_entry].name);
    } else {
        ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_NO_FILE));
        ImGui::BeginDisabled();
        ImGui::Button(menu_text(UI_MENU_AMIIBO_TAP), ImVec2(ImGui::GetContentRegionAvail().x, 0));
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_REMOVE), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
        amiibo_remove_tag();
        set_status(UI_MENU_AMIIBO_REMOVED);
    }

    ImGui::Spacing();
    ImGui::Separator();
    bool auto_remove = amiibo_get_auto_remove();
    if (ImGui::Checkbox(menu_text(UI_MENU_AMIIBO_AUTO_REMOVE), &auto_remove)) {
        amiibo_set_auto_remove(auto_remove);
    }
    if (auto_remove) {
        float delay = amiibo_get_auto_remove_delay_s();
        ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_HOLD));   // above the slider: beside it the text is cut off
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##amiibo_hold", &delay, 1.0f, 6.0f, menu_text(UI_MENU_AMIIBO_SECONDS))) {
            amiibo_set_auto_remove_delay_s(delay);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_TAG));
    if (amiibo_has_tag()) {
        const AmiiboInfo *info = amiibo_get_current_info();
        char character[8], uid[24];
        snprintf(character, sizeof character, "%02x%02x%02x", info->character_id[0], info->character_id[1],
                 info->character_id[2]);
        snprintf(uid, sizeof uid, "%02x:%02x:%02x:%02x:%02x:%02x:%02x", info->uuid[0], info->uuid[1], info->uuid[2],
                 info->uuid[3], info->uuid[4], info->uuid[5], info->uuid[6]);
        ImGui::Text(menu_text(UI_MENU_AMIIBO_TAG_FILE), info->filename);
        ImGui::Text(menu_text(UI_MENU_AMIIBO_TAG_ID), character, info->series_id, info->numbering_id, info->nfp_type);
        ImGui::Text(menu_text(UI_MENU_AMIIBO_TAG_UID), uid);
        draw_figure_data();
    } else {
        ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_NO_TAG));
        g_erase_armed_at = -1.0;
    }

    ImGui::Spacing();
    ImGui::Separator();
    draw_keys();

    ImGui::Spacing();
    ImGui::Separator();
    char status[512];
    snprintf(status, sizeof status, menu_text(g_status_id), g_status_arg.c_str());
    ImGui::TextDisabled(menu_text(UI_MENU_AMIIBO_STATUS), status);

    ImGui::EndChild();
}
