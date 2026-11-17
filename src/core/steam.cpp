#include "steam.h"

#include "json_util.h"

#include <glib.h>

#include <regex>

namespace gtkdrop::steam {

namespace fs = std::filesystem;

namespace {

// Valve's KeyValues text format is close enough to "key" "value" pairs that every key we need can be pulled
// out with a pattern; nesting doesn't matter for these files.
std::vector<std::string> vdf_values(const std::string &text, const std::string &key) {
    std::vector<std::string> values;
    const std::regex pattern("\"" + key + "\"\\s+\"((?:[^\"\\\\]|\\\\.)*)\"", std::regex::icase);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it) {
        std::string value = (*it)[1];
        // Unescape \\ and \" as Steam writes them.
        std::string out;
        for (size_t i = 0; i < value.size(); ++i)
            out += (value[i] == '\\' && i + 1 < value.size()) ? value[++i] : value[i];
        values.push_back(out);
    }
    return values;
}

} // namespace

std::vector<fs::path> steam_roots() {
    const fs::path home = g_get_home_dir();
    const fs::path candidates[] = {
            fs::path(g_get_user_data_dir()) / "Steam",
            home / ".steam" / "steam",
            home / ".steam" / "root",
            home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
    };
    std::vector<fs::path> roots;
    for (const auto &c: candidates) {
        std::error_code ec;
        if (!fs::is_directory(c / "steamapps", ec))
            continue;
        const auto canonical = fs::canonical(c, ec);
        if (!ec && std::ranges::find(roots, canonical) == roots.end())
            roots.push_back(canonical);
    }
    return roots;
}

fs::path find_stardew_dir() {
    const std::string manifest_name = "appmanifest_" + std::to_string(kStardewAppId) + ".acf";
    for (const auto &root: steam_roots()) {
        std::vector<fs::path> libraries{root};
        if (auto vdf = read_text_file(root / "steamapps" / "libraryfolders.vdf"))
            for (const auto &p: vdf_values(*vdf, "path"))
                libraries.emplace_back(p);

        for (const auto &library: libraries) {
            const auto acf = read_text_file(library / "steamapps" / manifest_name);
            if (!acf)
                continue;
            const auto installdir = vdf_values(*acf, "installdir");
            if (installdir.empty())
                continue;
            const auto dir = library / "steamapps" / "common" / installdir.front();
            std::error_code ec;
            if (fs::is_directory(dir, ec))
                return dir;
        }
    }
    return {};
}

bool is_steam_install(const fs::path &game_dir) {
    const auto steam_dir = find_stardew_dir();
    std::error_code ec;
    return !steam_dir.empty() && !game_dir.empty() && fs::equivalent(steam_dir, game_dir, ec);
}

} // namespace gtkdrop::steam
