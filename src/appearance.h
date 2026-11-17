#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

namespace gtkdrop {

// Where the window's colours come from.
enum class ColorSource {
    System, // libadwaita's own colours: follows the desktop's light/dark preference and accent colour
    Theme,  // a JSON colour theme (bundled, or from the user themes folder)
};

enum class ColorStyle { Auto, Light, Dark };

struct Appearance {
    ColorSource source = ColorSource::System;
    ColorStyle style = ColorStyle::Auto;
    std::string theme = "Stardrop"; // theme id = JSON file name without extension
};

struct ThemeInfo {
    std::string id;
    std::string name;
    bool user = false; // from the user themes folder (overrides a bundled theme with the same id)
};

// Persisted in $XDG_CONFIG_HOME/gtkdrop/settings.ini.
Appearance load_appearance();
void save_appearance(const Appearance &appearance);

// $XDG_CONFIG_HOME/gtkdrop/themes — drop *.json themes here.
std::string user_themes_dir();

// Bundled + user themes, re-read from disk on every call, sorted by name.
std::vector<ThemeInfo> list_themes();

// Applies `appearance` app-wide (and tags `window` for the mod-list rules).
void apply_appearance(GtkWindow *window, const Appearance &appearance);

} // namespace gtkdrop
