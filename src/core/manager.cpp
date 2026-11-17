#include "manager.h"

#include "mod_config.h"
#include "steam.h"

#include <glib.h>

namespace gtkdrop {

ModManager::LoadReport ModManager::load() {
    LoadReport report;
    settings_ = load_settings();

    if (!settings_.stardrop_import_done) {
        report.import = import_stardrop(settings_);
        settings_.stardrop_import_done = true;
    } else {
        adopt_stardrop_collections(settings_); // older imports pointed into Stardrop's folder
    }
    if (settings_.game_dir.empty()) {
        if (auto dir = steam::find_stardew_dir(); !dir.empty()) {
            settings_.game_dir = dir;
            report.game_detected = "Steam";
        } else if (auto gog_dir = gog::find_stardew_dir(); !gog_dir.empty()) {
            settings_.game_dir = gog_dir;
            report.game_detected = "GOG";
            if (settings_.launch_method.empty())
                settings_.launch_method = to_string(LaunchMethod::Gog); // nothing to ask: GOG has no client
        }
    }
    save_settings(settings_);

    store_.load();
    library_.load();
    active_name_ = store_.find(settings_.last_profile) ? settings_.last_profile : kDefaultProfileName;
    rescan();
    return report;
}

void ModManager::rescan() {
    mods_ = discover_mods({
            .mods_dir = settings_.effective_mods_dir(),
            .collections_dir = settings_.effective_collections_dir(),
            .collection_cache_dir = paths::collection_cache_dir(),
            .ignore_hidden_folders = settings_.ignore_hidden_folders,
    });
    apply_library();
    // Mods a collection profile was waiting for may have just been installed.
    for (const auto &p: store_.profiles())
        if (p.is_from_collection() || !p.pending_nexus_ids.empty())
            if (Profile *profile = store_.find(p.name); resolve_pending_collection_mods(*profile))
                store_.save(*profile);
    apply_profile_to_mods();
    apply_latest_versions();
}

void ModManager::apply_library() {
    if (library_.prune())
        library_.save();
    for (auto &mod: mods_) {
        const LibraryMod *rec = nullptr;
        const LibraryEntry *e = library_.find_folder(mod.folder, &rec);
        if (!e)
            continue;
        mod.library_file_id = e->nexus_file_id;
        mod.installed_from = e->archive;
        // An archive holding several mods (a mod with its frameworks bundled, say) names one Nexus mod for all of
        // them: only trust it over the manifest for a single-mod archive, or a manifest that names none.
        if (e->nexus_mod_id > 0 && (e->mods.size() == 1 || mod.manifest_nexus_id() == 0))
            mod.library_nexus_id = e->nexus_mod_id;
    }
}

void ModManager::set_latest_versions(std::map<int, std::string> versions) {
    latest_versions_ = std::move(versions);
    update_check_done_ = true;
    apply_latest_versions();
}

void ModManager::apply_latest_versions() {
    for (auto &mod: mods_) {
        // A collection pins the versions of the mods it installs, so they never show as outdated.
        auto it = mod.is_from_collection() ? latest_versions_.end() : latest_versions_.find(mod.nexus_id());
        mod.latest_version = it != latest_versions_.end() ? it->second : std::string();
    }
}

void ModManager::update_settings(const Settings &settings) {
    const bool paths_changed = settings.effective_mods_dir() != settings_.effective_mods_dir() ||
                               settings.effective_collections_dir() != settings_.effective_collections_dir() ||
                               settings.ignore_hidden_folders != settings_.ignore_hidden_folders;
    settings_ = settings;
    settings_.last_profile = active_name_;
    save_settings(settings_);
    if (paths_changed)
        rescan();
}

const Profile &ModManager::active() const {
    return const_cast<ModManager *>(this)->active_mut();
}

Profile &ModManager::active_mut() {
    if (Profile *p = store_.find(active_name_))
        return *p;
    active_name_ = kDefaultProfileName;
    return *store_.find(kDefaultProfileName); // load() guarantees it exists
}

void ModManager::apply_profile_to_mods() {
    const Profile &profile = active();
    for (auto &mod: mods_) {
        mod.enabled = profile.is_enabled(mod);
        auto note = profile.notes.find(to_lower(mod.unique_id()));
        mod.note = note != profile.notes.end() ? note->second : std::string();
    }
}

void ModManager::changed(bool defer) {
    if (!settings_.auto_save_profiles)
        dirty_ = true;
    else if (defer)
        pending_save_ = true;
    else
        flush(true);
}

void ModManager::flush(bool force) {
    if (!pending_save_ && !force)
        return;
    pending_save_ = false;
    store_.save(active());
}

void ModManager::set_enabled_many(const std::vector<Mod *> &mods, bool enabled) {
    Profile &profile = active_mut();
    bool any = false;
    for (Mod *mod: mods) {
        if (mod->enabled == enabled)
            continue;
        mod->enabled = enabled;
        std::erase(profile.enabled, mod->reference());
        if (enabled)
            profile.enabled.push_back(mod->reference());
        any = true;
    }
    if (any)
        changed();
}

namespace {

// The installed copy a collection profile should use for a Nexus mod: the collection's own copy first, then a
// loose install.
const Mod *find_by_nexus_id(const std::vector<Mod> &mods, int nexus_id, const std::string &source_id) {
    const Mod *found = nullptr;
    for (const auto &m: mods) {
        if (m.nexus_id() != nexus_id)
            continue;
        if (iequals(m.source_id, source_id))
            return &m;
        if (!found && !m.is_from_collection())
            found = &m;
    }
    return found;
}

} // namespace

bool ModManager::resolve_pending_collection_mods(Profile &profile) {
    bool changed_any = false;
    // Everything the collection installed into its own folder is enabled in its own profile. (Matching by Nexus ID
    // alone misses many: manifests often say "Nexus:???" or another mod's ID.) Not in copies of it, whose mods are
    // the user's choice, and not mods pinned there with Add Permanently, which the profile adding them enables.
    if (profile.is_from_collection() && profile.is_protected)
        for (const auto &m: mods_) {
            if (!iequals(m.source_id, profile.source_id) || m.unique_id().empty() ||
                std::ranges::find(profile.enabled, m.reference()) != profile.enabled.end())
                continue;
            if (const LibraryEntry *e = library_.find_folder(m.folder); e && e->origin == "pinned")
                continue;
            {
                profile.enabled.push_back(m.reference());
                changed_any = true;
            }
        }
    std::erase_if(profile.pending_nexus_ids, [&](int id) {
        const Mod *m = find_by_nexus_id(mods_, id, profile.source_id);
        if (!m)
            return false;
        if (std::ranges::find(profile.enabled, m->reference()) == profile.enabled.end())
            profile.enabled.push_back(m->reference());
        changed_any = true;
        return true;
    });
    return changed_any;
}

std::string ModManager::import_collection(const nexus::Collection &collection, bool *created) {
    const std::string source_id = std::string(nexus::kGameDomain) + "-" + collection.slug;
    Profile *profile = nullptr;
    for (const auto &p: store_.profiles())
        if (iequals(p.source_id, source_id) && p.is_protected) // the collection's own, not a user's copy
            profile = store_.find(p.name);

    if (created)
        *created = !profile;
    if (!profile) {
        Profile p;
        p.name = collection.name.empty() ? collection.slug : collection.name;
        // A plain profile may already use the name; collections never take it over.
        for (int n = 1; store_.name_taken(p.name); ++n)
            p.name = (collection.name.empty() ? collection.slug : collection.name) +
                     (n == 1 ? " (Collection)" : " (Collection " + std::to_string(n) + ")");
        p.is_protected = true;
        p.source_id = source_id;
        profile = store_.add(std::move(p));
        if (!profile)
            return {};
    }

    for (const auto &f: collection.files) {
        if (f.optional || f.mod_id <= 0 || f.mod_id == nexus::kSmapiModId)
            continue;
        if (std::ranges::find(profile->pending_nexus_ids, f.mod_id) == profile->pending_nexus_ids.end())
            profile->pending_nexus_ids.push_back(f.mod_id);
    }
    resolve_pending_collection_mods(*profile);
    store_.save(*profile);

    const std::string name = profile->name;
    if (name == active_name_)
        apply_profile_to_mods();
    return name;
}

void ModManager::set_enabled(Mod &mod, bool enabled) {
    if (mod.enabled == enabled)
        return;
    mod.enabled = enabled;
    Profile &profile = active_mut();
    const auto ref = mod.reference();
    std::erase(profile.enabled, ref);
    if (enabled)
        profile.enabled.push_back(ref);
    changed();
}

void ModManager::set_note(Mod &mod, const std::string &note) {
    if (mod.note == note)
        return;
    mod.note = note;
    Profile &profile = active_mut();
    if (note.empty())
        profile.notes.erase(to_lower(mod.unique_id()));
    else
        profile.notes[to_lower(mod.unique_id())] = note;
    changed(/*defer=*/true);
}

bool ModManager::capture_configs() {
    flush();
    if (!settings_.profile_mod_configs || !capture_mod_configs(active_mut(), mods_))
        return false;
    // Configs are saved even with auto-save off, as they aren't a pending edit. With unsaved edits, only the
    // configs go into the file on disk so those edits stay pending.
    if (!dirty_) {
        store_.save(active());
    } else {
        const auto path = paths::profiles_dir() / ProfileStore::file_name_for(active_name_);
        auto doc = read_json_file(path);
        auto on_disk = doc ? Profile::from_json(*doc) : std::nullopt;
        if (on_disk) {
            on_disk->preserved_configs = active().preserved_configs;
            store_.save(*on_disk);
        }
    }
    return true;
}

bool ModManager::switch_profile(const std::string &name) {
    if (name == active_name_ || !store_.find(name))
        return false;
    if (dirty_)
        revert_active();
    capture_configs();

    active_name_ = name;
    settings_.last_profile = name;
    save_settings(settings_);
    if (settings_.profile_mod_configs)
        apply_mod_configs(active(), mods_);
    apply_profile_to_mods();
    return true;
}

bool ModManager::add_profile(const std::string &name, bool copy_active, std::string *error) {
    Profile profile;
    if (copy_active) {
        // A copy of a collection's profile stays tied to the collection (its mods, Other Available Mods), but is
        // the user's: renamable, deletable, and left alone when the collection updates.
        profile = active();
        profile.is_protected = false;
    }
    profile.name = name;
    if (name.empty() || store_.name_taken(name)) {
        if (error)
            *error = name.empty() ? "The profile needs a name" : "A profile with that name already exists";
        return false;
    }
    if (!store_.add(std::move(profile))) {
        if (error)
            *error = "The profile could not be saved";
        return false;
    }
    return true;
}

bool ModManager::rename_profile(const std::string &old_name, const std::string &new_name, std::string *error) {
    const Profile *p = store_.find(old_name);
    if (!p || p->is_protected) {
        if (error)
            *error = "This profile can't be renamed";
        return false;
    }
    if (!store_.rename(old_name, new_name)) {
        if (error)
            *error = new_name.empty() ? "The profile needs a name" : "A profile with that name already exists";
        return false;
    }
    if (active_name_ == old_name) {
        active_name_ = new_name;
        settings_.last_profile = new_name;
        save_settings(settings_);
    }
    return true;
}

bool ModManager::delete_profile(const std::string &name, std::string *error) {
    if (name == active_name_)
        switch_profile(kDefaultProfileName);
    if (!store_.remove(name)) {
        if (error)
            *error = "This profile can't be deleted";
        return false;
    }
    return true;
}

void ModManager::save_active() {
    pending_save_ = false;
    store_.save(active());
    dirty_ = false;
}

void ModManager::revert_active() {
    store_.load(); // re-reads every file; the edits only ever touched the active one
    dirty_ = false;
    apply_profile_to_mods();
}

SelectedModsResult ModManager::prepare_launch() {
    flush();
    if (dirty_)
        save_active();
    return build_selected_mods(paths::selected_mods_dir(), active(), mods_,
                               settings_.collections_skip_smapi_update_check);
}

void ModManager::shutdown() {
    capture_configs(); // unsaved profile edits are the UI's to ask about
}

} // namespace gtkdrop
