#pragma once

#include "json_util.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gtkdrop {

namespace fs = std::filesystem;

// Compares SMAPI-style versions ("1.2.3", "1.2.3-beta.1", "2.0"). <0, 0, >0 like strcmp.
int compare_versions(std::string_view a, std::string_view b);

struct Dependency {
    std::string unique_id;
    std::string minimum_version;
    bool required = true;
};

struct Manifest {
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::string unique_id;
    std::string content_pack_for; // UniqueID of the framework this is a content pack for
    std::vector<Dependency> dependencies;
    std::vector<std::string> update_keys;

    static std::optional<Manifest> parse(const json &doc);
};

// Identifies one installed copy of a mod: the unique ID alone is not enough, as a collection can install its
// own pinned copy next to a loose install. source_id is empty for loose installs.
struct ModReference {
    std::string unique_id;
    std::string source_id;

    bool matches(std::string_view uid, std::string_view source) const {
        return iequals(unique_id, uid) && iequals(source_id, source);
    }
    bool operator==(const ModReference &o) const { return matches(o.unique_id, o.source_id); }
};

struct Requirement {
    std::string unique_id;
    std::string minimum_version;
    std::string name; // display name: the installed mod's name when known, else the unique ID
    bool missing = false;
};

struct Mod {
    Manifest manifest;
    fs::path folder;            // the mod's folder (holds manifest.json)
    fs::path config_file;       // folder/config.json when it exists, else empty
    std::string source_id;      // collection this copy belongs to (folder under the collections root), or empty
    std::string collection_name;
    std::chrono::system_clock::time_point modified; // manifest.json mtime, shown as "Last Updated"
    std::vector<Requirement> requirements;          // required dependencies + ContentPackFor

    // Per-profile state, applied from the active profile.
    bool enabled = false;
    std::string note;

    // Set by an update check.
    std::string latest_version;
    bool has_update() const;

    const std::string &unique_id() const { return manifest.unique_id; }
    const std::string &name() const { return manifest.name; }
    bool is_from_collection() const { return !source_id.empty(); }
    bool has_config() const { return !config_file.empty(); }
    ModReference reference() const { return {manifest.unique_id, source_id}; }
    int nexus_id() const; // from an UpdateKeys entry like "Nexus:1915", or 0
    std::string missing_requirements_text() const;
    std::string requirements_text() const;
};

struct DiscoveryOptions {
    fs::path mods_dir;
    fs::path collections_dir;
    fs::path collection_cache_dir;
    bool ignore_hidden_folders = true;
};

// Walks the mods folder (and the collections folder when it isn't inside it), taking every folder that holds a
// manifest.json as a mod without descending into it. A duplicated (UniqueID, source) keeps the newer version.
std::vector<Mod> discover_mods(const DiscoveryOptions &options);

// Marks requirements that aren't installed (or are older than the minimum version) as missing.
void evaluate_requirements(std::vector<Mod> &mods);

} // namespace gtkdrop
