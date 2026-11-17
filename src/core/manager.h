#pragma once

#include "mod.h"
#include "launcher.h"
#include "nexus.h"
#include "profile.h"
#include "selected_mods.h"
#include "settings.h"
#include "stardrop_import.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gtkdrop {

// Owns GTKdrop's state: settings, the installed mods and the profiles. UI-free, so it can be tested alone.
//
// Per-profile configs follow one rule: the config.json files on disk belong to the active profile. Switching
// profile saves them into the profile being left and writes the new profile's copies; the game exiting (or the
// app closing) saves them into the active profile, as mods are usually configured in game.
class ModManager {
public:
    struct LoadReport {
        std::optional<ImportResult> import; // set when the one-off Stardrop import ran this time
        std::string game_detected;          // "Steam" or "GOG" when the game folder was found this time
    };

    LoadReport load();
    void rescan();

    const Settings &settings() const { return settings_; }
    void update_settings(const Settings &settings);

    std::vector<Mod> &mods() { return mods_; }

    // Latest Nexus versions from an update check (Nexus mod ID -> version); kept across rescans.
    void set_latest_versions(std::map<int, std::string> versions);
    bool update_check_done() const { return update_check_done_; }
    const std::vector<Profile> &profiles() const { return store_.profiles(); }
    const Profile &active() const;
    bool dirty() const { return dirty_; }

    void set_enabled(Mod &mod, bool enabled);
    void set_enabled_many(const std::vector<Mod *> &mods, bool enabled); // one profile write

    // Creates (or updates) the protected profile for a Nexus collection, linked to it by Stardrop's source ID
    // ("stardewvalley-<slug>"). Installed required mods are enabled; missing ones are remembered by Nexus ID and
    // enabled once they're installed. Returns the profile's name; `created` says whether it is new.
    std::string import_collection(const nexus::Collection &collection, bool *created = nullptr);
    // Notes change on every keystroke, so with auto-save on they are only marked for saving: call flush().
    void set_note(Mod &mod, const std::string &note);
    bool has_pending_save() const { return pending_save_; }
    void flush(bool force = false);

    // Switching with unsaved changes (auto-save off) discards them; ask before calling.
    bool switch_profile(const std::string &name);
    bool add_profile(const std::string &name, bool copy_active, std::string *error = nullptr);
    bool rename_profile(const std::string &old_name, const std::string &new_name, std::string *error = nullptr);
    bool delete_profile(const std::string &name, std::string *error = nullptr);
    void save_active();
    void revert_active();

    // Saves the active profile's configs; returns true when any changed.
    bool capture_configs();

    // Everything needed before SMAPI starts: saves pending profile edits and rebuilds the selected-mods folder.
    SelectedModsResult prepare_launch();
    void game_exited() { capture_configs(); }
    void shutdown();

private:
    Profile &active_mut();
    void apply_profile_to_mods();
    void apply_latest_versions();
    bool resolve_pending_collection_mods(Profile &profile);
    void changed(bool defer = false);

    Settings settings_;
    ProfileStore store_{paths::profiles_dir()};
    std::vector<Mod> mods_;
    std::string active_name_ = kDefaultProfileName;
    bool dirty_ = false;        // unsaved edits waiting for "Save Profile" (auto-save off)
    bool pending_save_ = false; // auto-saved edits not written yet (see flush)
    std::map<int, std::string> latest_versions_;
    bool update_check_done_ = false;
};

} // namespace gtkdrop
