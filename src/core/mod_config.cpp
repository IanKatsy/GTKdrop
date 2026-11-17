#include "mod_config.h"

#include <glib.h>

namespace gtkdrop {

json merge_config(const json &base, const json &values) {
    if (base.is_object() && values.is_object()) {
        json out = json::object();
        for (auto it = base.begin(); it != base.end(); ++it) {
            auto v = values.find(it.key());
            out[it.key()] = (v == values.end() || v->is_null()) ? it.value() : merge_config(it.value(), *v);
        }
        return out;
    }
    return values.is_null() ? base : values;
}

namespace {

std::optional<json> read_config(const Mod &mod) {
    return mod.has_config() ? read_json_file(mod.config_file) : std::nullopt;
}

} // namespace

bool capture_mod_configs(Profile &profile, const std::vector<Mod> &mods) {
    bool changed = false;
    for (const auto &mod: mods) {
        if (mod.is_from_collection() || !profile.is_enabled(mod))
            continue;
        auto config = read_config(mod);
        if (!config)
            continue;
        auto &slot = profile.preserved_configs[to_lower(mod.unique_id())];
        if (slot != *config) {
            slot = std::move(*config);
            changed = true;
        }
    }
    return changed;
}

int apply_mod_configs(const Profile &profile, std::vector<Mod> &mods) {
    int written = 0;
    for (auto &mod: mods) {
        if (mod.is_from_collection() || !profile.is_enabled(mod))
            continue;
        auto saved = profile.preserved_configs.find(to_lower(mod.unique_id()));
        if (saved == profile.preserved_configs.end())
            continue;

        json target = saved->second;
        if (auto current = read_config(mod)) {
            if (current->is_object() && saved->second.is_object())
                target = merge_config(*current, saved->second);
            if (target == *current)
                continue;
        }

        const auto path = mod.has_config() ? mod.config_file : mod.folder / "config.json";
        if (write_json_file(path, target)) {
            mod.config_file = path;
            ++written;
        }
    }
    return written;
}

} // namespace gtkdrop
