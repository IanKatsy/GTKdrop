#include "main_window.h"
#include "build_info.h"
#include "appearance.h"
#include "core/manager.h"
#include "core/nexus.h"
#include "core/http_download.h"
#include "core/installer.h"
#include "core/payload.h"
#include "core/steam.h"
#include "dialogs.h"
#include "downloads.h"
#include "log.h"
#include "toast.h"
#include "mod_item.h"

#include <adwaita.h>

#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr char kRepositoryUrl[] = "https://github.com/Floogen/Stardrop";

namespace fs = std::filesystem;

// The local paths a file dialog returned (consumes `files`).
std::vector<fs::path> paths_of(GListModel *files) {
    std::vector<fs::path> paths;
    for (guint i = 0; i < g_list_model_get_n_items(files); ++i) {
        auto *file = G_FILE(g_list_model_get_item(files, i));
        if (char *path = g_file_get_path(file)) {
            paths.emplace_back(path);
            g_free(path);
        }
        g_object_unref(file);
    }
    g_object_unref(files);
    return paths;
}

// Opens the config.json of the row's mod (the Configuration column's button).
void open_config_from(GtkWidget *widget, GtkdropModItem *mod);

// ---------------------------------------------------------------------------
// Column definitions. Each cell is a widget whose `widget_prop` is bound to
// the mod's `item_prop`; the column sorts on `item_prop` as well.
// ---------------------------------------------------------------------------
// The collection check, while it asks Nexus for the archive names of files downloaded before they were recorded.
struct CollectionCheck {
    std::vector<std::string> installed; // archives installed from the list's download folder
    std::vector<gtkdrop::nexus::CollectionFile> missing;
    std::set<int> found; // file IDs in `missing` whose archive turned out installed
    int required = 0;
    int pending = 0;
    bool has(const std::string &archive) const { return std::ranges::find(installed, archive) != installed.end(); }
};

struct ColumnSpec {
    const char *id;
    const char *title;
    GtkWidget *(*make_widget)();
    const char *item_prop;
    const char *widget_prop;
    GBindingFlags flags;
    bool expand;
    bool visible;
    bool hideable;
    int fixed_width; // -1 = natural
    void (*after_bind)(GtkWidget *, GtkdropModItem *);
};

GtkWidget *make_label() {
    GtkWidget *label = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    return label;
}

GtkWidget *make_missing_requirements_label() {
    GtkWidget *label = make_label();
    gtk_widget_add_css_class(label, "mod-missing-requirements");
    return label;
}

GtkWidget *make_check() {
    GtkWidget *check = gtk_check_button_new();
    gtk_widget_set_halign(check, GTK_ALIGN_CENTER);
    return check;
}

GtkWidget *make_config_button() {
    GtkWidget *button = gtk_button_new_with_label("Click to open");
    gtk_widget_add_css_class(button, "flat");
    g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer) {
        if (auto *mod = static_cast<GtkdropModItem *>(g_object_get_data(G_OBJECT(b), "mod-item")))
            open_config_from(GTK_WIDGET(b), mod);
    }), nullptr);
    return button;
}

GtkWidget *make_notes_entry() {
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Add a note…");
    return entry;
}

void style_status_label(GtkWidget *label, GtkdropModItem *mod) {
    static constexpr const char *classes[] = {
        nullptr, "mod-status-update", "mod-status-unofficial", "mod-status-broken",
    };
    for (const char *c: classes)
        if (c) gtk_widget_remove_css_class(label, c);
    if (const char *c = classes[gtkdrop_mod_item_get_status(mod)])
        gtk_widget_add_css_class(label, c);
}

constexpr auto kOneWay = G_BINDING_DEFAULT;
constexpr auto kTwoWay = G_BINDING_BIDIRECTIONAL;

const ColumnSpec kColumns[] = {
    // id                     title                   widget                             item prop               widget prop  flags    expand visible hideable width after_bind
    {"enabled", "", make_check, "enabled", "active", kTwoWay, false, true, false, 40, nullptr},
    {"name", "Mod Name", make_label, "name", "label", kOneWay, true, true, false, -1, nullptr},
    {"unique-id", "Unique ID", make_label, "unique-id", "label", kOneWay, false, false, true, -1, nullptr},
    {"author", "Author", make_label, "author", "label", kOneWay, false, false, true, -1, nullptr},
    {"requirements", "Requirements", make_label, "requirements", "label", kOneWay, false, false, true, -1, nullptr},
    {"last-updated", "Last Updated", make_label, "last-updated", "label", kOneWay, false, true, true, -1, nullptr},
    {"missing-requirements", "Missing Requirements", make_missing_requirements_label, "missing-requirements", "label", kOneWay, false, true, true, -1, nullptr},
    {"version", "Version", make_label, "version", "label", kOneWay, false, true, true, -1, nullptr},
    {"configuration", "Configuration", make_config_button, "has-config", "visible", kOneWay, false, false, true, -1, nullptr},
    {"status", "Status", make_label, "status-text", "label", kOneWay, false, true, true, -1, style_status_label},
    {"notes", "Notes", make_notes_entry, "notes", "text", kTwoWay, false, true, true, 180, nullptr},
};

// ---------------------------------------------------------------------------

class MainWindow {
public:
    explicit MainWindow(GtkApplication *app);
    ~MainWindow() {
        g_clear_handle_id(&note_save_id_, g_source_remove);
        g_clear_object(&context_item_);
    }

    GtkWindow *window() const { return window_; }
    void toast(const std::string &text);
    void open_mod_path(GtkdropModItem *item, int which); // 0 folder, 1 manifest, 2 config

private:
    void setup_actions();
    void setup_columns();
    void setup_models();
    void setup_filter_bar();
    void setup_context_menu();
    void setup_drop_target(GtkWidget *target);
    void setup_appearance(GtkBuilder *builder);
    void on_appearance_changed();
    void reload_theme_list();
    void on_open_themes_folder();

    // Mods & profiles (backed by manager_)
    void report_load(const gtkdrop::ModManager::LoadReport &report);
    void populate_mods();
    void populate_profiles();
    void sync_items();
    void update_save_button();
    gtkdrop::Mod *mod_for(GtkdropModItem *item);
    void on_profile_selected();
    void switch_to(const std::string &name);
    void request_switch(const std::string &name);
    gboolean on_close_request();

    void connect_mod(GtkdropModItem *mod);
    void update_counts();
    void refilter() { gtk_filter_changed(filter_, GTK_FILTER_CHANGE_DIFFERENT); }
    bool matches(GtkdropModItem *mod) const;
    bool passes_source_filter(GtkdropModItem *mod) const;
    void update_source_visibility();
    void set_all_enabled(bool enabled);
    void show_context_menu(double x, double y);
    void confirm_delete(GtkdropModItem *mod);

    template<auto Method>
    static void action_thunk(GSimpleAction *, GVariant *, gpointer self) { (static_cast<MainWindow *>(self)->*Method)(); }
    static void not_implemented(GSimpleAction *action, GVariant *, gpointer);

    void on_add_mods();
    void on_add_mod_folders();
    void install_local(std::vector<fs::path> sources);
    gtkdrop::InstallTarget loose_mods_target();
    void on_close() { gtk_window_close(window_); }
    void on_enable_all() { set_all_enabled(true); }
    void on_disable_all() { set_all_enabled(false); }
    void on_open_repository();
    void on_focus_search() { gtk_widget_grab_focus(GTK_WIDGET(search_entry_)); }
    void on_mod_toggle_state();
    void on_mod_filter_author();
    void on_mod_filter_clear();
    void on_mod_ignore_update();
    void on_mod_delete() { if (context_item_) confirm_delete(context_item_); }
    void on_mod_open_folder() { if (context_item_) open_mod_path(context_item_, 0); }
    void on_mod_open_manifest() { if (context_item_) open_mod_path(context_item_, 1); }
    void on_mod_open_config() { if (context_item_) open_mod_path(context_item_, 2); }
    void on_refresh_mods();
    void on_settings();
    void on_edit_profiles();
    void on_save_profile();
    void on_save_configs();
    void on_start_smapi();
    void on_open_smapi_log();
    void on_open_log();

    // Nexus Mods: update checks and download lists ("payloads")
public:
    void open_uri(const std::string &uri);
private:
    bool require_nexus();
    void open_path(const fs::path &path);
    void on_check_mod_updates() { check_updates({}); }
    void check_updates(std::function<void()> then);
    void on_get_all_updates() { get_updates(false); }
    void on_get_enabled_updates() { get_updates(true); }
    void get_updates(bool enabled_only);
    void write_updates_payload(bool enabled_only);
    void handle_collection_link(const gtkdrop::nexus::NxmLink &link, const std::string &uri);
    void handle_file_link(const gtkdrop::nexus::NxmLink &link);
    void collection_checked(const gtkdrop::nexus::Collection &collection, const std::string &uri,
                            const std::string &file, int required,
                            std::vector<std::pair<std::string, std::string>> list);
    void toast_with_file(const std::string &text, const fs::path &path);
    void install_downloads(const fs::path &payload, const fs::path &download_dir, const std::string &download_status);
    std::set<fs::path> installing_, install_again_; // one install per download folder; later requests wait

    // Game launching
    enum class GameState { Idle, Starting, Running };
    void launch_with(gtkdrop::LaunchMethod method, bool steam_setup_shown = false);
    void show_steam_setup(bool then_launch);
    void on_steam_setup() { show_steam_setup(false); }
    gtkdrop::LaunchMethod launched_with_ = gtkdrop::LaunchMethod::Ask;
    void ask_launch_method();
    void set_game_state(GameState state);
    gtkdrop::GameLauncher::Callbacks game_callbacks();

    GtkWindow *window_ = nullptr;
    GtkColumnView *view_ = nullptr;
    GtkLabel *counts_label_ = nullptr;
    GtkSearchEntry *search_entry_ = nullptr;
    GtkMenuButton *search_fields_button_ = nullptr;
    GtkCheckButton *field_author_ = nullptr;
    GtkCheckButton *field_name_ = nullptr;
    GtkCheckButton *field_requirements_ = nullptr;
    GtkDropDown *state_filter_ = nullptr;
    GtkCheckButton *show_updatable_ = nullptr;
    GtkCheckButton *show_all_sources_ = nullptr;
    GtkWidget *row_menu_ = nullptr;
    AdwToastOverlay *toast_overlay_ = nullptr;
    AdwToggleGroup *color_source_group_ = nullptr;
    AdwToggleGroup *style_group_ = nullptr;
    GtkDropDown *theme_dropdown_ = nullptr;
    GtkWidget *style_box_ = nullptr;
    GtkWidget *theme_box_ = nullptr;
    std::vector<gtkdrop::ThemeInfo> themes_;
    gtkdrop::Appearance appearance_;
    bool loading_appearance_ = false;

