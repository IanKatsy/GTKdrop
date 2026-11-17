#include "dialogs.h"

#include "core/nxm_handler.h"
#include "downloads.h"
#include "core/payload.h"
#include "toast.h"

#include <adwaita.h>

#include <algorithm>
#include <string>
#include <vector>

namespace gtkdrop::ui {

namespace fs = std::filesystem;

void choose_folder(GtkWindow *parent, const char *title, const fs::path &initial,
                   std::function<void(fs::path)> on_chosen) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title);
    std::error_code ec;
    if (!initial.empty() && fs::is_directory(initial, ec)) {
        GFile *folder = g_file_new_for_path(initial.c_str());
        gtk_file_dialog_set_initial_folder(dialog, folder);
        g_object_unref(folder);
    }
    auto *cb = new std::function<void(fs::path)>(std::move(on_chosen));
    gtk_file_dialog_select_folder(dialog, parent, nullptr, [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *cb = static_cast<std::function<void(fs::path)> *>(data);
        if (GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr)) {
            if (char *path = g_file_get_path(file)) {
                (*cb)(path);
                g_free(path);
            }
            g_object_unref(file);
        }
        delete cb;
    }, cb);
    g_object_unref(dialog);
}

namespace {

GtkWindow *window_of(GtkWidget *widget) {
    return GTK_WINDOW(gtk_widget_get_root(widget));
}

GtkWidget *icon_button(const char *icon, const char *tooltip) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(b, "flat");
    return b;
}

GtkWidget *plain_action_row(const std::string &title, const std::string &subtitle) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
    return row;
}

void dialog_toast(AdwPreferencesDialog *dialog, const std::string &text) {
    AdwToast *toast = adw_toast_new(text.c_str());
    adw_toast_set_use_markup(toast, FALSE);
    show_toast(dialog, toast);
}

// Asks for a name in an alert dialog; `on_name` gets the trimmed text when confirmed.
void ask_for_name(GtkWidget *parent, const char *heading, const char *confirm, const std::string &initial,
                  std::function<void(std::string)> on_name) {
    AdwDialog *dialog = adw_alert_dialog_new(heading, nullptr);
    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), initial.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "ok", confirm, nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "ok", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "ok");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct Req {
        GtkWidget *entry;
        std::function<void(std::string)> cb;
    };
    auto *req = new Req{entry, std::move(on_name)};
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), parent, nullptr, [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *req = static_cast<Req *>(data);
        if (g_str_equal(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(src), res), "ok")) {
            char *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(req->entry))));
            req->cb(text);
            g_free(text);
        }
        delete req;
    }, req);
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------
struct ProfilesDialog {
    AdwPreferencesDialog *dialog;
    AdwPreferencesGroup *list_group;
    GtkWidget *name_row;
    GtkWidget *copy_row;
    GtkWidget *collection_row; // copying a collection's profile: keep the copy tied to the collection
    std::vector<GtkWidget *> rows;
    ModManager &manager;
    std::function<void()> on_changed;

    void rebuild();
    void add();
    void rename(const std::string &name);
    void remove(const std::string &name);
    void changed() {
        rebuild();
        on_changed();
    }
};

using ProfileRef = std::pair<ProfilesDialog *, std::string>;

void ProfilesDialog::rebuild() {
    for (GtkWidget *row: rows)
        adw_preferences_group_remove(list_group, row);
    rows.clear();

    for (const Profile &p: manager.profiles()) {
        std::string subtitle = std::to_string(p.enabled.size()) + (p.enabled.size() == 1 ? " mod enabled" : " mods enabled");
        if (p.name == manager.active().name)
            subtitle += " · active";
        if (p.is_from_collection())
            subtitle += p.is_protected ? " · from a collection" : " · copy of a collection";
        GtkWidget *row = plain_action_row(p.name, subtitle);

        auto *data = new ProfileRef(this, p.name);
        g_object_set_data_full(G_OBJECT(row), "profile", data, [](gpointer d) {
            delete static_cast<ProfileRef *>(d);
        });

        GtkWidget *rename_btn = icon_button("document-edit-symbolic", "Rename");
        GtkWidget *delete_btn = icon_button("user-trash-symbolic", "Delete");
        gtk_widget_set_sensitive(rename_btn, !p.is_protected);
        gtk_widget_set_sensitive(delete_btn, !p.is_protected);
        if (p.is_protected) {
            gtk_widget_set_tooltip_text(rename_btn, "This profile is protected");
            gtk_widget_set_tooltip_text(delete_btn, "This profile is protected");
        }
        g_signal_connect(rename_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *pd = static_cast<ProfileRef *>(d);
            pd->first->rename(pd->second);
        }), data);
        g_signal_connect(delete_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *pd = static_cast<ProfileRef *>(d);
            pd->first->remove(pd->second);
        }), data);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), rename_btn);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), delete_btn);

        adw_preferences_group_add(list_group, row);
        rows.push_back(row);
    }
}

