#pragma once

#include <filesystem>
#include <string>

namespace gtkdrop {

namespace fs = std::filesystem;

// GTKdrop's own data lives under $XDG_CONFIG_HOME/gtkdrop.
namespace paths {
fs::path data_dir();           // ~/.config/gtkdrop
fs::path settings_file();      // ~/.config/gtkdrop/settings.ini
fs::path profiles_dir();       // ~/.config/gtkdrop/profiles
fs::path selected_mods_dir();  // ~/.config/gtkdrop/selected-mods — what SMAPI is pointed at
fs::path collections_dir();    // ~/.config/gtkdrop/collections — default collection install root
fs::path collection_cache_dir(); // ~/.config/gtkdrop/collections/cache — one record per collection
fs::path stardrop_data_dir();  // ~/.config/Stardrop/Data (import source)
fs::path library_file();       // ~/.config/gtkdrop/library.json — what GTKdrop installed, from where
} // namespace paths

struct Settings {
    // Folders
    fs::path game_dir;        // the Stardew Valley folder holding SMAPI (StardewModdingAPI.dll)
    fs::path mods_dir;        // where mods are discovered (default: <game_dir>/Mods)
    fs::path collections_dir; // where collections install their own copies of mods (default: paths::collections_dir()/mods)

    // Profiles
    std::string last_profile = "Default";
    bool profile_mod_configs = true;    // keep a copy of every mod's config.json per profile
    bool auto_save_profiles = true;     // save the profile on every change instead of on "Save Profile"
    bool ignore_hidden_folders = true;  // skip mods inside folders starting with '.', as SMAPI does

    // Launching: "steam", "gog" (start the game directly) or empty (ask on the first launch).
    std::string launch_method;

    // Collections
    bool collections_skip_smapi_update_check = true;

    // One-off import of Stardrop's data has run (successfully or because there was nothing to import).
    bool stardrop_import_done = false;

    fs::path effective_mods_dir() const;
    fs::path effective_collections_dir() const;
    bool has_game_dir() const;
};

Settings load_settings();
void save_settings(const Settings &settings);

} // namespace gtkdrop