    // Owned by the model chain hanging off view_.
    GListStore *mods_ = nullptr;
    GtkFilter *filter_ = nullptr;
    GtkSelectionModel *selection_ = nullptr;
    GtkColumnViewColumn *name_column_ = nullptr;

    GtkdropModItem *context_item_ = nullptr; // row the context menu was opened on (owned ref)

    gtkdrop::ModManager manager_;
    GtkDropDown *profile_dropdown_ = nullptr;
    GtkWidget *save_profile_button_ = nullptr;
    AdwWindowTitle *window_title_ = nullptr;
    bool syncing_ = false;     // pushing manager state into the rows/selector: don't echo it back
    bool closing_ = false;     // the close was confirmed; let it through
    guint note_save_id_ = 0;   // pending deferred save of notes

    gtkdrop::GameLauncher launcher_;
    gtkdrop::nexus::Account nexus_;
    GtkButton *nexus_button_ = nullptr;
    GtkButton *update_status_button_ = nullptr;
    bool checking_updates_ = false;
    std::vector<std::string> pending_uris_; // links that arrived before the Nexus account check finished
    bool nexus_settling() const {
        using S = gtkdrop::nexus::Account::Status;
        return nexus_.status() == S::Unknown || nexus_.status() == S::Checking;
    }
    GtkLabel *smapi_label_ = nullptr;
    GtkWidget *smapi_button_ = nullptr;
    GtkWidget *content_box_ = nullptr;
};

MainWindow::MainWindow(GtkApplication *app) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/dev/gtkdrop/GTKdrop/ui/main_window.ui");
    auto get = [builder](const char *id) { return gtk_builder_get_object(builder, id); };

    window_ = GTK_WINDOW(get("window"));
    view_ = GTK_COLUMN_VIEW(get("mod_view"));
    counts_label_ = GTK_LABEL(get("counts_label"));
    search_entry_ = GTK_SEARCH_ENTRY(get("search_entry"));
    search_fields_button_ = GTK_MENU_BUTTON(get("search_fields_button"));
    field_author_ = GTK_CHECK_BUTTON(get("field_author"));
    field_name_ = GTK_CHECK_BUTTON(get("field_name"));
    field_requirements_ = GTK_CHECK_BUTTON(get("field_requirements"));
    state_filter_ = GTK_DROP_DOWN(get("state_filter"));
    show_updatable_ = GTK_CHECK_BUTTON(get("show_updatable"));
    show_all_sources_ = GTK_CHECK_BUTTON(get("show_all_sources"));
    GtkWidget *mod_card = GTK_WIDGET(get("mod_card"));
    toast_overlay_ = ADW_TOAST_OVERLAY(get("toast_overlay"));
    profile_dropdown_ = GTK_DROP_DOWN(get("profile_dropdown"));
    save_profile_button_ = GTK_WIDGET(get("save_profile_button"));
    window_title_ = ADW_WINDOW_TITLE(get("window_title"));
    smapi_label_ = GTK_LABEL(get("smapi_label"));
    smapi_button_ = GTK_WIDGET(get("smapi_button"));
    content_box_ = GTK_WIDGET(get("content_box"));
    nexus_button_ = GTK_BUTTON(get("nexus_button"));
    update_status_button_ = GTK_BUTTON(get("update_status_button"));
    gtk_label_set_label(GTK_LABEL(get("version_label")), GTKDROP_VERSION); // see build_info.h
    gtk_widget_set_tooltip_text(GTK_WIDGET(get("version_label")), ("GTKdrop " + gtkdrop::version_line()).c_str());

    gtk_window_set_application(window_, app);
    g_object_set_data_full(G_OBJECT(window_), "gtkdrop-main-window", this,
                           [](gpointer p) { delete static_cast<MainWindow *>(p); });
    setup_appearance(builder);
    g_object_unref(builder);

    setup_actions();
    setup_columns();
    setup_models();
    setup_filter_bar();
    setup_context_menu();
    setup_drop_target(mod_card);

    g_signal_connect_swapped(profile_dropdown_, "notify::selected", G_CALLBACK(+[](MainWindow *self) {
        self->on_profile_selected();
    }), this);
    g_signal_connect_swapped(window_, "close-request", G_CALLBACK(+[](MainWindow *self) {
        return self->on_close_request();
    }), this);

    const auto report = manager_.load();
    populate_profiles();
    populate_mods();
    report_load(report);

    nexus_.on_changed = [this] {
        gtk_button_set_label(nexus_button_, nexus_.status_text().c_str());
        if (!nexus_settling())
            for (auto uris = std::exchange(pending_uris_, {}); const auto &uri: uris)
                open_uri(uri);
    };
    nexus_.load(); // keyring, then a check with Nexus; the footer follows along

    gtkdrop::downloads::move_old_downloads();
    // Downloads keep going after the Download Lists window closes; say when they end.
    gtkdrop::downloads::Job::on_finished = [this](const gtkdrop::downloads::Job &job) {
        if (job.state() == gtkdrop::downloads::Job::State::Done)
            install_downloads(job.payload(), job.out_dir(), job.status());
        else
            toast(job.status());
    };

    // Opened while playing: watch that game instead of letting profile switches rewrite its configs.
    if (launcher_.attach_to_running(game_callbacks()))
        toast("Stardew Valley is running. Profiles are locked until it closes.");
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
void MainWindow::not_implemented(GSimpleAction *action, GVariant *, gpointer self) {
    static_cast<MainWindow *>(self)->toast(std::string("“") + g_action_get_name(G_ACTION(action)) +
                                           "” is not implemented yet");
}

void MainWindow::toast(const std::string &text) {
    AdwToast *toast = adw_toast_new(text.c_str());
    adw_toast_set_use_markup(toast, FALSE); // paths and mod names may contain '&' or '<'
    gtkdrop::ui::show_toast(toast_overlay_, toast);
}

void open_config_from(GtkWidget *widget, GtkdropModItem *mod) {
    auto *root = GTK_WIDGET(gtk_widget_get_root(widget));
    if (auto *self = static_cast<MainWindow *>(g_object_get_data(G_OBJECT(root), "gtkdrop-main-window")))
        self->open_mod_path(mod, 2);
}

// ---------------------------------------------------------------------------
// Colours: system (libadwaita) vs. JSON theme — the top-right header button
// ---------------------------------------------------------------------------
void MainWindow::setup_appearance(GtkBuilder *builder) {
    auto get = [builder](const char *id) { return gtk_builder_get_object(builder, id); };
    color_source_group_ = ADW_TOGGLE_GROUP(get("color_source_group"));
    style_group_ = ADW_TOGGLE_GROUP(get("style_group"));
    theme_dropdown_ = GTK_DROP_DOWN(get("theme_dropdown"));
    style_box_ = GTK_WIDGET(get("style_box"));
    theme_box_ = GTK_WIDGET(get("theme_box"));

    // Lets the dropdown's search box match on the theme name.
    gtk_drop_down_set_expression(theme_dropdown_,
                                 gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string"));

    appearance_ = gtkdrop::load_appearance();
    loading_appearance_ = true;
    adw_toggle_group_set_active_name(color_source_group_,
                                     appearance_.source == gtkdrop::ColorSource::Theme ? "theme" : "system");
    adw_toggle_group_set_active_name(style_group_, appearance_.style == gtkdrop::ColorStyle::Light  ? "light"
                                                   : appearance_.style == gtkdrop::ColorStyle::Dark ? "dark"
                                                                                                    : "auto");
    loading_appearance_ = false;
    reload_theme_list();

    auto changed = G_CALLBACK(+[](MainWindow *self) { self->on_appearance_changed(); });
    g_signal_connect_swapped(color_source_group_, "notify::active-name", changed, this);
    g_signal_connect_swapped(style_group_, "notify::active-name", changed, this);
    g_signal_connect_swapped(theme_dropdown_, "notify::selected", changed, this);
    // Re-read the theme files whenever the popover opens, so edited or new themes show up live.
    g_signal_connect_swapped(get("appearance_popover"), "show", G_CALLBACK(+[](MainWindow *self) {
        self->reload_theme_list();
    }), this);
}

void MainWindow::reload_theme_list() {
    themes_ = gtkdrop::list_themes();
    GtkStringList *names = gtk_string_list_new(nullptr);
    guint selected = GTK_INVALID_LIST_POSITION;
    for (guint i = 0; i < themes_.size(); ++i) {
        const auto &t = themes_[i];
        gtk_string_list_append(names, t.user ? (t.name + " (custom)").c_str() : t.name.c_str());
        if (t.id == appearance_.theme)
            selected = i;
    }

    loading_appearance_ = true;
    gtk_drop_down_set_model(theme_dropdown_, G_LIST_MODEL(names));
    gtk_drop_down_set_selected(theme_dropdown_, selected);
    loading_appearance_ = false;
    g_object_unref(names);

    on_appearance_changed(); // also re-applies the current theme from its (possibly edited) file
}

void MainWindow::on_appearance_changed() {
    if (loading_appearance_)
        return;
    const bool use_theme = g_strcmp0(adw_toggle_group_get_active_name(color_source_group_), "theme") == 0;
    gtk_widget_set_visible(style_box_, !use_theme);
    gtk_widget_set_visible(theme_box_, use_theme);

    const char *style = adw_toggle_group_get_active_name(style_group_);
    appearance_.source = use_theme ? gtkdrop::ColorSource::Theme : gtkdrop::ColorSource::System;
    appearance_.style = g_strcmp0(style, "light") == 0  ? gtkdrop::ColorStyle::Light
                        : g_strcmp0(style, "dark") == 0 ? gtkdrop::ColorStyle::Dark
                                                        : gtkdrop::ColorStyle::Auto;
    if (const guint i = gtk_drop_down_get_selected(theme_dropdown_); i < themes_.size())
        appearance_.theme = themes_[i].id;

    gtkdrop::apply_appearance(window_, appearance_);
    gtkdrop::save_appearance(appearance_);
}

void MainWindow::on_open_themes_folder() {
    const std::string dir = gtkdrop::user_themes_dir();
    g_mkdir_with_parents(dir.c_str(), 0700);
    GFile *file = g_file_new_for_path(dir.c_str());
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, window_, nullptr, nullptr, nullptr);
    g_object_unref(launcher);
    g_object_unref(file);
}

