#include "stardrop_import.h"

#include <algorithm>
#include <glib.h>

#include "json_util.h"
#include "profile.h"

namespace gtkdrop {

namespace {

bool copy_new_files(const fs::path &from, const fs::path &to, int &count, std::vector<std::string> &messages) {
    std::error_code ec;
    if (!fs::is_directory(from, ec))
        return false;
    fs::create_directories(to, ec);
    for (const auto &entry: fs::directory_iterator(from, ec)) {
        if (entry.path().extension() != ".json")
            continue;
        const auto dest = to / entry.path().filename();
        std::error_code copy_ec;
        if (fs::exists(dest, copy_ec))
            continue;
        if (fs::copy_file(entry.path(), dest, copy_ec))
            ++count;
        else
            messages.push_back("Could not copy " + entry.path().filename().string() + ": " + copy_ec.message());
    }
    return true;
}

} // namespace

ImportResult import_stardrop(Settings &settings, const fs::path &stardrop_dir) {
    ImportResult result;
    const auto settings_doc = read_json_file(stardrop_dir / "Settings.json");
    std::error_code ec;
    if (!settings_doc && !fs::is_directory(stardrop_dir / "Profiles", ec))
        return result;
    result.found = true;

    if (settings_doc) {
        const json &s = *settings_doc;
        auto path_of = [&](const char *key) { return fs::path(get_string_ci(s, key)); };
        auto bool_of = [&](const char *key, bool &out) {
            if (const json *v = find_key_ci(s, key); v && v->is_boolean())
                out = v->get<bool>();
        };

        if (auto game = path_of("SMAPIFolderPath"); !game.empty() && settings.game_dir.empty())
            settings.game_dir = game;
        // Only kept when it differs from the default, so moving the game later still finds <game>/Mods.
        if (auto mods = path_of("ModFolderPath"); !mods.empty() && settings.mods_dir.empty() &&
                                                 mods.lexically_normal() != (settings.game_dir / "Mods").lexically_normal())
            settings.mods_dir = mods;
        // A custom install path outside Stardrop's folder is the user's own and is kept; Stardrop's default one is
        // copied over below.
        if (settings.collections_dir.empty())
            if (auto collections = path_of("CollectionInstallPath");
                !collections.empty() && fs::is_directory(collections, ec))
                settings.collections_dir = collections;
        if (auto last = get_string_ci(s, "LastSelectedProfileName"); !last.empty())
            settings.last_profile = last;
        bool_of("EnableProfileSpecificModConfigs", settings.profile_mod_configs);
        bool_of("ShouldAutomaticallySaveProfileChanges", settings.auto_save_profiles);
        bool_of("IgnoreHiddenFolders", settings.ignore_hidden_folders);
        bool_of("CollectionsSkipSMAPIUpdateCheck", settings.collections_skip_smapi_update_check);
    }

    copy_new_files(stardrop_dir / "Profiles", paths::profiles_dir(), result.profiles, result.messages);
    copy_new_files(stardrop_dir / "Collections" / "Cache", paths::collection_cache_dir(), result.collections,
                   result.messages);

    if (settings.collections_dir.empty())
        settings.collections_dir = stardrop_dir / "Collections" / "Mods"; // adopted (copied) right below
    if (auto message = adopt_stardrop_collections(settings, stardrop_dir); !message.empty())
        result.messages.push_back(message);
    else if (!settings.collections_dir.empty())
        result.messages.push_back("Collection mods are used from " + settings.collections_dir.string());
    return result;
}

std::string adopt_stardrop_collections(Settings &settings, const fs::path &stardrop_dir) {
    if (settings.collections_dir.empty())
        return {};
    std::error_code ec;
    const fs::path from = fs::weakly_canonical(settings.collections_dir, ec);
    const fs::path stardrop = fs::weakly_canonical(stardrop_dir, ec);
    const auto [s_end, f_it] = std::ranges::mismatch(stardrop, from);
    if (s_end != stardrop.end()) // not inside Stardrop's folder
        return {};

    const fs::path to = Settings{}.effective_collections_dir();
    int copied = 0;
    std::string failure;
    if (fs::is_directory(from, ec)) {
        fs::create_directories(to, ec);
        for (const auto &entry: fs::directory_iterator(from, ec)) {
            if (!entry.is_directory())
                continue;
            const auto dest = to / entry.path().filename();
            std::error_code copy_ec;
            if (fs::exists(dest, copy_ec))
                continue;
            fs::copy(entry.path(), dest, fs::copy_options::recursive, copy_ec);
            if (copy_ec) {
                failure = entry.path().filename().string() + ": " + copy_ec.message();
                fs::remove_all(dest, ec); // no half-copied collections
                break;
            }
            ++copied;
        }
    }
    if (!failure.empty()) {
        g_warning("Couldn't copy Stardrop's collection mods (%s); still using %s", failure.c_str(),
                  settings.collections_dir.c_str());
        return "Couldn't copy Stardrop's collection mods (" + failure + "), so they're still used from Stardrop's folder";
    }
    settings.collections_dir.clear();
    g_message("Copied %d collection folders from Stardrop into %s", copied, to.c_str());
    return copied ? "Copied " + std::to_string(copied) + " collection(s) from Stardrop into " + to.string() : "";
}

} // namespace gtkdrop
