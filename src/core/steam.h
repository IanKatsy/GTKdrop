#pragma once

#include <filesystem>
#include <vector>

namespace gtkdrop::steam {

constexpr unsigned kStardewAppId = 413150;

// Steam installs found on this machine (native, the ~/.steam link and Flatpak), without duplicates.
std::vector<std::filesystem::path> steam_roots();

// The Stardew Valley folder from Steam's library list (libraryfolders.vdf + appmanifest_413150.acf), or empty.
std::filesystem::path find_stardew_dir();

// Whether `game_dir` is the copy Steam installed (compared after resolving symlinks).
bool is_steam_install(const std::filesystem::path &game_dir);

} // namespace gtkdrop::steam