void MainWindow::setup_actions() {
    const GActionEntry entries[] = {
        // File
        {"add-mods", action_thunk<&MainWindow::on_add_mods>},
        {"add-mod-folders", action_thunk<&MainWindow::on_add_mod_folders>},
        {"start-smapi", action_thunk<&MainWindow::on_start_smapi>},
        {"close", action_thunk<&MainWindow::on_close>},
        // View
        {"settings", action_thunk<&MainWindow::on_settings>},
        {"open-log", action_thunk<&MainWindow::on_open_log>},
        {"open-smapi-log", action_thunk<&MainWindow::on_open_smapi_log>},
        {"steam-setup", action_thunk<&MainWindow::on_steam_setup>},
        // Tools
        {"refresh-mods", action_thunk<&MainWindow::on_refresh_mods>},
        {"check-mod-updates", action_thunk<&MainWindow::on_check_mod_updates>},
        {"enable-all", action_thunk<&MainWindow::on_enable_all>},
        {"disable-all", action_thunk<&MainWindow::on_disable_all>},
        {"check-stardrop-update", not_implemented},
        {"check-smapi-update", not_implemented},
        {"open-repository", action_thunk<&MainWindow::on_open_repository>},
        // Nexus Mods
        {"nexus-connection", action_thunk<&MainWindow::on_settings>},
        {"nexus-install-all", action_thunk<&MainWindow::on_get_all_updates>},
        {"nexus-install-enabled", action_thunk<&MainWindow::on_get_enabled_updates>},
        {"switch-profile", +[](GSimpleAction *, GVariant *name, gpointer self) {
            static_cast<MainWindow *>(self)->request_switch(g_variant_get_string(name, nullptr));
        }, "s"},
        {"open-file", +[](GSimpleAction *, GVariant *path, gpointer self) {
            static_cast<MainWindow *>(self)->open_path(g_variant_get_string(path, nullptr));
        }, "s"},
        {"collections", +[](GSimpleAction *, GVariant *, gpointer self) {
            gtkdrop::ui::show_payloads_dialog(GTK_WIDGET(static_cast<MainWindow *>(self)->window_), static_cast<MainWindow *>(self)->nexus_);
        }},
        // Profile bar
        {"edit-profiles", action_thunk<&MainWindow::on_edit_profiles>},
        {"save-configs", action_thunk<&MainWindow::on_save_configs>},
        {"save-profile", action_thunk<&MainWindow::on_save_profile>},
        {"focus-search", action_thunk<&MainWindow::on_focus_search>},
        {"open-themes-folder", action_thunk<&MainWindow::on_open_themes_folder>},
        // Row context menu
        {"mod-open-folder", action_thunk<&MainWindow::on_mod_open_folder>},
        {"mod-open-page", not_implemented},
        {"mod-open-manifest", action_thunk<&MainWindow::on_mod_open_manifest>},
        {"mod-open-config", action_thunk<&MainWindow::on_mod_open_config>},
        {"mod-filter-author", action_thunk<&MainWindow::on_mod_filter_author>},
        {"mod-filter-clear", action_thunk<&MainWindow::on_mod_filter_clear>},
        {"mod-ignore-update", action_thunk<&MainWindow::on_mod_ignore_update>},
        {"mod-toggle-state", action_thunk<&MainWindow::on_mod_toggle_state>},
        {"mod-delete", action_thunk<&MainWindow::on_mod_delete>},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(window_), entries, G_N_ELEMENTS(entries), this);
}

void MainWindow::on_add_mods() {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Add Mods");

    GtkFileFilter *archives = gtk_file_filter_new();
    gtk_file_filter_set_name(archives, "Mod archives");
    gtk_file_filter_add_suffix(archives, "zip");
    gtk_file_filter_add_suffix(archives, "7z");
    gtk_file_filter_add_suffix(archives, "rar");
    gtk_file_dialog_set_default_filter(dialog, archives);
    g_object_unref(archives);

    gtk_file_dialog_open_multiple(dialog, window_, nullptr, [](GObject *src, GAsyncResult *res, gpointer data) {
        if (GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, nullptr))
            static_cast<MainWindow *>(data)->install_local(paths_of(files));
    }, this);
    g_object_unref(dialog);
}

// Unpacked mods: a mod's own folder (with manifest.json), or a folder holding several.
void MainWindow::on_add_mod_folders() {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Add Mod Folders");
    gtk_file_dialog_select_multiple_folders(dialog, window_, nullptr, [](GObject *src, GAsyncResult *res,
                                                                         gpointer data) {
        if (GListModel *files = gtk_file_dialog_select_multiple_folders_finish(GTK_FILE_DIALOG(src), res, nullptr))
            static_cast<MainWindow *>(data)->install_local(paths_of(files));
    }, this);
    g_object_unref(dialog);
}

// Loose mods (not a collection's): new ones go into the mods folder, ones with an installed UniqueID replace it.
gtkdrop::InstallTarget MainWindow::loose_mods_target() {
    gtkdrop::InstallTarget target{gtkdrop::InstallTarget::Kind::Update, manager_.settings().effective_mods_dir()};
    for (const auto &m: manager_.mods())
        if (!m.is_from_collection())
            target.installed.emplace(gtkdrop::to_lower(m.unique_id()), m.folder);
    return target;
}

void MainWindow::install_local(std::vector<fs::path> sources) {
    if (sources.empty())
        return;
    if (launcher_.busy() || launcher_.running()) {
        toast("Close the game before adding mods");
        return;
    }
    // Already in the mods folder: installing it would trash the original and put a copy of it back.
    const fs::path mods_dir = fs::weakly_canonical(manager_.settings().effective_mods_dir());
    std::erase_if(sources, [&](const fs::path &p) {
        const auto rel = fs::weakly_canonical(p).lexically_relative(mods_dir);
        const bool inside = !rel.empty() && *rel.begin() != "..";
        if (inside)
            toast("“" + p.filename().string() + "” is already in the mods folder");
        return inside;
    });
    if (sources.empty())
        return;
    toast(sources.size() == 1 ? "Installing " + sources.front().filename().string() + "…"
                              : "Installing " + std::to_string(sources.size()) + " items…");
    gtkdrop::install_files(std::move(sources), gtkdrop::downloads::downloads_dir() / ".add-mods",
                           loose_mods_target(), [this](gtkdrop::InstallResult r) {
        manager_.flush();
        manager_.rescan();
        populate_mods();
        for (const auto &e: r.errors)
            g_warning("Add Mods: %s", e.c_str());
        std::string text = "Installed " + std::to_string(r.mods) + (r.mods == 1 ? " mod" : " mods");
        if (!r.errors.empty())
            text += r.errors.size() == 1 ? " · " + r.errors.front()
                                         : " · " + std::to_string(r.errors.size()) + " problems, e.g. " + r.errors.front();
        toast(text);
    }, [](const std::string &line) { g_message("Add Mods: %s", line.c_str()); });
}

void MainWindow::on_open_repository() {
    GtkUriLauncher *launcher = gtk_uri_launcher_new(kRepositoryUrl);
    gtk_uri_launcher_launch(launcher, window_, nullptr, nullptr, nullptr);
    g_object_unref(launcher);
}

// Only the mods this profile shows: collection mods hidden by the source filter are left alone, so Enable All in
// Default doesn't pull in every collection's copies.
void MainWindow::set_all_enabled(bool enabled) {
    std::vector<gtkdrop::Mod *> targets;
    GListModel *model = G_LIST_MODEL(mods_);
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        auto *item = GTKDROP_MOD_ITEM(g_list_model_get_item(model, i));
        if (passes_source_filter(item))
            if (const gtkdrop::Mod *m = mod_for(item))
                targets.push_back(const_cast<gtkdrop::Mod *>(m));
        g_object_unref(item);
    }
    manager_.set_enabled_many(targets, enabled); // one profile write instead of one per row
    sync_items();
}

// Selecting it in the profile selector goes through the usual unsaved-changes question.
void MainWindow::request_switch(const std::string &name) {
    GListModel *names = gtk_drop_down_get_model(profile_dropdown_);
    for (guint i = 0; i < g_list_model_get_n_items(names); ++i) {
        auto *item = GTK_STRING_OBJECT(g_list_model_get_item(names, i));
        const bool match = name == gtk_string_object_get_string(item);
        g_object_unref(item);
        if (match) {
            gtk_drop_down_set_selected(profile_dropdown_, i);
            return;
        }
    }
}

void MainWindow::on_mod_toggle_state() {
    if (context_item_)
        gtkdrop_mod_item_set_enabled(context_item_, !gtkdrop_mod_item_get_enabled(context_item_));
}

void MainWindow::on_mod_filter_author() {
    if (!context_item_)
        return;
    gtk_check_button_set_active(field_author_, TRUE);
    gtk_check_button_set_active(field_name_, FALSE);
    gtk_check_button_set_active(field_requirements_, FALSE);
    gtk_editable_set_text(GTK_EDITABLE(search_entry_), gtkdrop_mod_item_get_author(context_item_));
}

void MainWindow::on_mod_filter_clear() {
    gtk_editable_set_text(GTK_EDITABLE(search_entry_), "");
}

void MainWindow::on_mod_ignore_update() {
    if (!context_item_)
        return;
    gtkdrop_mod_item_set_status(context_item_, GTKDROP_MOD_STATUS_NONE, "Update Ignored");
    update_counts();
    refilter();
}

void MainWindow::confirm_delete(GtkdropModItem *mod) {
    const gtkdrop::Mod *target = mod_for(mod);
    if (!target)
        return;
    if (target->is_from_collection()) {
        toast("“" + target->name() + "” belongs to the collection “" + target->collection_name +
              "”; remove it through the collection instead");
        return;
    }
    AdwDialog *dialog = adw_alert_dialog_new(nullptr, "Its folder will be moved to the trash.");
    adw_alert_dialog_format_heading(ADW_ALERT_DIALOG(dialog), "Delete %s?", gtkdrop_mod_item_get_name(mod));
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "delete", "_Delete", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct Request {
        MainWindow *self;
        GtkdropModItem *mod;
    };
    auto *req = new Request{this, GTKDROP_MOD_ITEM(g_object_ref(mod))};

    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window_), nullptr,
                            [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *req = static_cast<Request *>(data);
        if (g_str_equal(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(src), res), "delete")) {
            if (const gtkdrop::Mod *m = req->self->mod_for(req->mod)) {
                GFile *folder = g_file_new_for_path(m->folder.c_str());
                GError *error = nullptr;
                if (g_file_trash(folder, nullptr, &error)) {
                    req->self->toast("Moved “" + m->name() + "” to the trash");
                    req->self->on_refresh_mods();
                } else {
                    req->self->toast("Could not delete “" + m->name() + "”: " + error->message);
                    g_error_free(error);
                }
                g_object_unref(folder);
            }
        }
        g_object_unref(req->mod);
        delete req;
    }, req);
}