void ProfilesDialog::add() {
    char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(name_row))));
    std::string error;
    if (manager.add_profile(name, adw_switch_row_get_active(ADW_SWITCH_ROW(copy_row)), &error,
                            adw_switch_row_get_active(ADW_SWITCH_ROW(collection_row)))) {
        gtk_editable_set_text(GTK_EDITABLE(name_row), "");
        dialog_toast(dialog, std::string("Added “") + name + "”");
        changed();
    } else {
        dialog_toast(dialog, error);
    }
    g_free(name);
}

void ProfilesDialog::rename(const std::string &name) {
    ask_for_name(GTK_WIDGET(dialog), "Rename Profile", "_Rename", name, [this, name](std::string new_name) {
        if (new_name == name)
            return;
        std::string error;
        if (manager.rename_profile(name, new_name, &error))
            changed();
        else
            dialog_toast(dialog, error);
    });
}

void ProfilesDialog::remove(const std::string &name) {
    AdwDialog *alert = adw_alert_dialog_new(nullptr, "Its enabled mods, notes and saved mod configs are deleted "
                                                     "with it. The mods themselves stay installed.");
    adw_alert_dialog_format_heading(ADW_ALERT_DIALOG(alert), "Delete “%s”?", name.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", "_Cancel", "delete", "_Delete", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");

    auto *req = new ProfileRef(this, name);
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(alert), GTK_WIDGET(dialog), nullptr,
                            [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *req = static_cast<ProfileRef *>(data);
        if (g_str_equal(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(src), res), "delete")) {
            std::string error;
            if (req->first->manager.delete_profile(req->second, &error))
                req->first->changed();
            else
                dialog_toast(req->first->dialog, error);
        }
        delete req;
    }, req);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
struct SettingsDialog {
    AdwPreferencesDialog *dialog;
    ModManager &manager;
    Settings settings;
    std::function<void()> on_changed;
    GtkWidget *game_row, *mods_row, *collections_row;

    void save() {
        manager.update_settings(settings);
        refresh_rows();
        on_changed();
    }

    void refresh_rows() {
        const auto game = settings.game_dir.empty() ? std::string("Not set") : settings.game_dir.string();
        adw_action_row_set_subtitle(ADW_ACTION_ROW(game_row),
                                    (game + (settings.has_game_dir() || settings.game_dir.empty()
                                                     ? "" : "  (SMAPI isn't installed here)")).c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(mods_row),
                                    (settings.effective_mods_dir().string() +
                                     (settings.mods_dir.empty() ? "  (default)" : "")).c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(collections_row),
                                    (settings.effective_collections_dir().string() +
                                     (settings.collections_dir.empty() ? "  (default)" : "")).c_str());
    }
};

using PathMember = fs::path Settings::*;

GtkWidget *folder_row(SettingsDialog *sd, const char *title, PathMember member, bool resettable) {
    GtkWidget *row = plain_action_row(title, "");
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);

    struct Ctx {
        SettingsDialog *sd;
        PathMember member;
        std::string title;
    };
    auto *ctx = new Ctx{sd, member, title};
    g_object_set_data_full(G_OBJECT(row), "ctx", ctx, [](gpointer p) { delete static_cast<Ctx *>(p); });

    GtkWidget *choose = icon_button("folder-open-symbolic", "Choose Folder…");
    g_signal_connect(choose, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer p) {
        auto *ctx = static_cast<Ctx *>(p);
        choose_folder(window_of(GTK_WIDGET(b)), ctx->title.c_str(), ctx->sd->settings.*(ctx->member),
                      [sd = ctx->sd, member = ctx->member](fs::path chosen) {
            sd->settings.*member = chosen;
            sd->save();
            if (member == &Settings::game_dir && !sd->settings.has_game_dir())
                dialog_toast(sd->dialog, "SMAPI isn't installed in that folder (no StardewModdingAPI.dll)");
        });
    }), ctx);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), choose);

    if (resettable) {
        GtkWidget *reset = icon_button("edit-undo-symbolic", "Use Default");
        g_signal_connect(reset, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
            auto *ctx = static_cast<Ctx *>(p);
            ctx->sd->settings.*(ctx->member) = fs::path();
            ctx->sd->save();
        }), ctx);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), reset);
    }
    return row;
}

using SwitchCtx = std::pair<SettingsDialog *, bool Settings::*>;

GtkWidget *switch_row(SettingsDialog *sd, const char *title, const char *subtitle, bool Settings::*member) {
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), sd->settings.*member);

    auto *ctx = new SwitchCtx(sd, member);
    g_object_set_data_full(G_OBJECT(row), "ctx", ctx, [](gpointer p) {
        delete static_cast<SwitchCtx *>(p);
    });
    g_signal_connect(row, "notify::active", G_CALLBACK(+[](AdwSwitchRow *r, GParamSpec *, gpointer p) {
        auto *ctx = static_cast<SwitchCtx *>(p);
        ctx->first->settings.*(ctx->second) = adw_switch_row_get_active(r);
        ctx->first->save();
    }), ctx);
    return row;
}

} // namespace

