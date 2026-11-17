#pragma once

// GTKdrop's record of what it installed and where from (~/.config/gtkdrop/library.json).
//
// One entry per installed archive (or folder added by hand): which Nexus mod and file it was, the mod folders it
// put in place and, for a collection's settings pack, the folders it laid settings over. Archives are deleted once
// installed, and mods' manifests often name the wrong Nexus mod (or none), so this is what says "file 12345 is
// installed" and "this folder is Nexus mod 1915".
//
// Several versions of one mod can coexist: a loose copy in the mods folder and each collection's pinned copy in
// its own folder. Entries are per folder, so each copy keeps its own record (source_id says whose it is), and
// installing into a folder only replaces that folder's record.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gtkdrop {

namespace fs = std::filesystem;

struct LibraryMod {
    fs::path folder;       // holds manifest.json
    std::string unique_id; // from the manifest when installed
    std::string version;
    bool merged = false; // added to a folder another file already filled (main file + patch); not saved
};

struct LibraryEntry {
    std::string archive;     // the archive's file name (or the folder's name, for one added by hand)
    std::string origin;      // "nexus" (downloaded by GTKdrop) or "local" (Add Mods)
    int nexus_mod_id = 0;    // 0 when unknown
    int nexus_file_id = 0;
    std::string source_id;   // "" for loose mods, else the collection's ("stardewvalley-<slug>")
    std::string payload;     // the download list it came from, if any
    std::string installed_at; // ISO 8601, UTC
    std::vector<LibraryMod> mods;
    std::vector<fs::path> settings_for; // folders a settings-only archive was laid over

    bool present() const; // any of its folders still exists
};

class Library {
public:
    explicit Library(fs::path file);

    void load();
    bool save() const;

    // Records an install. Folders it put in place stop belonging to older entries (an update replaces a copy),
    // unless it only added to them (`merged`); entries left with nothing are dropped. Reads each mod's manifest for its UniqueID and version. Saves.
    void add(LibraryEntry entry);

    // Forgets folders that are gone (deleted, or replaced outside GTKdrop). A folder whose parent is missing too
    // is kept: the disk may just not be mounted. Returns whether anything changed (then call save()).
    bool prune();

    // The installed entry for a Nexus file usable by `source_id`: that collection's own copy or a loose one
    // (another collection's copy doesn't count).
    const LibraryEntry *find_file(int file_id, std::string_view source_id) const;
    // The entry that put `folder` in place, and that folder's record.
    const LibraryEntry *find_folder(const fs::path &folder, const LibraryMod **mod = nullptr) const;

    const std::vector<LibraryEntry> &entries() const { return entries_; }

private:
    fs::path file_;
    std::vector<LibraryEntry> entries_;
};

std::string library_timestamp(); // now, ISO 8601 UTC

} // namespace gtkdrop
