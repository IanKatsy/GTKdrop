#include "mod_item.h"

enum {
    PROP_0,
    PROP_ENABLED,
    PROP_HAS_CONFIG,
    PROP_STATUS,
    // String properties from here on; they're stored in GtkdropModItem::strings.
    PROP_NAME,
    PROP_UNIQUE_ID,
    PROP_AUTHOR,
    PROP_VERSION,
    PROP_LAST_UPDATED,
    PROP_REQUIREMENTS,
    PROP_MISSING_REQUIREMENTS,
    PROP_STATUS_TEXT,
    PROP_NOTES,
    N_PROPS,
};

constexpr int kFirstStringProp = PROP_NAME;
constexpr int kNumStrings = N_PROPS - kFirstStringProp;

struct _GtkdropModItem {
    GObject parent_instance;
    gboolean enabled;
    gboolean has_config;
    int status;
    char *strings[kNumStrings];
};

G_DEFINE_FINAL_TYPE(GtkdropModItem, gtkdrop_mod_item, G_TYPE_OBJECT)

static GParamSpec *props[N_PROPS];

static void gtkdrop_mod_item_finalize(GObject *object) {
    auto *self = GTKDROP_MOD_ITEM(object);
    for (auto &s: self->strings)
        g_free(s);
    G_OBJECT_CLASS(gtkdrop_mod_item_parent_class)->finalize(object);
}

static void gtkdrop_mod_item_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec) {
    auto *self = GTKDROP_MOD_ITEM(object);
    switch (id) {
        case PROP_ENABLED: g_value_set_boolean(value, self->enabled);
            break;
        case PROP_HAS_CONFIG: g_value_set_boolean(value, self->has_config);
            break;
        case PROP_STATUS: g_value_set_int(value, self->status);
            break;
        default:
            if (id >= kFirstStringProp && id < N_PROPS)
                g_value_set_string(value, self->strings[id - kFirstStringProp]);
            else
                G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
    }
}

static void gtkdrop_mod_item_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec) {
    auto *self = GTKDROP_MOD_ITEM(object);
    switch (id) {
        case PROP_ENABLED:
            gtkdrop_mod_item_set_enabled(self, g_value_get_boolean(value));
            break;
        case PROP_HAS_CONFIG:
            if (self->has_config != g_value_get_boolean(value)) {
                self->has_config = g_value_get_boolean(value);
                g_object_notify_by_pspec(object, pspec);
            }
            break;
        case PROP_STATUS:
            if (self->status != g_value_get_int(value)) {
                self->status = g_value_get_int(value);
                g_object_notify_by_pspec(object, pspec);
            }
            break;
        default:
            if (id >= kFirstStringProp && id < N_PROPS) {
                char *&slot = self->strings[id - kFirstStringProp];
                const char *v = g_value_get_string(value);
                if (!v) v = "";
                if (g_strcmp0(slot, v) != 0) {
                    g_free(slot);
                    slot = g_strdup(v);
                    g_object_notify_by_pspec(object, pspec);
                }
            } else {
                G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
            }
    }
}

static void gtkdrop_mod_item_class_init(GtkdropModItemClass *klass) {
    auto *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gtkdrop_mod_item_finalize;
    object_class->get_property = gtkdrop_mod_item_get_property;
    object_class->set_property = gtkdrop_mod_item_set_property;

    constexpr auto flags = static_cast<GParamFlags>(
        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    props[PROP_ENABLED] = g_param_spec_boolean("enabled", nullptr, nullptr, FALSE, flags);
    props[PROP_HAS_CONFIG] = g_param_spec_boolean("has-config", nullptr, nullptr, FALSE, flags);
    props[PROP_STATUS] = g_param_spec_int("status", nullptr, nullptr,
                                          GTKDROP_MOD_STATUS_NONE, GTKDROP_MOD_STATUS_BROKEN,
                                          GTKDROP_MOD_STATUS_NONE, flags);

    static constexpr const char *string_props[kNumStrings] = {
        "name", "unique-id", "author", "version", "last-updated",
        "requirements", "missing-requirements", "status-text", "notes",
    };
    for (int i = 0; i < kNumStrings; ++i)
        props[kFirstStringProp + i] = g_param_spec_string(string_props[i], nullptr, nullptr, "", flags);

    g_object_class_install_properties(object_class, N_PROPS, props);
}

static void gtkdrop_mod_item_init(GtkdropModItem *self) {
    for (auto &s: self->strings)
        s = g_strdup("");
}

GtkdropModItem *gtkdrop_mod_item_new(const char *unique_id, const char *name, const char *author, const char *version) {
    return GTKDROP_MOD_ITEM(g_object_new(GTKDROP_TYPE_MOD_ITEM,
        "unique-id", unique_id,
        "name", name,
        "author", author,
        "version", version,
        nullptr));
}

gboolean gtkdrop_mod_item_get_enabled(GtkdropModItem *self) { return self->enabled; }

void gtkdrop_mod_item_set_enabled(GtkdropModItem *self, gboolean enabled) {
    enabled = !!enabled;
    if (self->enabled == enabled)
        return;
    self->enabled = enabled;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ENABLED]);
}

gboolean gtkdrop_mod_item_get_has_config(GtkdropModItem *self) { return self->has_config; }

GtkdropModStatus gtkdrop_mod_item_get_status(GtkdropModItem *self) {
    return static_cast<GtkdropModStatus>(self->status);
}

void gtkdrop_mod_item_set_status(GtkdropModItem *self, GtkdropModStatus status, const char *status_text) {
    g_object_set(self, "status", static_cast<int>(status), "status-text", status_text, nullptr);
}

const char *gtkdrop_mod_item_get_name(GtkdropModItem *self) { return self->strings[PROP_NAME - kFirstStringProp]; }
const char *gtkdrop_mod_item_get_author(GtkdropModItem *self) { return self->strings[PROP_AUTHOR - kFirstStringProp]; }

const char *gtkdrop_mod_item_get_requirements(GtkdropModItem *self) {
    return self->strings[PROP_REQUIREMENTS - kFirstStringProp];
}