void show_profiles_dialog(GtkWidget *parent, ModManager &manager, std::function<void()> on_changed) {
    AdwDialog *dialog = adw_preferences_dialog_new();
    adw_dialog_set_title(dialog, "Profiles");
    adw_dialog_set_content_width(dialog, 480);
    adw_preferences_dialog_set_search_enabled(ADW_PREFERENCES_DIALOG(dialog), FALSE);

    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *add_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(add_group), "New Profile");
    GtkWidget *name_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(name_row), "Name");
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(name_row), TRUE);
    GtkWidget *copy_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(copy_row), "Copy the Current Profile");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(copy_row), "Start with its enabled mods, notes and mod configs");
    // Only for a collection's profile (or a copy of one), and only when copying it.
    GtkWidget *collection_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(collection_row), "Keep It a Collection Profile");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(collection_row),
                                "On: shows the collection’s mods and Other Available Mods. Off: a normal profile "
                                "that enables the same mods");
    adw_switch_row_set_active(ADW_SWITCH_ROW(collection_row), TRUE);
    const bool from_collection = manager.active().is_from_collection();
    gtk_widget_set_visible(collection_row, FALSE);
    if (from_collection) {
        g_object_bind_property(copy_row, "active", collection_row, "visible", G_BINDING_SYNC_CREATE);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(copy_row), ("Start with “" + manager.active().name +
                                                                "”’s enabled mods, notes and mod configs").c_str());
    }
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group), name_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group), copy_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group), collection_row);

    GtkWidget *list_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(list_group), "Profiles");

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(add_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(list_group));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));

    auto *pd = new ProfilesDialog{ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_GROUP(list_group), name_row,
                                  copy_row, collection_row, {}, manager, std::move(on_changed)};
    g_object_set_data_full(G_OBJECT(dialog), "state", pd, [](gpointer p) { delete static_cast<ProfilesDialog *>(p); });
    g_signal_connect_swapped(name_row, "apply", G_CALLBACK(+[](ProfilesDialog *pd) { pd->add(); }), pd);
    g_signal_connect_swapped(name_row, "entry-activated", G_CALLBACK(+[](ProfilesDialog *pd) { pd->add(); }), pd);

    pd->rebuild();
    adw_dialog_present(dialog, parent);
}

namespace {

// Nexus Mods login: the personal API key, checked with Nexus and kept in the system keyring.
GtkWidget *nexus_group(AdwPreferencesDialog *dialog, nexus::Account &account) {
    GtkWidget *group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Nexus Mods");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
                                          "Your personal API key is stored in the system keyring, not in "
                                          "GTKdrop's files.");