// ---------------------------------------------------------------------------
// Mod list
// ---------------------------------------------------------------------------
void MainWindow::setup_columns() {
    GMenu *header_menu = g_menu_new();
    GMenu *columns_section = g_menu_new();
    g_menu_append_section(header_menu, "Show / Hide Columns", G_MENU_MODEL(columns_section));

    for (const ColumnSpec &spec: kColumns) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
        const gpointer spec_ptr = const_cast<ColumnSpec *>(&spec);

        g_signal_connect(factory, "setup", G_CALLBACK(+[](GtkListItemFactory *, GtkListItem *li, gpointer data) {
            gtk_list_item_set_child(li, static_cast<const ColumnSpec *>(data)->make_widget());
        }), spec_ptr);

        g_signal_connect(factory, "bind", G_CALLBACK(+[](GtkListItemFactory *, GtkListItem *li, gpointer data) {
            auto *spec = static_cast<const ColumnSpec *>(data);
            GtkWidget *child = gtk_list_item_get_child(li);
            auto *mod = GTKDROP_MOD_ITEM(gtk_list_item_get_item(li));
            // Lets the context menu and cell buttons find their row again.
            g_object_set_data(G_OBJECT(child), "list-item", li);
            g_object_set_data(G_OBJECT(child), "mod-item", mod);
            GBinding *binding = g_object_bind_property(mod, spec->item_prop, child, spec->widget_prop,
                                                       static_cast<GBindingFlags>(spec->flags | G_BINDING_SYNC_CREATE));
            g_object_set_data(G_OBJECT(child), "binding", binding);
            if (spec->after_bind)
                spec->after_bind(child, mod);
        }), spec_ptr);

        g_signal_connect(factory, "unbind", G_CALLBACK(+[](GtkListItemFactory *, GtkListItem *li, gpointer) {
            GtkWidget *child = gtk_list_item_get_child(li);
            if (auto *binding = static_cast<GBinding *>(g_object_get_data(G_OBJECT(child), "binding")))
                g_binding_unbind(binding);
            g_object_set_data(G_OBJECT(child), "binding", nullptr);
            g_object_set_data(G_OBJECT(child), "list-item", nullptr);
            g_object_set_data(G_OBJECT(child), "mod-item", nullptr);
        }), nullptr);

        GtkColumnViewColumn *column = gtk_column_view_column_new(spec.title, factory);
        gtk_column_view_column_set_id(column, spec.id);
        gtk_column_view_column_set_expand(column, spec.expand);
        gtk_column_view_column_set_resizable(column, TRUE);
        gtk_column_view_column_set_visible(column, spec.visible);
        if (spec.fixed_width > 0)
            gtk_column_view_column_set_fixed_width(column, spec.fixed_width);
        gtk_column_view_column_set_header_menu(column, G_MENU_MODEL(header_menu));

        auto *mod_class = static_cast<GObjectClass *>(g_type_class_ref(GTKDROP_TYPE_MOD_ITEM));
        GParamSpec *pspec = g_object_class_find_property(mod_class, spec.item_prop);
        g_type_class_unref(mod_class);
        GtkExpression *expr = gtk_property_expression_new(GTKDROP_TYPE_MOD_ITEM, nullptr, spec.item_prop);
        GtkSorter *sorter = pspec->value_type == G_TYPE_STRING
                                ? GTK_SORTER(gtk_string_sorter_new(expr))
                                : GTK_SORTER(gtk_numeric_sorter_new(expr));
        gtk_column_view_column_set_sorter(column, sorter);
        g_object_unref(sorter);

        if (spec.hideable) {
            // A stateful action mirroring the column's "visible" property drives the header menu checkbox.
            char *action_name = g_strdup_printf("column-%s", spec.id);
            GPropertyAction *action = g_property_action_new(action_name, column, "visible");
            g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(action));
            g_object_unref(action);

            char *detailed = g_strdup_printf("win.%s", action_name);
            g_menu_append(columns_section, spec.title, detailed);
            g_free(detailed);
            g_free(action_name);
        }

        gtk_column_view_append_column(view_, column);
        if (g_str_equal(spec.id, "name"))
            name_column_ = column;
        g_object_unref(column);
    }

    g_object_unref(columns_section);
    g_object_unref(header_menu);
}

void MainWindow::setup_models() {
    mods_ = g_list_store_new(GTKDROP_TYPE_MOD_ITEM);
    filter_ = GTK_FILTER(gtk_custom_filter_new([](gpointer item, gpointer self) -> gboolean {
        return static_cast<MainWindow *>(self)->matches(GTKDROP_MOD_ITEM(item));
    }, this, nullptr));

    // store -> filter -> sort (driven by the clicked column header) -> selection -> view
    GtkFilterListModel *filtered = gtk_filter_list_model_new(G_LIST_MODEL(mods_), filter_);
    GtkSorter *view_sorter = GTK_SORTER(g_object_ref(gtk_column_view_get_sorter(view_)));
    GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(filtered), view_sorter);
    selection_ = GTK_SELECTION_MODEL(gtk_multi_selection_new(G_LIST_MODEL(sorted)));
    gtk_column_view_set_model(view_, selection_);
    g_object_unref(selection_);

    gtk_column_view_sort_by_column(view_, name_column_, GTK_SORT_ASCENDING);
}

// Stardrop's mod source filter: collection mods only show where they belong. In a collection's own profile that
// is its mods plus whatever else the profile enables; in any other profile, collection mods it enables. Taken as
// a snapshot per profile switch (update_source_visibility) so unticking a mod doesn't make its row vanish.
bool MainWindow::passes_source_filter(GtkdropModItem *mod) const {
    return gtk_check_button_get_active(show_all_sources_) || g_object_get_data(G_OBJECT(mod), "source-visible");
}

void MainWindow::update_source_visibility() {
    const gtkdrop::Profile &profile = manager_.active();
    bool any_collection_mods = false;
    GListModel *model = G_LIST_MODEL(mods_);
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        auto *item = GTKDROP_MOD_ITEM(g_list_model_get_item(model, i));
        if (const gtkdrop::Mod *m = mod_for(item)) {
            any_collection_mods |= m->is_from_collection();
            const bool visible = profile.is_from_collection()
                                     ? gtkdrop::iequals(m->source_id, profile.source_id) || m->enabled
                                     : !m->is_from_collection() || m->enabled;
            g_object_set_data(G_OBJECT(item), "source-visible", GINT_TO_POINTER(visible));
        }
        g_object_unref(item);
    }
    gtk_widget_set_visible(GTK_WIDGET(show_all_sources_), any_collection_mods);
}

bool MainWindow::matches(GtkdropModItem *mod) const {
    if (!passes_source_filter(mod))
        return false;
    const bool enabled = gtkdrop_mod_item_get_enabled(mod);
    switch (gtk_drop_down_get_selected(state_filter_)) {
        case 1: if (!gtkdrop_mod_item_get_has_config(mod)) return false;
            break;
        case 2: if (enabled) return false;
            break;
        case 3: if (!enabled) return false;
            break;
        default: break;
    }

    if (gtk_check_button_get_active(show_updatable_) &&
        gtkdrop_mod_item_get_status(mod) == GTKDROP_MOD_STATUS_NONE)
        return false;

    const char *query = gtk_editable_get_text(GTK_EDITABLE(search_entry_));
    if (!query || !*query)
        return true;

    char *needle = g_utf8_casefold(query, -1);
    auto contains = [needle](const char *haystack) {
        char *folded = g_utf8_casefold(haystack, -1);
        const bool hit = strstr(folded, needle) != nullptr;
        g_free(folded);
        return hit;
    };

    bool by_author = gtk_check_button_get_active(field_author_);
    bool by_name = gtk_check_button_get_active(field_name_);
    bool by_reqs = gtk_check_button_get_active(field_requirements_);
    if (!by_author && !by_name && !by_reqs)
        by_name = true; // same default as Stardrop when no search filter is selected

    const bool hit = (by_name && contains(gtkdrop_mod_item_get_name(mod))) ||
                     (by_author && contains(gtkdrop_mod_item_get_author(mod))) ||
                     (by_reqs && contains(gtkdrop_mod_item_get_requirements(mod)));
    g_free(needle);
    return hit;
}

void MainWindow::setup_filter_bar() {
    g_signal_connect_swapped(search_entry_, "search-changed", G_CALLBACK(+[](MainWindow *self) {
        self->refilter();
    }), this);
    g_signal_connect_swapped(state_filter_, "notify::selected", G_CALLBACK(+[](MainWindow *self) {
        self->refilter();
    }), this);
    g_signal_connect_swapped(show_updatable_, "toggled", G_CALLBACK(+[](MainWindow *self) {
        self->refilter();
    }), this);
    g_signal_connect_swapped(show_all_sources_, "toggled", G_CALLBACK(+[](MainWindow *self) {
        self->update_counts();
        self->refilter();
    }), this);

    for (GtkCheckButton *field: {field_author_, field_name_, field_requirements_}) {
        g_signal_connect_swapped(field, "toggled", G_CALLBACK(+[](MainWindow *self) {
            int active = 0;
            for (GtkCheckButton *f: {self->field_author_, self->field_name_, self->field_requirements_})
                active += gtk_check_button_get_active(f);
            char *label = active ? g_strdup_printf("%d Filter(s) Active", active) : g_strdup("No Filters Active");
            gtk_menu_button_set_label(self->search_fields_button_, label);
            g_free(label);
            self->refilter();
        }), this);
    }
}

