#pragma once

#include "json_util.h"
#include "mod.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace gtkdrop {

namespace fs = std::filesystem;

constexpr char kDefaultProfileName[] = "Default";

// A named set of enabled mods plus per-mod notes and (optionally) a saved copy of each mod's config.json.
// Stored as one JSON file per profile in Stardrop's format, so Stardrop's files can be imported as they are.
struct Profile {
    std::string name;
    bool is_protected = false;               // can't be renamed or deleted (Default, collection profiles)
    std::string source_id;                   // set when generated from a collection: read-only, tied to it
    std::vector<ModReference> enabled;
    std::map<std::string, std::string> notes;     // lower-cased UniqueID -> note
    std::map<std::string, json> preserved_configs; // lower-cased UniqueID -> config.json contents
    // Collection profiles (GTKdrop only): Nexus mod IDs the collection needs that weren't installed yet. They are
    // enabled as soon as a mod with that Nexus ID shows up, then dropped from here. (Every mod in the collection's
    // own folder is enabled anyway; this catches ones installed elsewhere.)
    std::vector<int> pending_nexus_ids;

    bool is_from_collection() const { return !source_id.empty(); }
    bool is_enabled(const Mod &mod) const;

    json to_json() const;
    static std::optional<Profile> from_json(const json &doc);
};

// Profile files on disk. Keeps Default first and collection profiles last, like Stardrop.
class ProfileStore {
public:
    explicit ProfileStore(fs::path dir);

    void load();                       // reads every *.json, creating the Default profile if needed
    const std::vector<Profile> &profiles() const { return profiles_; }

    Profile *find(std::string_view name);
    bool name_taken(std::string_view name, const Profile *ignore = nullptr) const;

    bool save(const Profile &profile);
    Profile *add(Profile profile);             // writes it; nullptr when the name is taken
    bool rename(std::string_view old_name, const std::string &new_name);
    bool remove(std::string_view name);

    static std::string file_name_for(std::string_view profile_name);

private:
    void sort();
    fs::path path_for(std::string_view name) const { return dir_ / file_name_for(name); }

    fs::path dir_;
    std::vector<Profile> profiles_;
};

} // namespace gtkdrop