    struct Ctx {
        AdwPreferencesDialog *dialog;
        nexus::Account *account;
        GtkWidget *status_row, *key_row, *remove_button;
        std::shared_ptr<bool> alive = std::make_shared<bool>(true);
        void refresh() {
            const bool has_key = !account->key().empty();
            std::string text = account->status_text();
            text = text.substr(text.find(':') + 2); // drop the "Nexus Mods: " prefix
            adw_action_row_set_subtitle(ADW_ACTION_ROW(status_row), text.c_str());
            gtk_widget_set_visible(remove_button, has_key);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(key_row),
                                          has_key ? "Replace API Key" : "Personal API Key");
        }
    };

    GtkWidget *status_row = plain_action_row("Account", "");
    GtkWidget *remove = icon_button("user-trash-symbolic", "Remove the Key");
    adw_action_row_add_suffix(ADW_ACTION_ROW(status_row), remove);

    GtkWidget *key_row = adw_password_entry_row_new();
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(key_row), TRUE);
    GtkWidget *get_key = icon_button("external-link-symbolic", "Open the Nexus Mods API Key Page");
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(key_row), get_key);

    auto *ctx = new Ctx{dialog, &account, status_row, key_row, remove};
    g_object_set_data_full(G_OBJECT(group), "ctx", ctx, [](gpointer p) {
        auto *c = static_cast<Ctx *>(p);
        *c->alive = false;
        delete c;
    });
    ctx->refresh();

    g_signal_connect(get_key, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer) {
        GtkUriLauncher *launcher = gtk_uri_launcher_new(nexus::kApiKeyPage);
        gtk_uri_launcher_launch(launcher, window_of(GTK_WIDGET(b)), nullptr, nullptr, nullptr);
        g_object_unref(launcher);
    }), nullptr);

    g_signal_connect(key_row, "apply", G_CALLBACK(+[](AdwEntryRow *row, gpointer p) {
        auto *c = static_cast<Ctx *>(p);
        char *key = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(row))));
        if (*key) {
            // Read-only rather than insensitive: disabling the focused field yanks its focus and GTK warns
            // "GtkText - did not receive a focus-out event".
            gtk_editable_set_editable(GTK_EDITABLE(row), FALSE);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(c->status_row), "Checking the key with Nexus Mods…");
            std::weak_ptr<bool> alive = c->alive;
            c->account->set_key(key, [c, alive](bool ok, const std::string &message) {
                if (alive.expired())
                    return;
                gtk_editable_set_editable(GTK_EDITABLE(c->key_row), TRUE);
                if (ok)
                    gtk_editable_set_text(GTK_EDITABLE(c->key_row), "");
                c->refresh();
                dialog_toast(c->dialog, message);
            });
        }
        g_free(key);
    }), ctx);

    g_signal_connect(remove, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
        auto *c = static_cast<Ctx *>(p);
        std::weak_ptr<bool> alive = c->alive;
        c->account->remove_key([c, alive] {
            if (alive.expired())
                return;
            c->refresh();
            dialog_toast(c->dialog, "Removed the key from the keyring");
        });
    }), ctx);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), status_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), key_row);

    // nxm:// links: the "Mod Manager Download" buttons on the site (collections for now).
    GtkWidget *links = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(links), "Handle Nexus Mods Links");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(links),
                                "Open “Mod Manager Download” links (nxm://) in GTKdrop instead of another manager");
    adw_switch_row_set_active(ADW_SWITCH_ROW(links), nxm_handler::is_default());
    g_signal_connect(links, "notify::active", G_CALLBACK(+[](AdwSwitchRow *row, GParamSpec *, gpointer d) {
        auto *dialog = static_cast<AdwPreferencesDialog *>(d);
        if (!adw_switch_row_get_active(row)) {
            nxm_handler::unset_default();
            return;
        }
        std::string error;
        if (!nxm_handler::make_default(&error)) {
            dialog_toast(dialog, "Couldn't register for Nexus links: " + error);
            g_signal_handlers_block_matched(row, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, d);
            adw_switch_row_set_active(row, FALSE);
            g_signal_handlers_unblock_matched(row, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, d);
        }
    }), dialog);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), links);
    return group;
}

} // namespace

void show_settings_dialog(GtkWidget *parent, ModManager &manager, nexus::Account &nexus,
                          std::function<void()> on_changed) {
    AdwDialog *dialog = adw_preferences_dialog_new();
    adw_dialog_set_title(dialog, "Settings");
    adw_preferences_dialog_set_search_enabled(ADW_PREFERENCES_DIALOG(dialog), FALSE);

    auto *sd = new SettingsDialog{ADW_PREFERENCES_DIALOG(dialog), manager, manager.settings(), std::move(on_changed),
                                  nullptr, nullptr, nullptr};
    g_object_set_data_full(G_OBJECT(dialog), "state", sd, [](gpointer p) { delete static_cast<SettingsDialog *>(p); });

    GtkWidget *page = adw_preferences_page_new();

    GtkWidget *folders = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(folders), "Folders");
    sd->game_row = folder_row(sd, "Stardew Valley Folder", &Settings::game_dir, false);
    sd->mods_row = folder_row(sd, "Mods Folder", &Settings::mods_dir, true);
    sd->collections_row = folder_row(sd, "Collections Folder", &Settings::collections_dir, true);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(folders), sd->game_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(folders), sd->mods_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(folders), sd->collections_row);
    sd->refresh_rows();

    GtkWidget *launch = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(launch), "Launch");
    {
        GtkWidget *row = adw_combo_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Start the Game");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Steam keeps the overlay, playtime and Cloud saves; "
                                                         "GOG and other copies start directly");
        const char *labels[] = {"Ask on Next Launch", "Through Steam", "Directly (GOG)", nullptr};
        GtkStringList *model = gtk_string_list_new(labels);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        g_object_unref(model);
        const auto method = launch_method_from_string(sd->settings.launch_method);
        adw_combo_row_set_selected(ADW_COMBO_ROW(row), method == LaunchMethod::Steam ? 1
                                                       : method == LaunchMethod::Gog ? 2 : 0);
        g_signal_connect(row, "notify::selected", G_CALLBACK(+[](AdwComboRow *r, GParamSpec *, gpointer p) {
            auto *sd = static_cast<SettingsDialog *>(p);
            const guint i = adw_combo_row_get_selected(r);
            sd->settings.launch_method = to_string(i == 1 ? LaunchMethod::Steam : i == 2 ? LaunchMethod::Gog
                                                                                          : LaunchMethod::Ask);
            sd->save();
        }), sd);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(launch), row);

        // The launch options Steam needs, shown again (the setup dialog appears by itself only before the first
        // Steam launch). Not for direct launches.
        GtkWidget *steam = plain_action_row("Steam Launch Options",
                                            "What to paste into Stardew Valley’s Launch Options in Steam, so "
                                            "launches from GTKdrop use the profile");
        GtkWidget *show = gtk_button_new_with_label("Show");
        gtk_widget_set_valign(show, GTK_ALIGN_CENTER);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(show), "win.steam-setup");
        adw_action_row_add_suffix(ADW_ACTION_ROW(steam), show);
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(steam), show);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(launch), steam);
        g_object_bind_property_full(row, "selected", steam, "visible", G_BINDING_SYNC_CREATE,
                                    +[](GBinding *, const GValue *from, GValue *to, gpointer) -> gboolean {
            g_value_set_boolean(to, g_value_get_uint(from) != 2); // 2: Directly (GOG)
            return TRUE;
        }, nullptr, nullptr, nullptr);
    }

    GtkWidget *profiles = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(profiles), "Profiles");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(profiles),
                              switch_row(sd, "Per-Profile Mod Configs",
                                         "Each profile keeps its own copy of every mod's config.json",
                                         &Settings::profile_mod_configs));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(profiles),
                              switch_row(sd, "Save Profile Changes Automatically",
                                         "Off: changes wait for “Save Profile”", &Settings::auto_save_profiles));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(profiles),
                              switch_row(sd, "Ignore Hidden Folders",
                                         "Skip mods inside folders starting with “.”, as SMAPI does",
                                         &Settings::ignore_hidden_folders));

    GtkWidget *collections = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(collections), "Collections");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(collections),
                              switch_row(sd, "Skip SMAPI Update Checks for Collection Mods",
                                         "A collection pins the versions of the mods it installs",
                                         &Settings::collections_skip_smapi_update_check));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(folders));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(launch));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(profiles));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(collections));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(nexus_group(ADW_PREFERENCES_DIALOG(dialog), nexus)));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));
    adw_dialog_present(dialog, parent);
}

