#include <adwaita.h>

#include "log.h"
#include "main_window.h"

static void on_startup(GApplication *app, gpointer) {
    // style.css is loaded by AdwApplication from the resource base path (/dev/gtkdrop/GTKdrop/).
    struct {
        const char *action;
        const char *accels[3];
    } const shortcuts[] = {
        {"win.add-mods", {"<Control>o", nullptr}},
        {"win.add-mod-folders", {"<Control><Shift>o", nullptr}},
        {"win.close", {"<Control>q", "<Control>w", nullptr}},
        {"win.refresh-mods", {"F5", nullptr}},
        {"win.focus-search", {"<Control>f", nullptr}},
        {"win.settings", {"<Control>comma", nullptr}},
    };
    for (const auto &s: shortcuts)
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), s.action, s.accels);
}

static GtkWindow *main_window(GApplication *app) {
    if (GtkWindow *existing = gtk_application_get_active_window(GTK_APPLICATION(app)))
        return existing;
    return gtkdrop_main_window_new(GTK_APPLICATION(app));
}

static void on_activate(GApplication *app, gpointer) {
    gtk_window_present(main_window(app));
}

// nxm:// links from the Nexus site ("Mod Manager Download"). A second GTKdrop started by the browser hands the
// link to the running one over D-Bus and exits, so links always land in the open window.
static void on_open(GApplication *app, GFile **files, int n_files, const char *, gpointer) {
    GtkWindow *window = main_window(app);
    gtk_window_present(window);
    for (int i = 0; i < n_files; ++i) {
        char *uri = g_file_get_uri(files[i]);
        gtkdrop_main_window_open_uri(window, uri);
        g_free(uri);
    }
}

int main(int argc, char *argv[]) {
    gtkdrop::log::init();
    // The Nexus Mods window (WebKitGTK) freezes on some GPU drivers with its DMA-BUF renderer; its pages are light, so
    // the shared-memory one costs nothing. Set before any web process starts; an explicit setting wins.
    g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", FALSE);
    AdwApplication *app = adw_application_new("dev.gtkdrop.GTKdrop", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    g_signal_connect(app, "open", G_CALLBACK(on_open), nullptr);

    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
