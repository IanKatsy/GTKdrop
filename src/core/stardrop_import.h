#pragma once

#include "settings.h"

#include <string>
#include <vector>

namespace gtkdrop {

struct ImportResult {
    bool found = false;         // a Stardrop data folder existed
    int profiles = 0;           // profiles copied
    int collections = 0;        // collection records copied
    std::vector<std::string> messages;
};

// Reads Stardrop's settings, profiles and collection records once. Folder paths and preferences go into
// `settings`; profile files and collection records are copied into GTKdrop's own folders (existing files win).
// Installed collection mods are copied into GTKdrop's collections folder too (see adopt_stardrop_collections).
ImportResult import_stardrop(Settings &settings, const fs::path &stardrop_dir = paths::stardrop_data_dir());

// GTKdrop keeps no paths into Stardrop's folder: when `settings.collections_dir` points inside it (older imports
// did that), its collection folders are copied into GTKdrop's own (existing ones win) and the setting is cleared.
// Returns a message for the user, or empty when nothing was done. Leaves the setting alone if copying fails.
std::string adopt_stardrop_collections(Settings &settings, const fs::path &stardrop_dir = paths::stardrop_data_dir());

} // namespace gtkdrop