// ---------------------------------------------------------------------------
// Download lists ("Collections" in the menu)
// ---------------------------------------------------------------------------
namespace {

namespace fs = std::filesystem;
using downloads::Job;

struct PayloadRow {
    fs::path path;
    std::string idle_subtitle; // "<n> mods · <date>"
    AdwActionRow *row;
    GtkButton *download;
    GtkButton *open_folder;
    GtkProgressBar *progress;
};

struct PayloadsDialog {
    AdwPreferencesDialog *dialog;
    std::vector<PayloadRow> rows;
    std::weak_ptr<Job> job;
    int listener = 0;
    nexus::Account *account;
};

void refresh_payload_rows(PayloadsDialog *pd) {
    auto job = Job::current();
    const bool busy = job && (job->active());
    for (auto &r: pd->rows) {
        const bool mine = job && job->payload() == r.path;
        const bool running = mine && busy;
        std::error_code ec;
        gtk_widget_set_visible(GTK_WIDGET(r.open_folder), fs::is_directory(downloads::downloads_dir_for(r.path), ec));
        gtk_button_set_icon_name(r.download, running ? "view-more-horizontal-symbolic" : "folder-download-symbolic");
        gtk_widget_set_tooltip_text(GTK_WIDGET(r.download), running ? "Show Download" : "Download");
        gtk_widget_set_sensitive(GTK_WIDGET(r.download), running || !busy);
        gtk_widget_set_visible(GTK_WIDGET(r.progress), running);
        if (running) {
            if (job->fraction() >= 0)
                gtk_progress_bar_set_fraction(r.progress, job->fraction());
            else
                gtk_progress_bar_pulse(r.progress);
        }
        const std::string subtitle = mine ? job->status() : r.idle_subtitle;
        adw_action_row_set_subtitle(r.row, subtitle.c_str());
    }
}

void watch_job(PayloadsDialog *pd) {
    auto job = Job::current();
    if (!job || pd->job.lock() == job)
        return;
    if (auto old = pd->job.lock())
        old->unlisten(pd->listener);
    pd->job = job;
    pd->listener = job->listen([pd] { refresh_payload_rows(pd); });
}

} // namespace

