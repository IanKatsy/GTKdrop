#include "selected_mods.h"

#include <glib.h>

#include <set>

namespace gtkdrop {

namespace {
constexpr char kSmapiConfigOverride[] = "SMAPI-config.json";
}

SelectedModsResult build_selected_mods(const fs::path &dir, const Profile &profile, const std::vector<Mod> &mods,
                                       bool skip_collection_update_checks) {
    SelectedModsResult result;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        result.errors.push_back("Could not create " + dir.string() + ": " + ec.message());
        return result;
    }

    // Clear the previous selection. Anything that isn't one of ours is left alone.
    for (const auto &entry: fs::directory_iterator(dir, ec)) {
        std::error_code entry_ec;
        if (entry.is_symlink(entry_ec) || entry.path().filename() == kSmapiConfigOverride)
            fs::remove(entry.path(), entry_ec);
        else
            result.errors.push_back("Left " + entry.path().filename().string() + " in place: it isn't a link GTKdrop made");
        if (entry_ec)
            result.errors.push_back("Could not remove " + entry.path().string() + ": " + entry_ec.message());
    }

    std::vector<const Mod *> enabled;
    for (const auto &mod: mods)
        if (profile.is_enabled(mod))
            enabled.push_back(&mod);
    std::ranges::stable_sort(enabled, [&](const Mod *a, const Mod *b) {
        auto rank = [&](const Mod *m) {
            return m->is_from_collection() && iequals(m->source_id, profile.source_id) ? 0 : m->is_from_collection() ? 2 : 1;
        };
        return rank(a) < rank(b);
    });

    std::set<std::string> seen_ids, used_names;
    json suppressed = json::array();
    for (const Mod *mod: enabled) {
        if (!seen_ids.insert(to_lower(mod->unique_id())).second)
            continue; // SMAPI refuses duplicate IDs; the first copy wins

        // SMAPI skips folders starting with '.', so a link must never be named like one.
        std::string name = mod->folder.filename().string();
        if (name.empty() || name[0] == '.')
            name = mod->unique_id();
        if (used_names.contains(to_lower(name)))
            name += mod->is_from_collection() ? " (" + mod->source_id + ")" : " (local)";
        for (int n = 2; used_names.contains(to_lower(name)); ++n)
            name = mod->folder.filename().string() + " (" + std::to_string(n) + ")";
        used_names.insert(to_lower(name));

        std::error_code link_ec;
        fs::create_directory_symlink(fs::absolute(mod->folder, link_ec), dir / name, link_ec);
        if (link_ec) {
            result.errors.push_back("Could not link " + mod->name() + ": " + link_ec.message());
            continue;
        }
        ++result.linked;
        if (mod->is_from_collection())
            suppressed.push_back(mod->unique_id());
    }

    // SMAPI merges this over its own defaults, so only the one setting is written.
    if (skip_collection_update_checks && !suppressed.empty())
        if (!write_json_file(dir / kSmapiConfigOverride, {{"SuppressUpdateChecks", suppressed}}))
            result.errors.push_back("Could not write SMAPI-config.json; SMAPI will check collection mods for updates");

    return result;
}

} // namespace gtkdrop
