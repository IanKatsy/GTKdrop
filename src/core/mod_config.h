#pragma once

#include "mod.h"
#include "profile.h"

#include <vector>

namespace gtkdrop {

// Per-profile mod configuration: each profile keeps a copy of the config.json of every mod it enables.
// Collection mods are skipped, as a collection owns the configuration of the mods it installs.

// Merges like Stardrop's JsonTools.Merge(base, values, false): the keys of `base` are kept, each taking its
// value from `values` when present and not null. Objects merge recursively; arrays come from `values` whole.
json merge_config(const json &base, const json &values);

// Copies the current config.json of each enabled loose mod into the profile. Returns true if anything changed.
bool capture_mod_configs(Profile &profile, const std::vector<Mod> &mods);

// Writes the profile's saved configs back into the mod folders, merged over the current files so options added
// by a mod update survive. Mods without a config.json get the saved copy as-is. Returns the number written.
int apply_mod_configs(const Profile &profile, std::vector<Mod> &mods);

} // namespace gtkdrop