// ---------------------------------------------------------------------------
// Download window: status, progress and the commands being run with their output.
// ---------------------------------------------------------------------------
void show_download_dialog(GtkWidget *parent, std::function<void()> on_closed) {
    auto job = Job::current();
    if (!job)
        return;

    struct State {
        std::weak_ptr<Job> job;
        AdwDialog *dialog;
        AdwToastOverlay *toasts;
        GtkLabel *status, *counts;
        GtkProgressBar *progress;
        GtkTextView *log;
        GtkScrolledWindow *scroll;
        GtkButton *stop, *open_folder, *open_failed;
        bool follow = true; // keep scrolled to the newest line unless the user scrolled up
        int listener = 0, log_listener = 0;
        guint pulse = 0;
        std::function<void()> on_closed;
    };
    auto *st = new State{job};
    st->on_closed = std::move(on_closed);

    AdwDialog *dialog = adw_dialog_new();
    st->dialog = dialog;
    adw_dialog_set_title(dialog, ("Download · " + job->payload().filename().string()).c_str());
    adw_dialog_set_content_width(dialog, 760);
    adw_dialog_set_content_height(dialog, 520);

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
    st->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(st->toasts));
    adw_dialog_set_child(dialog, toolbar);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    adw_toast_overlay_set_child(st->toasts, box);

    st->status = GTK_LABEL(gtk_label_new(nullptr));
    gtk_label_set_xalign(st->status, 0);
    gtk_label_set_ellipsize(st->status, PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(GTK_WIDGET(st->status), "heading");
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(st->status));

    st->progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(st->progress));

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    st->counts = GTK_LABEL(gtk_label_new(nullptr));
    gtk_label_set_xalign(st->counts, 0);
    gtk_label_set_ellipsize(st->counts, PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(GTK_WIDGET(st->counts), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(st->counts), "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(st->counts));
    st->open_failed = GTK_BUTTON(gtk_button_new_with_mnemonic("Download Failed in _Browser"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(st->open_failed),
                                "Opens each failed mod's Files tab in your browser. A file's “Mod Manager Download” "
                                "button sends it back to GTKdrop, which installs it.");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(st->open_failed));
    st->open_folder = GTK_BUTTON(gtk_button_new_with_mnemonic("_Open Folder"));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(st->open_folder));
    st->stop = GTK_BUTTON(gtk_button_new_with_mnemonic("_Stop"));
    gtk_widget_add_css_class(GTK_WIDGET(st->stop), "destructive-action");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(st->stop));
    gtk_box_append(GTK_BOX(box), row);

    st->log = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(st->log, FALSE);
    gtk_text_view_set_cursor_visible(st->log, FALSE);
    gtk_text_view_set_monospace(st->log, TRUE);
    gtk_text_view_set_wrap_mode(st->log, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(st->log, 10);
    gtk_text_view_set_right_margin(st->log, 10);
    gtk_text_view_set_top_margin(st->log, 8);
    gtk_text_view_set_bottom_margin(st->log, 8);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(st->log);
    gtk_text_buffer_create_tag(buffer, "command", "weight", PANGO_WEIGHT_BOLD, nullptr);
    gtk_text_buffer_create_tag(buffer, "note", "style", PANGO_STYLE_ITALIC, nullptr);
    st->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_child(st->scroll, GTK_WIDGET(st->log));
    gtk_widget_set_vexpand(GTK_WIDGET(st->scroll), TRUE);
    GtkWidget *frame = gtk_frame_new(nullptr);
    gtk_frame_set_child(GTK_FRAME(frame), GTK_WIDGET(st->scroll));
    gtk_box_append(GTK_BOX(box), frame);

    // Follow the end while the view is at the bottom; stop following when the user scrolls up.
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(st->scroll);
    g_signal_connect(adj, "value-changed", G_CALLBACK(+[](GtkAdjustment *a, gpointer d) {
        static_cast<State *>(d)->follow = gtk_adjustment_get_value(a) + gtk_adjustment_get_page_size(a) >=
                                          gtk_adjustment_get_upper(a) - 4;
    }), st);
    g_signal_connect(adj, "changed", G_CALLBACK(+[](GtkAdjustment *a, gpointer d) {
        if (static_cast<State *>(d)->follow)
            gtk_adjustment_set_value(a, gtk_adjustment_get_upper(a) - gtk_adjustment_get_page_size(a));
    }), st);

    auto append = [](State *st, const std::string &line) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(st->log);
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        const char *tag = line.starts_with("$ ") ? "command" : line.starts_with("— ") ? "note" : nullptr;
        const std::string text = line + "\n";
        if (tag)
            gtk_text_buffer_insert_with_tags_by_name(buffer, &end, text.c_str(), -1, tag, nullptr);
        else
            gtk_text_buffer_insert(buffer, &end, text.c_str(), -1);
    };
    for (const auto &line: job->log())
        append(st, line);

    auto refresh = [](State *st) {
        auto job = st->job.lock();
        if (!job)
            return;
        gtk_label_set_text(st->status, job->status().c_str());
        if (job->fraction() >= 0)
            gtk_progress_bar_set_fraction(st->progress, job->fraction());
        const bool running = job->active();
        if (!running && job->state() == Job::State::Done)
            gtk_progress_bar_set_fraction(st->progress, 1.0);
        std::string counts = std::to_string(job->succeeded()) + " downloaded";
        if (!job->failed().empty())
            counts += " · " + std::to_string(job->failed().size()) + " failed";
        counts += " · " + std::to_string(job->total()) + " in the list";
        gtk_label_set_text(st->counts, counts.c_str());
        gtk_widget_set_tooltip_text(GTK_WIDGET(st->open_folder), job->out_dir().c_str());
        gtk_widget_set_sensitive(GTK_WIDGET(st->stop), running);
        gtk_widget_set_visible(GTK_WIDGET(st->open_failed), !running && !job->failed_urls().empty());
        std::error_code ec;
        gtk_widget_set_sensitive(GTK_WIDGET(st->open_folder), fs::is_directory(job->out_dir(), ec));
    };

    st->listener = job->listen([st, refresh] { refresh(st); });
    st->log_listener = job->listen_log([st, append](const std::string &line) { append(st, line); });
    // Unknown progress (e.g. Camoufox starting) pulses.
    st->pulse = g_timeout_add(150, [](gpointer d) -> gboolean {
        auto *st = static_cast<State *>(d);
        if (auto job = st->job.lock(); job && job->fraction() < 0 && job->active())
            gtk_progress_bar_pulse(st->progress);
        return G_SOURCE_CONTINUE;
    }, st);

    g_signal_connect(st->stop, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        if (auto job = static_cast<State *>(d)->job.lock())
            job->cancel();
    }), st);
    g_signal_connect(st->open_failed, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
        if (auto job = static_cast<State *>(d)->job.lock())
            downloads::open_in_browser(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))), job->failed_urls());
    }), st);
    g_signal_connect(st->open_folder, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
        auto job = static_cast<State *>(d)->job.lock();
        if (!job)
            return;
        GFile *file = g_file_new_for_path(job->out_dir().c_str());
        GtkFileLauncher *launcher = gtk_file_launcher_new(file);
        gtk_file_launcher_launch(launcher, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))), nullptr, nullptr, nullptr);
        g_object_unref(launcher);
        g_object_unref(file);
    }), st);

    g_object_set_data_full(G_OBJECT(dialog), "download-state", st, [](gpointer d) {
        auto *st = static_cast<State *>(d);
        if (auto job = st->job.lock()) {
            job->unlisten(st->listener);
            job->unlisten_log(st->log_listener);
        }
        g_source_remove(st->pulse);
        if (st->on_closed)
            st->on_closed();
        delete st;
    });

    adw_dialog_present(dialog, parent);
    refresh(st);
}

