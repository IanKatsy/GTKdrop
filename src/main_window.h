#pragma once

#include <gtk/gtk.h>

// Builds the main window (mod list, profile bar, filter bar, status bar).
GtkWindow *gtkdrop_main_window_new(GtkApplication *app);

// Handles a URI given to the app, e.g. an nxm:// link from the Nexus site.
void gtkdrop_main_window_open_uri(GtkWindow *window, const char *uri);