void MainWindow::connect_mod(GtkdropModItem *mod) {
    g_signal_connect(mod, "notify::enabled", G_CALLBACK(+[](GtkdropModItem *item, GParamSpec *, gpointer data) {
        auto *self = static_cast<MainWindow *>(data);
        if (self->syncing_)
            return;
        if (gtkdrop::Mod *m = self->mod_for(item))
            self->manager_.set_enabled(*m, gtkdrop_mod_item_get_enabled(item));
        self->update_counts();
        self->update_save_button();
        if (gtk_drop_down_get_selected(self->state_filter_) >= 2)
            self->refilter();
    }), this);
    g_signal_connect(mod, "notify::notes", G_CALLBACK(+[](GtkdropModItem *item, GParamSpec *, gpointer data) {
        auto *self = static_cast<MainWindow *>(data);
        if (self->syncing_)
            return;
        if (gtkdrop::Mod *m = self->mod_for(item)) {
            char *notes = nullptr;
            g_object_get(item, "notes", &notes, nullptr);
            self->manager_.set_note(*m, notes ? notes : "");
            g_free(notes);
        }
        self->update_save_button();
        // Written once typing pauses rather than on every keystroke.
        g_clear_handle_id(&self->note_save_id_, g_source_remove);
        self->note_save_id_ = g_timeout_add_once(800, [](gpointer data) {
            auto *self = static_cast<MainWindow *>(data);
            self->note_save_id_ = 0;
            self->manager_.flush();
        }, self);
    }), this);
}

void MainWindow::update_counts() {
    GListModel *model = G_LIST_MODEL(mods_);
    guint total = 0, enabled = 0, updatable = 0;
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        auto *mod = GTKDROP_MOD_ITEM(g_list_model_get_item(model, i));
        if (!passes_source_filter(mod)) {
            g_object_unref(mod);
            continue;
        }
        ++total;
        enabled += gtkdrop_mod_item_get_enabled(mod);
        updatable += gtkdrop_mod_item_get_status(mod) != GTKDROP_MOD_STATUS_NONE;
        g_object_unref(mod);
    }
    char *text = g_strdup_printf("Enabled Mods: %u  |  Total Mods: %u  |  Mods Ready to Update: %u",
                                 enabled, total, updatable);
    gtk_label_set_label(counts_label_, text);
    g_free(text);
}

// ---------------------------------------------------------------------------
// Row context menu
// ---------------------------------------------------------------------------
void MainWindow::setup_context_menu() {
    row_menu_ = gtk_popover_menu_new_from_model(nullptr);
    gtk_popover_set_has_arrow(GTK_POPOVER(row_menu_), FALSE);
    gtk_widget_set_halign(row_menu_, GTK_ALIGN_START);
    gtk_widget_set_parent(row_menu_, GTK_WIDGET(view_));
    g_signal_connect_swapped(view_, "destroy", G_CALLBACK(gtk_widget_unparent), row_menu_);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick *, int, double x, double y, gpointer self) {
        static_cast<MainWindow *>(self)->show_context_menu(x, y);
    }), this);
    gtk_widget_add_controller(GTK_WIDGET(view_), GTK_EVENT_CONTROLLER(click));
}

void MainWindow::show_context_menu(double x, double y) {
    // Walk up from the widget under the pointer to the cell widget our factory tagged.
    GtkWidget *w = gtk_widget_pick(GTK_WIDGET(view_), x, y, GTK_PICK_DEFAULT);
    while (w && w != GTK_WIDGET(view_) && !g_object_get_data(G_OBJECT(w), "list-item"))
        w = gtk_widget_get_parent(w);
    if (!w || w == GTK_WIDGET(view_))
        return;

    auto *li = GTK_LIST_ITEM(g_object_get_data(G_OBJECT(w), "list-item"));
    const guint pos = gtk_list_item_get_position(li);
    if (!gtk_selection_model_is_selected(selection_, pos))
        gtk_selection_model_select_item(selection_, pos, TRUE);

    g_set_object(&context_item_, GTKDROP_MOD_ITEM(gtk_list_item_get_item(li)));
    GtkdropModItem *mod = context_item_;

    GMenu *menu = g_menu_new();

    GMenu *open = g_menu_new();
    g_menu_append(open, "Open Containing Folder", "win.mod-open-folder");
    g_menu_append(open, "Open Mod Page", "win.mod-open-page");
    g_menu_append(open, "Open Manifest File", "win.mod-open-manifest");
    if (gtkdrop_mod_item_get_has_config(mod))
        g_menu_append(open, "Open Config File", "win.mod-open-config");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(open));

    GMenu *filters = g_menu_new();
    g_menu_append(filters, "Show Author's Mods", "win.mod-filter-author");
    g_menu_append(filters, "Clear Filter", "win.mod-filter-clear");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(filters));

    GMenu *options = g_menu_new();
    if (gtkdrop_mod_item_get_status(mod) != GTKDROP_MOD_STATUS_NONE)
        g_menu_append(options, "Ignore Update", "win.mod-ignore-update");
    g_menu_append(options, gtkdrop_mod_item_get_enabled(mod) ? "Disable" : "Enable", "win.mod-toggle-state");
    g_menu_append(options, "Delete…", "win.mod-delete");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(options));

    gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(row_menu_), G_MENU_MODEL(menu));
    g_object_unref(open);
    g_object_unref(filters);
    g_object_unref(options);
    g_object_unref(menu);

    const GdkRectangle rect{static_cast<int>(x), static_cast<int>(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(row_menu_), &rect);
    gtk_popover_popup(GTK_POPOVER(row_menu_));
}

// ---------------------------------------------------------------------------
// Drag & drop mod archives onto the list
// ---------------------------------------------------------------------------
void MainWindow::setup_drop_target(GtkWidget *target) {
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(+[](GtkDropTarget *, const GValue *value, double, double, gpointer data) {
        auto *list = static_cast<GdkFileList *>(g_value_get_boxed(value));
        GSList *files = gdk_file_list_get_files(list);
        for (GSList *l = files; l; l = l->next) {
            char *path = g_file_get_parse_name(G_FILE(l->data));
            static_cast<MainWindow *>(data)->toast(std::string("Installing ") + path + " is not implemented yet");
            g_free(path);
        }
        g_slist_free(files);
        return TRUE;
    }), this);
    gtk_widget_add_controller(target, GTK_EVENT_CONTROLLER(drop));
}

// ---------------------------------------------------------------------------
// Mods & profiles
// ---------------------------------------------------------------------------
gtkdrop::Mod *MainWindow::mod_for(GtkdropModItem *item) {
    // Rows are rebuilt on every rescan, so the index stays valid for a row's lifetime.
    const auto i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), "mod-index"));
    auto &mods = manager_.mods();
    return i < mods.size() ? &mods[i] : nullptr;
}

void MainWindow::populate_mods() {
    syncing_ = true;
    const auto &mods = manager_.mods();
    std::vector<gpointer> items;
    items.reserve(mods.size());
    for (guint i = 0; i < mods.size(); ++i) {
        const gtkdrop::Mod &m = mods[i];
        // A collection's pinned copy can sit next to a loose install of the same mod: say which is which.
        const std::string name = m.is_from_collection() ? m.name() + " (" + m.collection_name + ")" : m.name();

        GDateTime *dt = g_date_time_new_from_unix_local(
                std::chrono::duration_cast<std::chrono::seconds>(m.modified.time_since_epoch()).count());
        char *updated = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M") : g_strdup("");
        if (dt)
            g_date_time_unref(dt);

        GtkdropModItem *item = gtkdrop_mod_item_new(m.unique_id().c_str(), name.c_str(), m.manifest.author.c_str(),
                                                    m.manifest.version.c_str());
        g_object_set(item,
                     "enabled", m.enabled,
                     "has-config", m.has_config(),
                     "last-updated", updated,
                     "requirements", m.requirements_text().c_str(),
                     "missing-requirements", m.missing_requirements_text().c_str(),
                     "notes", m.note.c_str(),
                     nullptr);
        g_free(updated);
        if (m.has_update())
            gtkdrop_mod_item_set_status(item, GTKDROP_MOD_STATUS_UPDATE,
                                        ("Update Available (" + m.latest_version + ")").c_str());
        g_object_set_data(G_OBJECT(item), "mod-index", GUINT_TO_POINTER(i));
        connect_mod(item);
        items.push_back(item);
    }
    // One splice instead of an insert per row: the view re-sorts once and keeps its scroll position at the top.
    g_list_store_splice(mods_, 0, g_list_model_get_n_items(G_LIST_MODEL(mods_)), items.data(), items.size());
    for (gpointer item: items)
        g_object_unref(item);
    update_source_visibility();
    syncing_ = false;
    update_counts();
    update_save_button();
    refilter();
}

// Pushes the manager's per-profile state (enabled, notes) into the existing rows.
void MainWindow::sync_items() {
    syncing_ = true;
    GListModel *model = G_LIST_MODEL(mods_);
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        auto *item = GTKDROP_MOD_ITEM(g_list_model_get_item(model, i));
        if (const gtkdrop::Mod *m = mod_for(item))
            g_object_set(item, "enabled", m->enabled, "notes", m->note.c_str(), nullptr);
        g_object_unref(item);
    }
    syncing_ = false;
    update_counts();
    update_save_button();
    refilter();
}

void MainWindow::populate_profiles() {
    syncing_ = true;
    const auto &profiles = manager_.profiles();
    guint selected = 0;
    for (guint i = 0; i < profiles.size(); ++i)
        if (profiles[i].name == manager_.active().name)
            selected = i;

    // The model is only replaced when the list of names changed. This also runs from the dropdown's own
    // notify::selected handler (a profile switch), and replacing the model there frees the item the dropdown
    // is still notifying about (GLib-GObject-CRITICAL: g_object_notify_by_pspec on a dead object).
    GListModel *current = gtk_drop_down_get_model(profile_dropdown_);
    bool same = current && g_list_model_get_n_items(current) == profiles.size();
    for (guint i = 0; same && i < profiles.size(); ++i)
        same = profiles[i].name == gtk_string_list_get_string(GTK_STRING_LIST(current), i);
    if (!same) {
        GtkStringList *names = gtk_string_list_new(nullptr);
        for (const auto &p: profiles)
            gtk_string_list_append(names, p.name.c_str());
        gtk_drop_down_set_model(profile_dropdown_, G_LIST_MODEL(names));
        g_object_unref(names);
    }
    if (gtk_drop_down_get_selected(profile_dropdown_) != selected)
        gtk_drop_down_set_selected(profile_dropdown_, selected);
    syncing_ = false;
    adw_window_title_set_title(window_title_, ("GTKdrop — " + manager_.active().name).c_str());
}

void MainWindow::update_save_button() {
    gtk_widget_set_visible(save_profile_button_, manager_.dirty());
}

