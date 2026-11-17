#pragma once

#include <adwaita.h>

namespace gtkdrop::ui {

// Shows a toast with a countdown bar under its text that shows how long it stays up. GTKdrop runs the countdown
// itself (libadwaita's toasts have no progress display), pausing while the pointer is over the toast or the
// window isn't focused. Toasts with timeout 0 stay until dismissed and get no bar.
void show_toast(AdwToastOverlay *overlay, AdwToast *toast);
void show_toast(AdwPreferencesDialog *dialog, AdwToast *toast);

} // namespace gtkdrop::ui
