#pragma once

#include <filesystem>

namespace gtkdrop::log {

// ~/.config/gtkdrop/log/gtkdrop.log, started fresh on every run.
std::filesystem::path file_path();

// Opens the log file and routes every GLib/GTK and GTKdrop message (g_message, g_warning, …) into it as well as
// the terminal. Debug builds also add a stack trace after each warning or critical, since GLib only reports
// where its check failed, never who called it (for a debugger stop instead: G_DEBUG=fatal-criticals).
void init();

} // namespace gtkdrop::log
