#pragma once

// Unpacks downloaded mod archives with the system's tools (unzip, 7z, unrar, bsdtar…) and installs them.

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gtkdrop {

namespace fs = std::filesystem;

struct InstallTarget {
    enum class Kind {
        Collection, // everything goes into `dest` (the collection's own folder), so it stays out of other profiles
        Update,     // replaces the installed copy with the same UniqueID; mods not installed yet go into `dest`
    } kind;
    fs::path dest;
    std::map<std::string, fs::path> installed; // Update: lower-cased UniqueID -> folder of the loose copy
};

// What one archive (or added folder) put in place, for the library.
struct PlacedArchive {
    fs::path archive;
    std::vector<fs::path> mods;   // mod folders (holding manifest.json) it installed or replaced
    std::vector<fs::path> merged; // mod folders it added files to (a patch over a collection's main file)
    std::vector<fs::path> settings_for; // folders a settings-only archive was laid over
    bool empty() const { return mods.empty() && merged.empty() && settings_for.empty(); }
};

struct InstallResult {
    int archives = 0;  // unpacked and installed
    int mods = 0;      // mod folders placed
    std::vector<std::string> errors;
    std::vector<PlacedArchive> placed;
};

// Installs every archive in `download_dir`, one at a time. Installed archives are listed in
// download_dir/.installed (download lists and the collection check go by that record), then deleted; ones that
// failed stay for another try. Replaced mod folders go to the trash (their config.json is kept).
// `log` (optional) gets each command run, its output and where things went.
void install_archives(const fs::path &download_dir, InstallTarget target, std::function<void(InstallResult)> done,
                      std::function<void(const std::string &)> log = {});

// Installs the user's own archives and mod folders (Add Mods), unpacking in `work_dir`. The sources are left as
// they are and nothing is recorded.
void install_files(std::vector<fs::path> sources, const fs::path &work_dir, InstallTarget target,
                   std::function<void(InstallResult)> done, std::function<void(const std::string &)> log = {});

// Copies a mod folder to `to` (which must not exist), through a temporary folder next to it so a failed copy
// leaves nothing half-done. Blocking: run it off the main thread for big mods.
bool copy_mod_folder(const fs::path &from, const fs::path &to, std::string &error);

// download_dir/.installed: the archives installed from it.
std::vector<std::string> installed_archive_names(const fs::path &download_dir);
// Deletes the archives in `download_dir` that .installed lists.
void remove_installed_archives(const fs::path &download_dir);

// Which tool unpacks `archive`, e.g. {"7z", "x", …}; empty when none is installed.
std::vector<std::string> extract_command(const fs::path &archive, const fs::path &into);

} // namespace gtkdrop
