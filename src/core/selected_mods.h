#pragma once

#include "mod.h"
#include "profile.h"

#include <string>
#include <vector>

namespace gtkdrop {

struct SelectedModsResult {
    int linked = 0;
    std::vector<std::string> errors;
};

// Rebuilds the folder SMAPI loads mods from (SMAPI_MODS_PATH / --mods-path): one symlink per enabled mod.
// The first copy of a UniqueID wins, with the active profile's own collection copies ahead of loose installs.
// When `skip_collection_update_checks` is set, writes an SMAPI-config.json that suppresses SMAPI's update
// check for collection mods (a collection pins their versions). Only symlinks and that file are ever removed.
SelectedModsResult build_selected_mods(const fs::path &dir, const Profile &profile, const std::vector<Mod> &mods,
                                       bool skip_collection_update_checks);

} // namespace gtkdrop
