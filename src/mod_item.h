#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

// One row of the mod list. A GObject so it can live in a GListStore and be bound to widgets.
#define GTKDROP_TYPE_MOD_ITEM (gtkdrop_mod_item_get_type())
G_DECLARE_FINAL_TYPE(GtkdropModItem, gtkdrop_mod_item, GTKDROP, MOD_ITEM, GObject)

typedef enum {
    GTKDROP_MOD_STATUS_NONE,
    GTKDROP_MOD_STATUS_UPDATE,
    GTKDROP_MOD_STATUS_UNOFFICIAL,
    GTKDROP_MOD_STATUS_BROKEN,
} GtkdropModStatus;

GtkdropModItem *gtkdrop_mod_item_new(const char *unique_id, const char *name, const char *author, const char *version);

gboolean gtkdrop_mod_item_get_enabled(GtkdropModItem *self);
void gtkdrop_mod_item_set_enabled(GtkdropModItem *self, gboolean enabled);
gboolean gtkdrop_mod_item_get_has_config(GtkdropModItem *self);
GtkdropModStatus gtkdrop_mod_item_get_status(GtkdropModItem *self);
void gtkdrop_mod_item_set_status(GtkdropModItem *self, GtkdropModStatus status, const char *status_text);

const char *gtkdrop_mod_item_get_name(GtkdropModItem *self);
const char *gtkdrop_mod_item_get_author(GtkdropModItem *self);
const char *gtkdrop_mod_item_get_requirements(GtkdropModItem *self);

G_END_DECLS