void MainWindow::on_profile_selected() {
    if (syncing_)
        return;
    auto *item = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(profile_dropdown_));
    if (!item)
        return;
    const std::string name = gtk_string_object_get_string(item);
    if (name == manager_.active().name)
        return;
    if (!manager_.dirty()) {
        switch_to(name);
        return;
    }

    AdwDialog *dialog = adw_alert_dialog_new("Save Profile Changes?", nullptr);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), "“%s” has unsaved changes.",
                                 manager_.active().name.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "discard", "_Discard",
                                   "save", "_Save", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    auto *req = new std::pair<MainWindow *, std::string>(this, name);
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window_), nullptr,
                            [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *req = static_cast<std::pair<MainWindow *, std::string> *>(data);
        MainWindow *self = req->first;
        const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(src), res);
        if (g_str_equal(response, "save"))
            self->manager_.save_active();
        if (g_str_equal(response, "cancel"))
            self->populate_profiles(); // put the selector back on the active profile
        else
            self->switch_to(req->second);
        delete req;
    }, req);
}

void MainWindow::switch_to(const std::string &name) {
    manager_.switch_profile(name);
    populate_profiles();
    update_source_visibility(); // before sync_items, which refilters
    sync_items();
}

void MainWindow::on_refresh_mods() {
    manager_.flush();
    manager_.rescan();
    populate_mods();
}

void MainWindow::on_settings() {
    gtkdrop::ui::show_settings_dialog(GTK_WIDGET(window_), manager_, nexus_, [this] {
        populate_mods(); // folders may have changed (the manager rescans when they do)
    });
}

void MainWindow::on_edit_profiles() {
    gtkdrop::ui::show_profiles_dialog(GTK_WIDGET(window_), manager_, [this] {
        populate_profiles();
        update_source_visibility();
        sync_items(); // deleting the active profile switches back to Default
    });
}

void MainWindow::on_save_profile() {
    manager_.save_active();
    update_save_button();
    toast("Saved “" + manager_.active().name + "”");
}

void MainWindow::on_save_configs() {
    if (!manager_.settings().profile_mod_configs) {
        toast("Per-profile mod configs are turned off in Settings");
        return;
    }
    toast(manager_.capture_configs() ? "Saved the mod configs to “" + manager_.active().name + "”"
                                     : "The mod configs of “" + manager_.active().name + "” are already up to date");
}

void MainWindow::on_start_smapi() {
    if (launcher_.busy())
        return;
    const auto &settings = manager_.settings();
    if (!settings.has_game_dir()) {
        AdwToast *t = adw_toast_new("SMAPI wasn't found. Set the Stardew Valley folder first.");
        adw_toast_set_button_label(t, "Settings");
        adw_toast_set_action_name(t, "win.settings");
        gtkdrop::ui::show_toast(toast_overlay_, t);
        return;
    }

    auto method = gtkdrop::launch_method_from_string(settings.launch_method);
    if (method == gtkdrop::LaunchMethod::Ask) {
        if (gtkdrop::steam::is_steam_install(settings.game_dir)) {
            ask_launch_method(); // continues in launch_with()
            return;
        }
        method = gtkdrop::LaunchMethod::Gog; // not Steam's copy: start it directly
        auto updated = settings;
        updated.launch_method = gtkdrop::to_string(method);
        manager_.update_settings(updated);
    }
    launch_with(method);
}

void MainWindow::ask_launch_method() {
    AdwDialog *dialog = adw_alert_dialog_new("How Should the Game Start?", nullptr);
    // Wide layout: three responses side by side instead of a tall, narrow stack.
    adw_alert_dialog_set_prefer_wide_layout(ADW_ALERT_DIALOG(dialog), TRUE);
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog),
                              "Through Steam keeps the Steam overlay, playtime and Cloud saves working. Steam "
                              "may ask once to allow starting the game with GTKdrop's mods folder.\n\n"
                              "Directly starts SMAPI without Steam, as Stardrop does.\n\n"
                              "You can change this later in Settings.");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "gog", "_Directly",
                                   "steam", "Through _Steam", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "steam", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "steam");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window_), nullptr,
                            [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *self = static_cast<MainWindow *>(data);
        const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(src), res);
        if (g_str_equal(response, "cancel"))
            return;
        auto updated = self->manager_.settings();
        updated.launch_method = response;
        self->manager_.update_settings(updated);
        self->launch_with(gtkdrop::launch_method_from_string(response));
    }, this);
}

// Steam drops the arguments GTKdrop passes, so the mods folder goes through the game's Steam launch options.
void MainWindow::show_steam_setup(bool then_launch) {
    const std::string options = gtkdrop::steam_wrapper::launch_options();
    AdwDialog *dialog = adw_alert_dialog_new(
            "Set Up Steam Launch Options",
            "Steam doesn’t pass GTKdrop’s mods folder on to the game, so it would start with the default Mods folder "
            "instead of this profile.\n\nIn Steam, right-click Stardew Valley → Properties → General, and paste this "
            "into Launch Options. It's needed once. Only launches from GTKdrop use a profile; starting the game from "
            "Steam itself still uses the default Mods folder.");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), options.c_str());
    gtk_editable_set_editable(GTK_EDITABLE(entry), FALSE);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_widget_add_css_class(entry, "monospace");
    GtkWidget *copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
    gtk_widget_set_tooltip_text(copy, "Copy");
    g_object_set_data_full(G_OBJECT(copy), "text", g_strdup(options.c_str()), g_free);
    g_signal_connect(copy, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer) {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(b)),
                               static_cast<const char *>(g_object_get_data(G_OBJECT(b), "text")));
        gtk_button_set_icon_name(b, "object-select-symbolic");
    }), nullptr);
    gtk_box_append(GTK_BOX(row), entry);
    gtk_box_append(GTK_BOX(row), copy);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), row);
    if (then_launch) {
        adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "launch", "I’ve Set It, _Launch",
                                       nullptr);
        adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "launch", ADW_RESPONSE_SUGGESTED);
    } else {
        adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Close", nullptr);
    }
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    g_signal_connect(dialog, "response", G_CALLBACK(+[](AdwAlertDialog *, const char *response, gpointer self) {
        if (std::string_view(response) == "launch")
            static_cast<MainWindow *>(self)->launch_with(gtkdrop::LaunchMethod::Steam, true);
    }), this);
    adw_dialog_present(dialog, GTK_WIDGET(window_));
}

void MainWindow::launch_with(gtkdrop::LaunchMethod method, bool steam_setup_shown) {
    const auto &settings = manager_.settings();
    if (method == gtkdrop::LaunchMethod::Steam && !gtkdrop::steam::is_steam_install(settings.game_dir)) {
        AdwToast *t = adw_toast_new("This Stardew Valley folder isn't the one Steam installed. Start it directly "
                                    "instead (Settings → Launch).");
        adw_toast_set_button_label(t, "Settings");
        adw_toast_set_action_name(t, "win.settings");
        gtkdrop::ui::show_toast(toast_overlay_, t);
        return;
    }
    // Already running (started outside GTKdrop): don't touch its mods folder, just watch it.
    if (launcher_.attach_to_running(game_callbacks())) {
        toast("Stardew Valley is already running");
        return;
    }

    if (method == gtkdrop::LaunchMethod::Steam && !steam_setup_shown && !gtkdrop::steam_wrapper::configured()) {
        show_steam_setup(true); // continues here once set up
        return;
    }

    const auto result = manager_.prepare_launch();
    update_save_button();
    for (const auto &e: result.errors)
        g_warning("Selected mods: %s", e.c_str());

    std::string error;
    if (!launcher_.launch(method, settings.game_dir, gtkdrop::paths::selected_mods_dir(), game_callbacks(), &error)) {
        toast("Could not start the game: " + error);
        return;
    }
    set_game_state(GameState::Starting);
    launched_with_ = method;
    toast(std::string("Starting Stardew Valley") + (method == gtkdrop::LaunchMethod::Steam ? " through Steam" : "") +
          " with " + std::to_string(result.linked) + " mods from “" + manager_.active().name + "”" +
          (result.errors.empty() ? "" : " (" + std::to_string(result.errors.size()) + " problems, see the log)"));
}

gtkdrop::GameLauncher::Callbacks MainWindow::game_callbacks() {
    return {
        .started = [this] {
            set_game_state(GameState::Running);
            if (launched_with_ != gtkdrop::LaunchMethod::Steam)
                return;
            launched_with_ = gtkdrop::LaunchMethod::Ask;
            // Did Steam's launch options pass the profile's folder on? Without it SMAPI uses the default Mods folder.
            const auto arg = launcher_.mods_path_arg();
            std::error_code ec;
            const bool ok = arg && fs::equivalent(*arg, gtkdrop::paths::selected_mods_dir(), ec);
            gtkdrop::steam_wrapper::remember_verified(ok);
            if (ok)
                return;
            g_warning("Steam started SMAPI with %s, not GTKdrop's mods folder",
                      arg ? arg->c_str() : "no mods folder (neither --mods-path nor SMAPI_MODS_PATH)");
            AdwToast *t = adw_toast_new(("Steam started the game with the default Mods folder, not “" +
                                         manager_.active().name + "”. Set the game's Steam launch options.").c_str());
            adw_toast_set_use_markup(t, FALSE);
            adw_toast_set_button_label(t, "Show How");
            adw_toast_set_action_name(t, "win.steam-setup");
            adw_toast_set_timeout(t, 0);
            gtkdrop::ui::show_toast(toast_overlay_, t);
        },
        .exited = [this] {
            set_game_state(GameState::Idle);
            const bool saved = manager_.settings().profile_mod_configs && manager_.capture_configs();
            toast(saved ? "Stardew Valley closed. Mod configs saved to “" + manager_.active().name + "”"
                        : std::string("Stardew Valley closed"));
        },
        .failed = [this](const std::string &message) {
            set_game_state(GameState::Idle);
            gtkdrop::steam_wrapper::cancel_request(); // a later start from Steam mustn't pick it up
            AdwToast *t = adw_toast_new(message.c_str());
            adw_toast_set_use_markup(t, FALSE);
            adw_toast_set_button_label(t, "SMAPI Log");
            adw_toast_set_action_name(t, "win.open-smapi-log");
            adw_toast_set_timeout(t, 0);
            gtkdrop::ui::show_toast(toast_overlay_, t);
        },
    };
}

