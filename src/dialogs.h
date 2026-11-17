#pragma once

#include "core/manager.h"
#include "core/nexus.h"

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>

namespace gtkdrop::ui {

// Lists the profiles with rename / delete, plus a row to add one (empty or copied from the active profile).
// `on_changed` runs after every change so the window can refresh its profile selector.
void show_profiles_dialog(GtkWidget *parent, ModManager &manager, std::function<void()> on_changed);

// Game / mods / collections folders and the profile preferences. `on_changed` runs after settings are saved.
void show_settings_dialog(GtkWidget *parent, ModManager &manager, nexus::Account &nexus,
                          std::function<void()> on_changed);

// Lists the download lists (payload files) GTKdrop wrote, newest first, each with a button that copies its JSON.
void show_payloads_dialog(GtkWidget *parent, nexus::Account &account);

// The running (or last) download: status, progress, and every command run with its output.
void show_download_dialog(GtkWidget *parent, std::function<void()> on_closed = {});

void choose_folder(GtkWindow *parent, const char *title, const std::filesystem::path &initial,
                   std::function<void(std::filesystem::path)> on_chosen);

} // namespace gtkdrop::ui
