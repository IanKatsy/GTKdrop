#include "profile.h"

#include <glib.h>

#include <algorithm>

namespace gtkdrop {

bool Profile::is_enabled(const Mod &mod) const {
    return std::ranges::any_of(enabled, [&](const ModReference &r) {
        return r.matches(mod.unique_id(), mod.source_id);
    });
}

json Profile::to_json() const {
    json enabled_ids = json::array();
    for (const auto &r: enabled) {
        // Loose installs keep Stardrop's legacy bare-string form, so its files don't churn.
        if (r.source_id.empty())
            enabled_ids.push_back(r.unique_id);
        else
            enabled_ids.push_back({{"UniqueId", r.unique_id}, {"SourceId", r.source_id}});
    }

    json notes_json = json::array();
    for (const auto &[uid, note]: notes)
        notes_json.push_back({{"UniqueId", uid}, {"Note", note}});

    json configs = json::object();
    for (const auto &[uid, config]: preserved_configs)
        configs[uid] = config;

    json doc = {
        {"Name", name},
        {"IsProtected", is_protected},
        {"SourceId", source_id.empty() ? json(nullptr) : json(source_id)},
        {"EnabledModIds", enabled_ids},
        {"PreservedModConfigs", configs},
        {"Notes", notes_json},
    };
    if (!pending_nexus_ids.empty())
        doc["GTKdropPendingNexusMods"] = pending_nexus_ids;
    return doc;
}

std::optional<Profile> Profile::from_json(const json &doc) {
    if (!doc.is_object())
        return std::nullopt;

    Profile p;
    p.name = get_string_ci(doc, "Name");
    if (p.name.empty())
        return std::nullopt;
    if (const json *v = find_key_ci(doc, "IsProtected"); v && v->is_boolean())
        p.is_protected = v->get<bool>();
    p.source_id = get_string_ci(doc, "SourceId");

    if (const json *ids = find_key_ci(doc, "EnabledModIds"); ids && ids->is_array()) {
        for (const json &entry: *ids) {
            ModReference ref;
            if (entry.is_string()) {
                ref.unique_id = entry.get<std::string>();
            } else if (entry.is_object()) {
                ref.unique_id = get_string_ci(entry, "UniqueId");
                ref.source_id = get_string_ci(entry, "SourceId");
            }
            if (!ref.unique_id.empty() && std::ranges::find(p.enabled, ref) == p.enabled.end())
                p.enabled.push_back(std::move(ref));
        }
    }

    if (const json *notes = find_key_ci(doc, "Notes"); notes && notes->is_array())
        for (const json &n: *notes)
            if (auto uid = get_string_ci(n, "UniqueId"); !uid.empty())
                if (auto text = get_string_ci(n, "Note"); !text.empty())
                    p.notes[to_lower(uid)] = text;

    if (const json *configs = find_key_ci(doc, "PreservedModConfigs"); configs && configs->is_object())
        for (auto it = configs->begin(); it != configs->end(); ++it)
            p.preserved_configs[to_lower(it.key())] = it.value();

    if (const json *pending = find_key_ci(doc, "GTKdropPendingNexusMods"); pending && pending->is_array())
        for (const json &id: *pending)
            if (id.is_number_integer() && id.get<int>() > 0)
                p.pending_nexus_ids.push_back(id.get<int>());

    return p;
}

// ---------------------------------------------------------------------------
// ProfileStore
// ---------------------------------------------------------------------------
ProfileStore::ProfileStore(fs::path dir) : dir_(std::move(dir)) {}

std::string ProfileStore::file_name_for(std::string_view profile_name) {
    // '/' and NUL are the only characters a Linux file name can't hold; Stardrop replaces them the same way.
    std::string file(profile_name);
    std::ranges::replace(file, '/', '_');
    std::ranges::replace(file, '\0', '_');
    if (file == "." || file == "..")
        file = "_" + file;
    return file + ".json";
}

void ProfileStore::load() {
    profiles_.clear();
    std::error_code ec;
    fs::create_directories(dir_, ec);
    for (const auto &entry: fs::directory_iterator(dir_, ec)) {
        if (entry.path().extension() != ".json")
            continue;
        auto doc = read_json_file(entry.path());
        auto profile = doc ? Profile::from_json(*doc) : std::nullopt;
        if (!profile) {
            g_warning("Skipping unreadable profile %s", entry.path().c_str());
            continue;
        }
        if (!name_taken(profile->name))
            profiles_.push_back(std::move(*profile));
    }

    if (!find(kDefaultProfileName)) {
        Profile def;
        def.name = kDefaultProfileName;
        def.is_protected = true;
        save(def);
        profiles_.push_back(std::move(def));
    }
    sort();
}

void ProfileStore::sort() {
    std::ranges::stable_sort(profiles_, [](const Profile &a, const Profile &b) {
        auto rank = [](const Profile &p) { return p.name == kDefaultProfileName ? 0 : p.is_from_collection() ? 2 : 1; };
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        return g_utf8_collate(a.name.c_str(), b.name.c_str()) < 0;
    });
}

Profile *ProfileStore::find(std::string_view name) {
    for (auto &p: profiles_)
        if (p.name == name)
            return &p;
    return nullptr;
}

bool ProfileStore::name_taken(std::string_view name, const Profile *ignore) const {
    // Compared on the file name, since two names can differ only in characters that get replaced.
    const auto file = to_lower(file_name_for(name));
    return std::ranges::any_of(profiles_, [&](const Profile &p) {
        return &p != ignore && to_lower(file_name_for(p.name)) == file;
    });
}

bool ProfileStore::save(const Profile &profile) {
    return write_json_file(path_for(profile.name), profile.to_json());
}

Profile *ProfileStore::add(Profile profile) {
    if (profile.name.empty() || name_taken(profile.name))
        return nullptr;
    if (!save(profile))
        return nullptr;
    const std::string name = profile.name;
    profiles_.push_back(std::move(profile));
    sort();
    return find(name);
}

bool ProfileStore::rename(std::string_view old_name, const std::string &new_name) {
    Profile *p = find(old_name);
    if (!p || p->is_protected || new_name.empty() || name_taken(new_name, p))
        return false;
    const auto old_path = path_for(p->name);
    p->name = new_name;
    if (!save(*p)) {
        p->name = std::string(old_name);
        return false;
    }
    std::error_code ec;
    if (old_path != path_for(new_name))
        fs::remove(old_path, ec);
    sort();
    return true;
}

bool ProfileStore::remove(std::string_view name) {
    auto it = std::ranges::find_if(profiles_, [&](const Profile &p) { return p.name == name; });
    if (it == profiles_.end() || it->is_protected)
        return false;
    std::error_code ec;
    fs::remove(path_for(it->name), ec);
    profiles_.erase(it);
    return true;
}

} // namespace gtkdrop