void show_payloads_dialog(GtkWidget *parent, nexus::Account &account) {
    struct Entry {
        fs::path path;
        fs::file_time_type modified;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    for (const auto &e: fs::directory_iterator(payloads_dir(), ec))
        if (e.is_regular_file() && e.path().extension() == ".json")
            entries.push_back({e.path(), e.last_write_time(ec)});
    std::ranges::sort(entries, std::greater{}, &Entry::modified);

    AdwDialog *dialog = adw_preferences_dialog_new();
    adw_dialog_set_title(dialog, "Download Lists");
    adw_dialog_set_content_width(dialog, 600);
    adw_dialog_set_content_height(dialog, 480);
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    auto *pd = new PayloadsDialog{ADW_PREFERENCES_DIALOG(dialog), {}, {}, 0, &account};
    g_object_set_data_full(G_OBJECT(dialog), "payloads", pd, [](gpointer d) {
        auto *pd = static_cast<PayloadsDialog *>(d);
        if (auto job = pd->job.lock())
            job->unlisten(pd->listener);
        delete pd;
    });

    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    if (entries.empty()) {
        GtkWidget *status = adw_status_page_new();
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "folder-download-symbolic");
        adw_status_page_set_title(ADW_STATUS_PAGE(status), "No Download Lists");
        adw_status_page_set_description(ADW_STATUS_PAGE(status),
                                        "Open a collection from Nexus Mods, or use Get All Updates, to make one.");
        gtk_widget_set_vexpand(status, TRUE);
        adw_preferences_group_add(group, status);
    } else {
        adw_preferences_group_set_description(group, payloads_dir().c_str());
    }

    auto icon_button = [](const char *icon, const char *tooltip) {
        GtkWidget *b = gtk_button_new_from_icon_name(icon);
        gtk_widget_set_tooltip_text(b, tooltip);
        gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(b, "flat");
        return b;
    };

    for (const auto &e: entries) {
        std::string contents;
        size_t count = 0;
        if (auto text = read_text_file(e.path)) {
            contents = *text;
            if (auto doc = parse_lenient_json(contents); doc && doc->is_object())
                if (auto mods = doc->find("mods"); mods != doc->end() && mods->is_array())
                    count = mods->size();
        }
        const auto sys_time = std::chrono::clock_cast<std::chrono::system_clock>(e.modified);
        GDateTime *dt = g_date_time_new_from_unix_local(
                std::chrono::duration_cast<std::chrono::seconds>(sys_time.time_since_epoch()).count());
        char *when = g_date_time_format(dt, "%Y-%m-%d %H:%M");
        PayloadRow r{e.path, std::to_string(count) + (count == 1 ? " mod" : " mods") + "  ·  " + when};
        g_free(when);
        g_date_time_unref(dt);

        r.row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r.row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r.row), e.path.filename().c_str());
        adw_action_row_set_subtitle_lines(r.row, 1);

        r.progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
        gtk_widget_set_valign(GTK_WIDGET(r.progress), GTK_ALIGN_CENTER);
        gtk_widget_set_size_request(GTK_WIDGET(r.progress), 100, -1);
        adw_action_row_add_suffix(r.row, GTK_WIDGET(r.progress));

        r.open_folder = GTK_BUTTON(icon_button("folder-open-symbolic", "Open Downloads Folder"));
        g_object_set_data_full(G_OBJECT(r.open_folder), "dir",
                               g_strdup(downloads::downloads_dir_for(e.path).c_str()), g_free);
        g_signal_connect(r.open_folder, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer) {
            GFile *file = g_file_new_for_path(static_cast<const char *>(g_object_get_data(G_OBJECT(b), "dir")));
            GtkFileLauncher *launcher = gtk_file_launcher_new(file);
            gtk_file_launcher_launch(launcher, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))), nullptr, nullptr, nullptr);
            g_object_unref(launcher);
            g_object_unref(file);
        }), nullptr);
        adw_action_row_add_suffix(r.row, GTK_WIDGET(r.open_folder));

        GtkWidget *browser = icon_button("web-browser-symbolic", "Download in Browser: opens each mod's page; its "
                                                                 "“Mod Manager Download” button sends the file back "
                                                                 "to GTKdrop");
        gtk_widget_set_sensitive(browser, count > 0);
        g_object_set_data_full(G_OBJECT(browser), "path", g_strdup(e.path.c_str()), g_free);
        g_signal_connect(browser, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
            auto *pd = static_cast<PayloadsDialog *>(d);
            const fs::path path = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "path"));
            auto urls = downloads::payload_urls(path);
            if (urls.empty())
                return;
            if (urls.size() <= 10) {
                downloads::open_in_browser(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))), urls);
                return;
            }
            AdwDialog *ask = adw_alert_dialog_new(("Open " + std::to_string(urls.size()) + " Pages?").c_str(),
                                                  "Each mod in the list opens in its own browser tab.");
            adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(ask), "cancel", "_Cancel", "open", "_Open", nullptr);
            adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(ask), "cancel");
            adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(ask), "open", ADW_RESPONSE_SUGGESTED);
            auto *keep = new std::vector<std::string>(std::move(urls));
            g_signal_connect_data(ask, "response", G_CALLBACK(+[](AdwAlertDialog *a, const char *response, gpointer d) {
                if (std::string_view(response) == "open")
                    downloads::open_in_browser(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(a))),
                                               *static_cast<std::vector<std::string> *>(d));
            }), keep, [](gpointer d, GClosure *) { delete static_cast<std::vector<std::string> *>(d); },
                                  GConnectFlags(0));
            adw_dialog_present(ask, GTK_WIDGET(pd->dialog));
        }), pd);
        adw_action_row_add_suffix(r.row, browser);

        r.download = GTK_BUTTON(icon_button("folder-download-symbolic", "Download"));
        g_object_set_data_full(G_OBJECT(r.download), "path", g_strdup(e.path.c_str()), g_free);
        g_signal_connect(r.download, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
            auto *pd = static_cast<PayloadsDialog *>(d);
            const fs::path path = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "path"));
            auto job = Job::current();
            const bool running = job && (job->active());
            if (!running || job->payload() != path) {
                if (running) // the button is insensitive then; just in case
                    return;
                job = Job::start(path, *pd->account, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(pd->dialog))));
                if (!job) {
                    dialog_toast(pd->dialog, "This list has no mods to download");
                    return;
                }
                watch_job(pd);
                refresh_payload_rows(pd);
            }
            show_download_dialog(GTK_WIDGET(pd->dialog));
        }), pd);
        adw_action_row_add_suffix(r.row, GTK_WIDGET(r.download));

        GtkWidget *copy = icon_button("edit-copy-symbolic", "Copy");
        g_object_set_data_full(G_OBJECT(copy), "contents", g_strdup(contents.c_str()), g_free);
        g_signal_connect(copy, "clicked", G_CALLBACK(+[](GtkButton *button, gpointer d) {
            const char *text = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "contents"));
            gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button)), text);
            dialog_toast(ADW_PREFERENCES_DIALOG(d), "Copied");
        }), dialog);
        adw_action_row_add_suffix(r.row, copy);

        adw_preferences_group_add(group, GTK_WIDGET(r.row));
        pd->rows.push_back(r);
    }
    adw_preferences_page_add(page, group);
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), page);
    watch_job(pd);
    refresh_payload_rows(pd);
    adw_dialog_present(dialog, parent);
}

} // namespace gtkdrop::ui