// While the game runs the window is locked, as in Stardrop: switching profiles would rewrite the config files
// of mods the game has loaded, and mod changes wouldn't take effect until the next launch anyway.
void MainWindow::set_game_state(GameState state) {
    const bool idle = state == GameState::Idle;
    if (!idle)
        gtk_window_set_focus(window_, nullptr); // a focused search/notes entry would warn when disabled
    gtk_widget_set_sensitive(content_box_, idle);
    gtk_widget_set_sensitive(smapi_button_, idle);
    gtk_label_set_label(smapi_label_, state == GameState::Starting ? "Starting…"
                                      : state == GameState::Running ? "Running"
                                                                    : "Start SMAPI");
    adw_window_title_set_subtitle(window_title_, state == GameState::Running ? "Stardew Valley is running"
                                                 : state == GameState::Starting ? "Starting Stardew Valley…"
                                                                                : "Unknown SMAPI version");
    for (const char *name: {"start-smapi", "add-mods", "add-mod-folders", "refresh-mods", "enable-all", "disable-all", "edit-profiles",
                            "save-configs", "save-profile", "settings", "mod-delete", "mod-toggle-state"})
        if (GAction *action = g_action_map_lookup_action(G_ACTION_MAP(window_), name))
            g_simple_action_set_enabled(G_SIMPLE_ACTION(action), idle);
}

// ---------------------------------------------------------------------------
// Nexus Mods: the API key is only used for update checks and collection file lists. Downloads happen in the
// browser from the generated payload: {"mods": [{"<mod name>": "<url>"}, …]}.
// ---------------------------------------------------------------------------
bool MainWindow::require_nexus() {
    if (nexus_.status() == gtkdrop::nexus::Account::Status::Connected)
        return true;
    AdwToast *t = adw_toast_new("Connect your Nexus Mods account first (Settings → Nexus Mods)");
    adw_toast_set_button_label(t, "Settings");
    adw_toast_set_action_name(t, "win.settings");
    gtkdrop::ui::show_toast(toast_overlay_, t);
    return false;
}

void MainWindow::open_path(const fs::path &path) {
    GFile *file = g_file_new_for_path(path.c_str());
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, window_, nullptr, nullptr, nullptr);
    g_object_unref(launcher);
    g_object_unref(file);
}

void MainWindow::toast_with_file(const std::string &text, const fs::path &path) {
    AdwToast *t = adw_toast_new(text.c_str());
    adw_toast_set_use_markup(t, FALSE);
    adw_toast_set_button_label(t, "Open");
    adw_toast_set_action_name(t, "win.open-file");
    adw_toast_set_action_target(t, "s", path.c_str());
    adw_toast_set_timeout(t, 10);
    gtkdrop::ui::show_toast(toast_overlay_, t);
}

void MainWindow::check_updates(std::function<void()> then) {
    if (checking_updates_ || !require_nexus())
        return;
    std::vector<int> ids;
    for (const auto &m: manager_.mods())
        if (const int id = m.nexus_id(); id > 0 && !m.is_from_collection() &&
                                         std::ranges::find(ids, id) == ids.end())
            ids.push_back(id);

    checking_updates_ = true;
    gtk_button_set_label(update_status_button_, "Checking for Updates…");
    gtk_widget_set_sensitive(GTK_WIDGET(update_status_button_), FALSE);
    gtkdrop::nexus::fetch_latest_versions(nexus_, ids, [this, then = std::move(then)](
            std::optional<std::vector<gtkdrop::nexus::LatestVersion>> versions, const std::string &error) {
        checking_updates_ = false;
        gtk_widget_set_sensitive(GTK_WIDGET(update_status_button_), TRUE);
        if (!versions) {
            gtk_button_set_label(update_status_button_, "Click To Check For Mod Updates");
            toast("Couldn't check for updates: " + error);
            return;
        }
        std::map<int, std::string> latest;
        for (const auto &v: *versions)
            latest[v.mod_id] = v.version;
        manager_.set_latest_versions(std::move(latest));
        populate_mods();

        int updates = 0;
        for (const auto &m: manager_.mods())
            updates += m.has_update();
        GDateTime *now = g_date_time_new_now_local();
        char *time = g_date_time_format(now, "%H:%M");
        const std::string label = (updates ? std::to_string(updates) + (updates == 1 ? " Update" : " Updates") +
                                                     " Available"
                                           : std::string("All Mods Up to Date")) +
                                  "  ·  Checked " + time;
        g_free(time);
        g_date_time_unref(now);
        gtk_button_set_label(update_status_button_, label.c_str());
        g_message("Update check: %zu Nexus mods looked up, %d updates", versions->size(), updates);
        if (then)
            then();
    });
}

void MainWindow::get_updates(bool enabled_only) {
    // "Get" always works from fresh versions: a check first if none ran this session.
    if (!manager_.update_check_done())
        check_updates([this, enabled_only] { write_updates_payload(enabled_only); });
    else
        write_updates_payload(enabled_only);
}

void MainWindow::write_updates_payload(bool enabled_only) {
    std::vector<std::pair<std::string, std::string>> list;
    std::vector<int> seen;
    for (const auto &m: manager_.mods()) {
        if (!m.has_update() || (enabled_only && !m.enabled) || std::ranges::find(seen, m.nexus_id()) != seen.end())
            continue;
        seen.push_back(m.nexus_id());
        list.emplace_back(m.name(), gtkdrop::nexus::mod_page_url(m.nexus_id()));
    }
    if (list.empty()) {
        toast(enabled_only ? "All enabled mods are up to date" : "All mods are up to date");
        return;
    }
    const auto path = gtkdrop::write_payload("updates.json", list);
    if (path.empty()) {
        toast("Couldn't write the download list");
        return;
    }
    toast_with_file("Listed " + std::to_string(list.size()) + (list.size() == 1 ? " update" : " updates") +
                    " in updates.json", path);
}

// After nexus-cli finishes: unpack the archives with the system's tools into the collection's folder (collection
// lists) or over the installed copies (update lists), rescan, then ask Nexus again what the collection still needs.
void MainWindow::install_downloads(const fs::path &payload, const fs::path &download_dir,
                                   const std::string &download_status) {
    if (launcher_.busy() || launcher_.running()) {
        toast_with_file(download_status + ". Close the game, then install them from " + download_dir.string(),
                        download_dir);
        return;
    }
    if (installing_.contains(download_dir)) {
        install_again_.insert(download_dir); // picks up the new archives once the current install ends
        return;
    }
    installing_.insert(download_dir);
    std::string nxm;
    if (auto doc = gtkdrop::read_json_file(payload); doc && doc->is_object())
        nxm = gtkdrop::get_string_ci(*doc, "nxm");
    const auto link = gtkdrop::nexus::parse_nxm(nxm);

    gtkdrop::InstallTarget target{gtkdrop::InstallTarget::Kind::Update, manager_.settings().effective_mods_dir()};
    if (link && link->kind == gtkdrop::nexus::NxmLink::Kind::Collection) {
        target.kind = gtkdrop::InstallTarget::Kind::Collection;
        target.dest = manager_.settings().effective_collections_dir() /
                      (std::string(gtkdrop::nexus::kGameDomain) + "-" + link->slug);
    } else {
        target = loose_mods_target();
    }

    toast(download_status + ". Installing…");
    // Into the download's log, so the Download window shows the unpacking too.
    std::weak_ptr<gtkdrop::downloads::Job> job = gtkdrop::downloads::Job::current();
    auto log = [job](const std::string &line) {
        if (auto j = job.lock())
            j->append_log(line);
    };
    log("— Installing into " + target.dest.string());
    gtkdrop::install_archives(download_dir, std::move(target),
                              [this, payload, nxm, download_dir, job, is_collection = link.has_value()](
                                      gtkdrop::InstallResult r) {
        installing_.erase(download_dir);
        if (install_again_.erase(download_dir)) {
            install_downloads(payload, download_dir, "More downloads arrived");
            return; // that run reports and re-checks
        }
        manager_.flush();
        manager_.rescan(); // also enables newly installed mods a collection profile was waiting for
        populate_mods();
        for (const auto &e: r.errors)
            g_warning("Install: %s", e.c_str());
        std::string text = "Installed " + std::to_string(r.mods) + (r.mods == 1 ? " mod" : " mods") + " from " +
                           std::to_string(r.archives) + (r.archives == 1 ? " archive" : " archives");
        if (!r.errors.empty())
            text += " · " + std::to_string(r.errors.size()) + " problem(s), e.g. " + r.errors.front();
        toast_with_file(text, download_dir);
        if (auto j = job.lock())
            j->append_log("— " + text + (is_collection ? ". Checking the collection for missing mods…" : ""));
        if (is_collection)
            open_uri(nxm); // what's still missing (failed downloads, other files of a mod…)
    }, log);
}

void MainWindow::open_uri(const std::string &uri) {
    // A link that started GTKdrop arrives while the saved key is still being checked.
    if (nexus_settling()) {
        pending_uris_.push_back(uri);
        return;
    }
    if (g_ascii_strncasecmp(uri.c_str(), "nxm:", 4) != 0) {
        g_message("Ignoring %s: GTKdrop only opens nxm:// links", uri.c_str()); // e.g. "./GTKdrop ."
        return;
    }
    const auto link = gtkdrop::nexus::parse_nxm(uri);
    if (!link) {
        g_warning("Ignoring a link GTKdrop doesn't understand: %s", uri.c_str());
        toast("GTKdrop can't open this link");
        return;
    }
    if (link->domain != gtkdrop::nexus::kGameDomain) {
        toast("That link is for another game (" + link->domain + ")");
        return;
    }
    if (link->kind == gtkdrop::nexus::NxmLink::Kind::ModFile) {
        handle_file_link(*link);
        return;
    }
    handle_collection_link(*link, uri);
}

