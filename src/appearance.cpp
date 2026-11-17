#include "appearance.h"

#include <adwaita.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>

using json = nlohmann::json;

namespace gtkdrop {
namespace {

constexpr char kBundledThemesPath[] = "/dev/gtkdrop/GTKdrop/themes/";
constexpr char kThemeExt[] = ".json";
constexpr char kThemeWindowClass[] = "stardrop-theme";

// Keys a theme's "colors" object may contain. All are optional.
constexpr const char *kColorKeys[] = {
    "window_bg", "window_fg",           // window background / text
    "view_bg", "view_fg",               // mod list background / text (default: window_*)
    "headerbar_bg", "headerbar_fg",     // title bar (default: window_*)
    "card_bg",                          // the rounded cards (default: libadwaita's translucent tint)
    "popover_bg", "popover_selected_bg",
    "border",                           // card outlines, header bar edge, separators
    "accent", "accent_fg",              // highlights, checkboxes, suggested buttons (accent_fg: text on accent)
    "list_header_bg", "row_bg", "row_alt_bg", "row_fg",
    "success", "warning", "error",      // update status colours
};

struct Theme {
    std::string id;
    std::string name;
    bool user = false;
    std::optional<bool> dark;
    std::map<std::string, GdkRGBA> colors;
};

std::string config_path(const char *leaf) {
    char *path = g_build_filename(g_get_user_config_dir(), "gtkdrop", leaf, nullptr);
    std::string result = path;
    g_free(path);
    return result;
}

double luminance(const GdkRGBA &c) { return 0.2126 * c.red + 0.7152 * c.green + 0.0722 * c.blue; }

std::string css_color(const GdkRGBA &c) {
    char *s = gdk_rgba_to_string(&c); // locale-independent
    std::string result = s;
    g_free(s);
    return result;
}

// Parses one theme file. Colours accept anything CSS does (#rgb, #rrggbbaa, rgb(), names).
std::optional<Theme> parse_theme(const std::string &id, const std::string &text, const std::string &origin) {
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (doc.is_discarded() || !doc.is_object()) {
        g_warning("%s: not a valid JSON theme", origin.c_str());
        return std::nullopt;
    }

    Theme theme;
    theme.id = id;
    theme.name = doc.contains("name") && doc["name"].is_string() ? doc["name"].get<std::string>() : id;
    if (doc.contains("dark") && doc["dark"].is_boolean())
        theme.dark = doc["dark"].get<bool>();

    if (doc.contains("colors") && doc["colors"].is_object()) {
        for (const auto &[key, value]: doc["colors"].items()) {
            if (std::ranges::find_if(kColorKeys, [&](const char *k) { return key == k; }) == std::end(kColorKeys)) {
                g_warning("%s: unknown colour key '%s'", origin.c_str(), key.c_str());
                continue;
            }
            GdkRGBA rgba;
            if (!value.is_string() || !gdk_rgba_parse(&rgba, value.get<std::string>().c_str())) {
                g_warning("%s: '%s' is not a valid colour", origin.c_str(), key.c_str());
                continue;
            }
            theme.colors[key] = rgba;
        }
    }
    return theme;
}

std::string read_resource(const std::string &path) {
    GBytes *bytes = g_resources_lookup_data(path.c_str(), G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    if (!bytes)
        return {};
    gsize size;
    const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &size));
    std::string text(data, size);
    g_bytes_unref(bytes);
    return text;
}

std::string strip_ext(const char *file) {
    std::string id = file;
    return id.substr(0, id.size() - strlen(kThemeExt));
}

// All themes by id; a user theme replaces a bundled one with the same id.
std::map<std::string, Theme> load_all_themes() {
    std::map<std::string, Theme> themes;

    char **children = g_resources_enumerate_children(kBundledThemesPath, G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    for (char **c = children; c && *c; ++c) {
        if (!g_str_has_suffix(*c, kThemeExt))
            continue;
        const std::string path = std::string(kBundledThemesPath) + *c;
        if (auto t = parse_theme(strip_ext(*c), read_resource(path), path))
            themes[t->id] = std::move(*t);
    }
    g_strfreev(children);

    const std::string dir = user_themes_dir();
    if (GDir *d = g_dir_open(dir.c_str(), 0, nullptr)) {
        while (const char *file = g_dir_read_name(d)) {
            if (!g_str_has_suffix(file, kThemeExt))
                continue;
            char *path = g_build_filename(dir.c_str(), file, nullptr);
            char *text = nullptr;
            gsize len = 0;
            if (g_file_get_contents(path, &text, &len, nullptr)) {
                if (auto t = parse_theme(strip_ext(file), std::string(text, len), path)) {
                    t->user = true;
                    themes[t->id] = std::move(*t);
                }
                g_free(text);
            }
            g_free(path);
        }
        g_dir_close(d);
    }
    return themes;
}

// Turns a theme into CSS: Adwaita's colour variables (so every widget follows), plus a few
// rules for the mod list, which has colours libadwaita has no variables for.
std::string theme_css(const Theme &t, bool &dark) {
    auto get = [&](std::initializer_list<const char *> keys) -> std::optional<GdkRGBA> {
        for (const char *k: keys)
            if (auto it = t.colors.find(k); it != t.colors.end())
                return it->second;
        return std::nullopt;
    };

    const auto window_bg = get({"window_bg"});
    dark = t.dark.value_or(window_bg ? luminance(*window_bg) < 0.5 : true);

    std::string vars;
    auto set = [&](const char *var, std::optional<GdkRGBA> c) {
        if (c)
            vars += std::string("  ") + var + ": " + css_color(*c) + ";\n";
    };

    set("--window-bg-color", window_bg);
    set("--window-fg-color", get({"window_fg"}));
    set("--view-bg-color", get({"view_bg", "window_bg"}));
    set("--view-fg-color", get({"view_fg", "row_fg", "window_fg"}));
    set("--headerbar-bg-color", get({"headerbar_bg", "window_bg"}));
    set("--headerbar-backdrop-color", get({"headerbar_bg", "window_bg"}));
    set("--headerbar-fg-color", get({"headerbar_fg", "window_fg"}));
    set("--headerbar-border-color", get({"border"}));
    // The raised-border divider under the header / above the status bar is drawn with these.
    set("--headerbar-shade-color", get({"border"}));
    set("--headerbar-darker-shade-color", get({"border"}));
    set("--card-bg-color", get({"card_bg"}));
    set("--card-fg-color", get({"window_fg"}));
    set("--dialog-bg-color", window_bg);
    set("--dialog-fg-color", get({"window_fg"}));
    set("--sidebar-bg-color", window_bg);
    set("--sidebar-fg-color", get({"window_fg"}));

    // A mostly transparent popover would be unreadable over the page; use the window colour then.
    auto popover = get({"popover_bg"});
    if (!popover || popover->alpha < 0.5)
        popover = window_bg;
    set("--popover-bg-color", popover);
    set("--popover-fg-color", get({"window_fg"}));

    const auto accent = get({"accent", "border"});
    set("--accent-bg-color", accent);
    set("--accent-color", accent);
    auto accent_fg = get({"accent_fg"});
    if (!accent_fg && accent)
        accent_fg = luminance(*accent) > 0.5 ? GdkRGBA{0, 0, 0, 1} : GdkRGBA{1, 1, 1, 1};
    set("--accent-fg-color", accent_fg);

    set("--success-color", get({"success"}));
    set("--warning-color", get({"warning"}));
    set("--error-color", get({"error"}));
    set("--destructive-color", get({"error"}));
    set("--border-color", get({"border"}));

    std::string css = ":root {\n" + vars + "}\n";

    // Mod-list rules, only for colours the theme actually sets.
    const std::string scope = std::string("window.") + kThemeWindowClass + " ";
    auto rule = [&](const char *selector, const char *prop, std::optional<GdkRGBA> c) {
        if (c)
            css += scope + selector + " { " + prop + ": " + css_color(*c) + "; }\n";
    };
    rule(".mod-list columnview > header > button", "background-color", get({"list_header_bg"}));
    rule(".mod-list columnview > listview > row", "background-color", get({"row_bg"}));
    rule(".mod-list columnview > listview > row:nth-child(even)", "background-color", get({"row_alt_bg"}));
    rule(".mod-list columnview > listview > row", "color", get({"row_fg"}));
    rule("popover listview > row:selected", "background-color", get({"popover_selected_bg"}));
    if (auto border = get({"border"}))
        css += scope + ".card { box-shadow: 0 0 0 1px " + css_color(*border) + "; }\n";
    css += scope + ".mod-list columnview > listview > row:selected"
                   " { background-color: color-mix(in srgb, var(--accent-bg-color) 40%, transparent); }\n";
    // Stardrop outlines unchecked boxes in the accent colour; Adwaita's faint outline vanishes on coloured rows.
    css += scope + "check:not(:checked)"
                   " { box-shadow: inset 0 0 0 2px color-mix(in srgb, var(--accent-bg-color) 80%, transparent); }\n";
    return css;
}

} // namespace

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
Appearance load_appearance() {
    Appearance a;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, config_path("settings.ini").c_str(), G_KEY_FILE_NONE, nullptr)) {
        char *source = g_key_file_get_string(kf, "appearance", "colors", nullptr);
        char *style = g_key_file_get_string(kf, "appearance", "style", nullptr);
        char *theme = g_key_file_get_string(kf, "appearance", "theme", nullptr);
        a.source = g_strcmp0(source, "theme") == 0 ? ColorSource::Theme : ColorSource::System;
        a.style = g_strcmp0(style, "light") == 0 ? ColorStyle::Light
                  : g_strcmp0(style, "dark") == 0 ? ColorStyle::Dark
                                                  : ColorStyle::Auto;
        if (theme && *theme)
            a.theme = theme;
        g_free(source);
        g_free(style);
        g_free(theme);
    }
    g_key_file_free(kf);
    return a;
}

