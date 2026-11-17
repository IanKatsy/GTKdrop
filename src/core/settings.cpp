#include "settings.h"

#include <glib.h>

namespace gtkdrop {

namespace paths {
fs::path data_dir() { return fs::path(g_get_user_config_dir()) / "gtkdrop"; }
fs::path settings_file() { return data_dir() / "settings.ini"; }
fs::path profiles_dir() { return data_dir() / "profiles"; }
fs::path selected_mods_dir() { return data_dir() / "selected-mods"; }
fs::path collections_dir() { return data_dir() / "collections"; }
fs::path collection_cache_dir() { return collections_dir() / "cache"; }
fs::path library_file() { return data_dir() / "library.json"; }
fs::path stardrop_data_dir() { return fs::path(g_get_user_config_dir()) / "Stardrop" / "Data"; }
} // namespace paths

fs::path Settings::effective_mods_dir() const {
    if (!mods_dir.empty())
        return mods_dir;
    return game_dir.empty() ? fs::path() : game_dir / "Mods";
}

fs::path Settings::effective_collections_dir() const {
    return collections_dir.empty() ? paths::collections_dir() / "mods" : collections_dir;
}

bool Settings::has_game_dir() const {
    std::error_code ec;
    return !game_dir.empty() && fs::exists(game_dir / "StardewModdingAPI.dll", ec);
}

namespace {

constexpr char kPaths[] = "paths";
constexpr char kProfiles[] = "profiles";
constexpr char kCollections[] = "collections";
constexpr char kImport[] = "import";
constexpr char kLaunch[] = "launch";

std::string get_str(GKeyFile *kf, const char *group, const char *key) {
    char *v = g_key_file_get_string(kf, group, key, nullptr);
    std::string s = v ? v : "";
    g_free(v);
    return s;
}

bool get_bool(GKeyFile *kf, const char *group, const char *key, bool fallback) {
    GError *error = nullptr;
    const gboolean v = g_key_file_get_boolean(kf, group, key, &error);
    if (error) {
        g_error_free(error);
        return fallback;
    }
    return v;
}

} // namespace

Settings load_settings() {
    Settings s;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, paths::settings_file().c_str(), G_KEY_FILE_NONE, nullptr)) {
        s.game_dir = get_str(kf, kPaths, "game");
        s.mods_dir = get_str(kf, kPaths, "mods");
        s.collections_dir = get_str(kf, kPaths, "collections");
        if (auto p = get_str(kf, kProfiles, "last"); !p.empty())
            s.last_profile = p;
        s.profile_mod_configs = get_bool(kf, kProfiles, "per-profile-mod-configs", s.profile_mod_configs);
        s.auto_save_profiles = get_bool(kf, kProfiles, "auto-save", s.auto_save_profiles);
        s.ignore_hidden_folders = get_bool(kf, kProfiles, "ignore-hidden-folders", s.ignore_hidden_folders);
        s.collections_skip_smapi_update_check =
                get_bool(kf, kCollections, "skip-smapi-update-check", s.collections_skip_smapi_update_check);
        s.stardrop_import_done = get_bool(kf, kImport, "stardrop-done", false);
        s.launch_method = get_str(kf, kLaunch, "method");
    }
    g_key_file_free(kf);
    return s;
}

void save_settings(const Settings &s) {
    const auto file = paths::settings_file();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);

    // Loaded first so the [appearance] group (written by the appearance code) is kept.
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, file.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    g_key_file_set_string(kf, kPaths, "game", s.game_dir.c_str());
    g_key_file_set_string(kf, kPaths, "mods", s.mods_dir.c_str());
    g_key_file_set_string(kf, kPaths, "collections", s.collections_dir.c_str());
    g_key_file_set_string(kf, kProfiles, "last", s.last_profile.c_str());
    g_key_file_set_boolean(kf, kProfiles, "per-profile-mod-configs", s.profile_mod_configs);
    g_key_file_set_boolean(kf, kProfiles, "auto-save", s.auto_save_profiles);
    g_key_file_set_boolean(kf, kProfiles, "ignore-hidden-folders", s.ignore_hidden_folders);
    g_key_file_set_boolean(kf, kCollections, "skip-smapi-update-check", s.collections_skip_smapi_update_check);
    g_key_file_set_boolean(kf, kImport, "stardrop-done", s.stardrop_import_done);
    g_key_file_set_string(kf, kLaunch, "method", s.launch_method.c_str());

    GError *error = nullptr;
    if (!g_key_file_save_to_file(kf, file.c_str(), &error)) {
        g_warning("Could not save %s: %s", file.c_str(), error->message);
        g_error_free(error);
    }
    g_key_file_free(kf);
}

} // namespace gtkdrop
