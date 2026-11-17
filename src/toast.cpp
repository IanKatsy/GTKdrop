#include "toast.h"

#include <algorithm>

// ---------------------------------------------------------------------------------------------------------------
// The bar: a widget with no size of its own, overlaid on the toast's text, that paints a 3px strip along the
// bottom edge of the whole toast (measured from the toast, not from itself). The toast clips it to its rounded
// corners, so it reads as part of the toast without touching libadwaita's layout or padding.
// ---------------------------------------------------------------------------------------------------------------
G_DECLARE_FINAL_TYPE(GtkdropToastBar, gtkdrop_toast_bar, GTKDROP, TOAST_BAR, GtkWidget)

struct _GtkdropToastBar {
    GtkWidget parent_instance;
    double fraction;
};

G_DEFINE_FINAL_TYPE(GtkdropToastBar, gtkdrop_toast_bar, GTK_TYPE_WIDGET)

namespace {

constexpr float kBarHeight = 3;

GtkWidget *toast_widget_of(GtkWidget *w) {
    while (w && g_strcmp0(gtk_widget_get_css_name(w), "toast") != 0)
        w = gtk_widget_get_parent(w);
    return w;
}

} // namespace

static void gtkdrop_toast_bar_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    auto *self = GTKDROP_TOAST_BAR(widget);
    GtkWidget *toast = toast_widget_of(widget);
    graphene_rect_t bounds;
    if (!toast || !gtk_widget_compute_bounds(toast, widget, &bounds))
        return;

    GdkRGBA trough, fill;
    gtk_widget_get_color(widget, &trough);
    fill = trough;
    trough.alpha *= 0.15f;
    fill.alpha *= 0.55f;

    graphene_rect_t r = GRAPHENE_RECT_INIT(bounds.origin.x, bounds.origin.y + bounds.size.height - kBarHeight,
                                           bounds.size.width, kBarHeight);
    gtk_snapshot_append_color(snapshot, &trough, &r);
    r.size.width *= static_cast<float>(std::clamp(self->fraction, 0.0, 1.0));
    gtk_snapshot_append_color(snapshot, &fill, &r);
}

static void gtkdrop_toast_bar_measure(GtkWidget *, GtkOrientation, int, int *min, int *nat, int *, int *) {
    *min = *nat = 0;
}

static void gtkdrop_toast_bar_class_init(GtkdropToastBarClass *klass) {
    GTK_WIDGET_CLASS(klass)->snapshot = gtkdrop_toast_bar_snapshot;
    GTK_WIDGET_CLASS(klass)->measure = gtkdrop_toast_bar_measure;
}

static void gtkdrop_toast_bar_init(GtkdropToastBar *self) {
    self->fraction = 1.0;
    gtk_widget_set_can_target(GTK_WIDGET(self), FALSE);
}

namespace gtkdrop::ui {

namespace {

struct Countdown {
    AdwToast *toast = nullptr;         // weak: the overlay owns it
    GtkWidget *toast_widget = nullptr; // weak: libadwaita's widget showing it
    GtkdropToastBar *bar = nullptr;
    double total = 0;     // seconds
    double remaining = 0; // seconds
    gint64 last_frame = 0;
    bool hovered = false;
};

// Hovering anywhere on the toast (text, button, close) pauses it, as does the window losing focus.
void watch_toast_widget(Countdown *c) {
    if (c->toast_widget || !(c->toast_widget = toast_widget_of(GTK_WIDGET(c->bar))))
        return;
    g_object_add_weak_pointer(G_OBJECT(c->toast_widget), reinterpret_cast<gpointer *>(&c->toast_widget));
    gtk_widget_set_overflow(c->toast_widget, GTK_OVERFLOW_HIDDEN); // clip the bar to the rounded corners

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(+[](GtkEventControllerMotion *, double, double, gpointer d) {
        static_cast<Countdown *>(d)->hovered = true;
    }), c);
    g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion *, gpointer d) {
        static_cast<Countdown *>(d)->hovered = false;
    }), c);
    gtk_widget_add_controller(c->toast_widget, motion);
    g_object_set_data(G_OBJECT(c->bar), "motion", motion);
}

gboolean tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    auto *c = static_cast<Countdown *>(data);
    watch_toast_widget(c);

    const gint64 now = gdk_frame_clock_get_frame_time(clock);
    const double dt = c->last_frame ? double(now - c->last_frame) / G_USEC_PER_SEC : 0;
    c->last_frame = now;

    auto *window = GTK_WINDOW(gtk_widget_get_root(widget));
    if (!c->hovered && (!window || gtk_window_is_active(window)))
        c->remaining -= dt;

    c->bar->fraction = std::max(0.0, c->remaining / c->total);
    gtk_widget_queue_draw(widget);
    if (c->remaining <= 0) {
        if (c->toast)
            adw_toast_dismiss(c->toast);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// Swaps the plain title for text + bar and takes the timeout over from libadwaita.
void add_countdown(AdwToast *toast) {
    const guint timeout = adw_toast_get_timeout(toast);
    if (timeout == 0)
        return;

    auto *c = new Countdown{};
    c->total = c->remaining = timeout;
    c->toast = toast;
    g_object_add_weak_pointer(G_OBJECT(toast), reinterpret_cast<gpointer *>(&c->toast));
    adw_toast_set_timeout(toast, 0);

    GtkWidget *label = gtk_label_new(adw_toast_get_title(toast));
    gtk_label_set_use_markup(GTK_LABEL(label), adw_toast_get_use_markup(toast));
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(label), 0);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_valign(overlay, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), label);
    c->bar = GTKDROP_TOAST_BAR(g_object_new(gtkdrop_toast_bar_get_type(), nullptr));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(c->bar));

    // Frame ticks only run while the toast is on screen, so a queued toast's countdown starts when it shows.
    gtk_widget_add_tick_callback(GTK_WIDGET(c->bar), tick, c, [](gpointer d) {
        auto *c = static_cast<Countdown *>(d);
        if (c->toast_widget) {
            if (auto *motion = static_cast<GtkEventController *>(g_object_get_data(G_OBJECT(c->bar), "motion")))
                gtk_widget_remove_controller(c->toast_widget, motion);
            g_object_remove_weak_pointer(G_OBJECT(c->toast_widget), reinterpret_cast<gpointer *>(&c->toast_widget));
        }
        if (c->toast)
            g_object_remove_weak_pointer(G_OBJECT(c->toast), reinterpret_cast<gpointer *>(&c->toast));
        delete c;
    });

    adw_toast_set_custom_title(toast, overlay);
}

} // namespace

void show_toast(AdwToastOverlay *overlay, AdwToast *toast) {
    add_countdown(toast);
    adw_toast_overlay_add_toast(overlay, toast);
}

void show_toast(AdwPreferencesDialog *dialog, AdwToast *toast) {
    add_countdown(toast);
    adw_preferences_dialog_add_toast(dialog, toast);
}

} // namespace gtkdrop::ui