void save_appearance(const Appearance &a) {
    const std::string path = config_path("settings.ini");
    char *dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    g_key_file_remove_key(kf, "appearance", "gtk-style", nullptr); // pre-libadwaita key
    g_key_file_set_string(kf, "appearance", "colors", a.source == ColorSource::Theme ? "theme" : "system");
    g_key_file_set_string(kf, "appearance", "style",
                          a.style == ColorStyle::Light ? "light" : a.style == ColorStyle::Dark ? "dark" : "auto");
    g_key_file_set_string(kf, "appearance", "theme", a.theme.c_str());
    GError *error = nullptr;
    if (!g_key_file_save_to_file(kf, path.c_str(), &error)) {
        g_warning("Could not save %s: %s", path.c_str(), error->message);
        g_error_free(error);
    }
    g_key_file_free(kf);
}

// ---------------------------------------------------------------------------
// Themes
// ---------------------------------------------------------------------------
std::string user_themes_dir() { return config_path("themes"); }

std::vector<ThemeInfo> list_themes() {
    std::vector<ThemeInfo> list;
    for (const auto &[id, t]: load_all_themes())
        list.push_back({id, t.name, t.user});
    std::ranges::sort(list, [](const ThemeInfo &a, const ThemeInfo &b) {
        return g_utf8_collate(a.name.c_str(), b.name.c_str()) < 0;
    });
    return list;
}