// "Mod Manager Download" in the browser for a file one of the download lists has (e.g. after "Download in
// Browser"): fetch it into that list's folder and install it like the list's own downloads.
void MainWindow::handle_file_link(const gtkdrop::nexus::NxmLink &link) {
    if (!require_nexus())
        return;
    int list_file = 0;
    const fs::path payload = gtkdrop::downloads::payload_with_file(link.mod_id, link.file_id, &list_file);
    if (payload.empty()) {
        toast("That file isn't in any of GTKdrop's download lists; installing single mods isn't supported yet");
        return;
    }
    const fs::path dir = gtkdrop::downloads::downloads_dir_for(payload);
    std::error_code ec;
    fs::create_directories(dir, ec);
    toast("Downloading a file for " + payload.filename().string() + "…");
    g_message("Browser download: mod %d file %d for %s", link.mod_id, link.file_id, payload.filename().c_str());
    gtkdrop::nexus::fetch_download_links(nexus_, link.domain, link.mod_id, link.file_id, link.key, link.expires,
                                         [this, payload, dir, link, list_file](std::vector<std::string> urls,
                                                                    const std::string &error) {
        if (urls.empty()) {
            toast("Couldn't get the file from Nexus Mods: " + error);
            return;
        }
        gtkdrop::nexus::fetch_file_name(nexus_, link.domain, link.mod_id, link.file_id,
                                        [this, payload, dir, link, list_file, url = urls.front()](
                                                std::optional<std::string> name, const std::string &) {
            gtkdrop::download_file(url, dir, name.value_or(""), nullptr, {},
                                   [this, payload, dir, link, list_file](fs::path path, const std::string &error) {
                if (!error.empty()) {
                    toast("Download failed: " + error);
                    return;
                }
                gtkdrop::downloads::record_file(dir, link.file_id, path.filename().string());
                // Another file of the mod than the list named (e.g. a newer one): it stands in for the listed one.
                if (list_file > 0 && list_file != link.file_id)
                    gtkdrop::downloads::record_file(dir, list_file, path.filename().string());
                g_message("Browser download: saved %s", path.filename().c_str());
                // A running download of the same list installs it with the rest when it ends.
                if (auto job = gtkdrop::downloads::Job::current(); job && job->active() && job->out_dir() == dir) {
                    job->append_log("— Got " + path.filename().string() + " from the browser");
                    return;
                }
                install_downloads(payload, dir, "Downloaded " + path.filename().string());
            });
        });
    });
}

void MainWindow::handle_collection_link(const gtkdrop::nexus::NxmLink &link, const std::string &uri) {
    if (!require_nexus())
        return;
    toast("Getting the collection’s mod list from Nexus Mods…");
    gtkdrop::nexus::fetch_collection(nexus_, link.slug, link.revision, [this, uri](
            std::optional<gtkdrop::nexus::Collection> collection, const std::string &error) {
        if (!collection) {
            toast("Couldn't get the collection: " + error);
            return;
        }
        // Needed = required files that aren't installed in at least the collection's version. SMAPI is listed by
        // collections but belongs in the game folder, not among the mods. Many mods' manifests carry a wrong Nexus
        // ID (or none) and settings packs have no manifest at all, so a file also counts once the archive it was
        // downloaded as got installed from this list's download folder.
        const std::string file = "collection-" + collection->slug + "-r" + std::to_string(collection->revision) + ".json";
        const auto dl_dir = gtkdrop::downloads::downloads_dir_for(gtkdrop::payloads_dir() / file);
        auto check = std::make_shared<CollectionCheck>();
        check->installed = gtkdrop::downloads::installed_archives(dl_dir);
        auto recorded = gtkdrop::downloads::recorded_files(dl_dir);
        for (const auto &f: collection->files) {
            if (f.optional || f.mod_id == gtkdrop::nexus::kSmapiModId)
                continue;
            ++check->required;
            const bool installed = std::ranges::any_of(manager_.mods(), [&](const gtkdrop::Mod &m) {
                return m.nexus_id() == f.mod_id &&
                       (f.version.empty() || gtkdrop::compare_versions(m.manifest.version, f.version) >= 0);
            });
            if (installed)
                continue;
            if (auto it = recorded.find(f.file_id); it != recorded.end() && check->has(it->second))
                continue;
            check->missing.push_back(f);
        }
        // Files never recorded (downloaded before GTKdrop kept track): ask Nexus for their archive names.
        std::vector<gtkdrop::nexus::CollectionFile> unknown;
        if (!check->installed.empty())
            for (const auto &f: check->missing)
                if (!recorded.contains(f.file_id))
                    unknown.push_back(f);
        auto finish = [this, collection = *collection, uri, file, check] {
            std::vector<std::pair<std::string, std::string>> list;
            for (const auto &f: check->missing)
                if (!check->found.contains(f.file_id))
                    list.emplace_back(f.mod_name, gtkdrop::nexus::mod_page_url(f.mod_id, f.file_id));
            collection_checked(collection, uri, file, check->required, std::move(list));
        };
        if (unknown.empty()) {
            finish();
            return;
        }
        check->pending = int(unknown.size());
        for (const auto &f: unknown)
            gtkdrop::nexus::fetch_file_name(nexus_, gtkdrop::nexus::kGameDomain, f.mod_id, f.file_id,
                                            [check, dl_dir, id = f.file_id, finish](std::optional<std::string> name,
                                                                                    const std::string &) {
                if (name) {
                    gtkdrop::downloads::record_file(dl_dir, id, *name);
                    if (check->has(*name))
                        check->found.insert(id);
                }
                if (--check->pending == 0)
                    finish();
            });
    });
}

void MainWindow::collection_checked(const gtkdrop::nexus::Collection &c, const std::string &uri,
                                    const std::string &file, int required,
                                    std::vector<std::pair<std::string, std::string>> list) {
    const gtkdrop::nexus::Collection *collection = &c;
    {
        // The collection gets its own protected profile (re-opening a link updates it). Mods it needs that aren't
        // installed yet get enabled in it once they show up — ideally in the collection's own folder, so they stay
        // out of other profiles.
        bool created = false;
        const std::string profile = manager_.import_collection(*collection, &created);
        std::error_code ec;
        const auto folder = manager_.settings().effective_collections_dir() /
                            (std::string(gtkdrop::nexus::kGameDomain) + "-" + collection->slug);
        fs::create_directories(folder, ec);
        populate_profiles();
        sync_items();
        if (profile.empty()) {
            toast("Couldn't create a profile for “" + collection->name + "”");
        } else if (created) {
            AdwToast *t = adw_toast_new(("Added the profile “" + profile + "”").c_str());
            adw_toast_set_use_markup(t, FALSE);
            adw_toast_set_button_label(t, "Switch");
            adw_toast_set_action_name(t, "win.switch-profile");
            adw_toast_set_action_target(t, "s", profile.c_str());
            gtkdrop::ui::show_toast(toast_overlay_, t);
        }
        if (list.empty()) {
            gtkdrop::write_payload(file, {}, uri); // an old list of this revision is done
            toast("You have every mod “" + collection->name + "” needs");
            return;
        }
        // A download of this list re-checking it after install: say so in its log.
        if (auto job = gtkdrop::downloads::Job::current(); job && job->payload().filename() == file)
            job->append_log("— Nexus Mods: " + std::to_string(list.size()) + " of " + std::to_string(required) +
                            " required mods still missing" + (list.empty() ? "" : "; " + file + " now lists them"));
        const auto path = gtkdrop::write_payload(file, list, uri);
        if (path.empty()) {
            toast("Couldn't write the download list");
            return;
        }
        g_message("Collection %s r%d: %d required files, %zu needed", collection->slug.c_str(), collection->revision,
                  required, list.size());
        toast_with_file("“" + collection->name + "”: " + std::to_string(list.size()) + " of " +
                        std::to_string(required) + " mods needed. Install them into " + folder.string(), path);
    }
}

void MainWindow::on_open_log() {
    GFile *file = g_file_new_for_path(gtkdrop::log::file_path().c_str());
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, window_, nullptr, nullptr, nullptr);
    g_object_unref(launcher);
    g_object_unref(file);
}

void MainWindow::on_open_smapi_log() {
    // SMAPI writes its log under the game's own data folder.
    const fs::path log = fs::path(g_get_user_config_dir()) / "StardewValley" / "ErrorLogs" / "SMAPI-latest.txt";
    std::error_code ec;
    if (!fs::exists(log, ec)) {
        toast("There's no SMAPI log yet. It's written the first time SMAPI runs.");
        return;
    }
    GFile *file = g_file_new_for_path(log.c_str());
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, window_, nullptr, nullptr, nullptr);
    g_object_unref(launcher);
    g_object_unref(file);
}

void MainWindow::open_mod_path(GtkdropModItem *item, int which) {
    const gtkdrop::Mod *m = mod_for(item);
    if (!m)
        return;
    const fs::path path = which == 0 ? m->folder : which == 1 ? m->folder / "manifest.json" : m->config_file;
    if (path.empty()) {
        toast("“" + m->name() + "” has no config.json yet. Run the game once to create it.");
        return;
    }
    GFile *file = g_file_new_for_path(path.c_str());
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, window_, nullptr, nullptr, nullptr);
    g_object_unref(launcher);
    g_object_unref(file);
}

void MainWindow::report_load(const gtkdrop::ModManager::LoadReport &report) {
    if (report.import && report.import->found) {
        const auto &r = *report.import;
        toast("Imported " + std::to_string(r.profiles) + " profiles and " + std::to_string(r.collections) +
              " collections from Stardrop");
        for (const auto &m: r.messages)
            g_message("Stardrop import: %s", m.c_str());
    }
    if (!report.game_detected.empty())
        toast("Found the " + report.game_detected + " copy of Stardew Valley at " + manager_.settings().game_dir.string());
    if (!manager_.settings().has_game_dir()) {
        AdwToast *t = adw_toast_new("SMAPI wasn't found. Set the Stardew Valley folder in Settings.");
        adw_toast_set_button_label(t, "Settings");
        adw_toast_set_action_name(t, "win.settings");
        adw_toast_set_timeout(t, 0);
        gtkdrop::ui::show_toast(toast_overlay_, t);
    }
}

gboolean MainWindow::on_close_request() {
    if (closing_ || !manager_.dirty()) {
        g_clear_handle_id(&note_save_id_, g_source_remove);
        manager_.shutdown();
        return FALSE;
    }

    AdwDialog *dialog = adw_alert_dialog_new("Save Profile Changes?", nullptr);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), "“%s” has unsaved changes.",
                                 manager_.active().name.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "discard", "_Discard",
                                   "save", "_Save", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window_), nullptr,
                            [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *self = static_cast<MainWindow *>(data);
        const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(src), res);
        if (g_str_equal(response, "cancel"))
            return;
        if (g_str_equal(response, "save"))
            self->manager_.save_active();
        else
            self->manager_.revert_active();
        self->closing_ = true;
        gtk_window_close(self->window_);
    }, this);
    return TRUE;
}

} // namespace

GtkWindow *gtkdrop_main_window_new(GtkApplication *app) {
    return (new MainWindow(app))->window();
}

void gtkdrop_main_window_open_uri(GtkWindow *window, const char *uri) {
    if (auto *self = static_cast<MainWindow *>(g_object_get_data(G_OBJECT(window), "gtkdrop-main-window")))
        self->open_uri(uri);
}