void apply_appearance(GtkWindow *window, const Appearance &a) {
    static GtkCssProvider *provider = nullptr;
    if (!provider) {
        provider = gtk_css_provider_new();
        // Above the app's own style.css, which AdwApplication loads at APPLICATION priority.
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }

    AdwColorScheme scheme = a.style == ColorStyle::Light ? ADW_COLOR_SCHEME_FORCE_LIGHT
                            : a.style == ColorStyle::Dark ? ADW_COLOR_SCHEME_FORCE_DARK
                                                          : ADW_COLOR_SCHEME_DEFAULT;
    std::string css;
    if (a.source == ColorSource::Theme) {
        auto themes = load_all_themes();
        if (auto it = themes.find(a.theme); it != themes.end()) {
            bool dark = false;
            css = theme_css(it->second, dark);
            // Match libadwaita's light/dark variant so colours the theme doesn't set still fit.
            scheme = dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT;
        } else {
            g_warning("Theme '%s' not found; using system colours", a.theme.c_str());
        }
    }

    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), scheme);
    gtk_css_provider_load_from_string(provider, css.c_str());
    if (css.empty())
        gtk_widget_remove_css_class(GTK_WIDGET(window), kThemeWindowClass);
    else
        gtk_widget_add_css_class(GTK_WIDGET(window), kThemeWindowClass);
}

} // namespace gtkdrop
