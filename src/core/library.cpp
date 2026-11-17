#include "library.h"

#include "json_util.h"
#include "mod.h"

#include <glib.h>

#include <algorithm>

namespace gtkdrop {

namespace {

constexpr int kFormat = 1;

// Folders are compared as written by the installer and the scan, both absolute; tidy them the same way.
fs::path normal(const fs::path &p) {
    std::error_code ec;
    fs::path n = fs::weakly_canonical(p, ec);
    if (ec)
        n = p.lexically_normal();
    if (!n.has_filename())
        n = n.parent_path();
    return n;
}

bool is_there(const fs::path &p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

json to_json(const LibraryEntry &e) {
    json mods = json::array();
    for (const auto &m: e.mods)
        mods.push_back({{"folder", m.folder.string()}, {"uniqueId", m.unique_id}, {"version", m.version}});
    json settings = json::array();
    for (const auto &f: e.settings_for)
        settings.push_back(f.string());
    json j = {{"archive", e.archive}, {"origin", e.origin}, {"installedAt", e.installed_at}, {"mods", mods}};
    if (e.nexus_mod_id > 0 || e.nexus_file_id > 0)
        j["nexus"] = {{"modId", e.nexus_mod_id}, {"fileId", e.nexus_file_id}};
    if (!e.source_id.empty())
        j["sourceId"] = e.source_id;
    if (!e.payload.empty())
        j["payload"] = e.payload;
    if (!e.settings_for.empty())
        j["settingsFor"] = settings;
    return j;
}

std::optional<LibraryEntry> from_json(const json &j) {
    if (!j.is_object())
        return std::nullopt;
    LibraryEntry e;
    e.archive = j.value("archive", "");
    e.origin = j.value("origin", "");
    e.installed_at = j.value("installedAt", "");
    e.source_id = j.value("sourceId", "");
    e.payload = j.value("payload", "");
    if (auto n = j.find("nexus"); n != j.end() && n->is_object()) {
        e.nexus_mod_id = n->value("modId", 0);
        e.nexus_file_id = n->value("fileId", 0);
    }
    if (auto mods = j.find("mods"); mods != j.end() && mods->is_array())
        for (const auto &m: *mods)
            if (m.is_object() && m.contains("folder") && m["folder"].is_string())
                e.mods.push_back({m["folder"].get<std::string>(), m.value("uniqueId", ""), m.value("version", "")});
    if (auto s = j.find("settingsFor"); s != j.end() && s->is_array())
        for (const auto &f: *s)
            if (f.is_string())
                e.settings_for.emplace_back(f.get<std::string>());
    return e;
}

} // namespace

bool LibraryEntry::present() const {
    return std::ranges::any_of(mods, [](const LibraryMod &m) { return is_there(m.folder); }) ||
           std::ranges::any_of(settings_for, [](const fs::path &f) { return is_there(f); });
}

Library::Library(fs::path file) : file_(std::move(file)) {}

void Library::load() {
    entries_.clear();
    const auto doc = read_json_file(file_);
    if (!doc || !doc->is_object())
        return;
    if (doc->value("format", 0) > kFormat)
        g_warning("%s is from a newer GTKdrop; reading what this version understands", file_.c_str());
    if (auto list = doc->find("entries"); list != doc->end() && list->is_array())
        for (const auto &j: *list)
            if (auto e = from_json(j))
                entries_.push_back(std::move(*e));
}

bool Library::save() const {
    json list = json::array();
    for (const auto &e: entries_)
        list.push_back(to_json(e));
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    const bool ok = write_json_file(file_, {{"format", kFormat}, {"entries", list}});
    if (!ok)
        g_warning("Couldn't write %s", file_.c_str());
    return ok;
}

void Library::add(LibraryEntry entry) {
    for (auto &m: entry.mods) {
        m.folder = normal(m.folder);
        if (auto doc = read_json_file(m.folder / "manifest.json"))
            if (auto manifest = Manifest::parse(*doc)) {
                m.unique_id = manifest->unique_id;
                m.version = manifest->version;
            }
    }
    for (auto &f: entry.settings_for)
        f = normal(f);
    if (entry.installed_at.empty())
        entry.installed_at = library_timestamp();

    // The folders now hold this entry's files.
    for (auto &old: entries_)
        std::erase_if(old.mods, [&](const LibraryMod &m) {
            return std::ranges::any_of(entry.mods, [&](const LibraryMod &n) { return !n.merged && n.folder == m.folder; });
        });
    // The same file installed again into the same place: one entry for it.
    std::erase_if(entries_, [&](const LibraryEntry &old) {
        const bool same_file = entry.nexus_file_id > 0 && old.nexus_file_id == entry.nexus_file_id &&
                               old.source_id == entry.source_id;
        return same_file || (old.mods.empty() && old.settings_for.empty());
    });
    entries_.push_back(std::move(entry));
    save();
}

bool Library::prune() {
    auto gone = [](const fs::path &f) { return !is_there(f) && is_there(f.parent_path()); };
    bool changed = false;
    for (auto &e: entries_) {
        changed |= std::erase_if(e.mods, [&](const LibraryMod &m) { return gone(m.folder); }) > 0;
        changed |= std::erase_if(e.settings_for, gone) > 0;
    }
    changed |= std::erase_if(entries_, [](const LibraryEntry &e) { return e.mods.empty() && e.settings_for.empty(); }) > 0;
    return changed;
}

const LibraryEntry *Library::find_file(int file_id, std::string_view source_id) const {
    if (file_id <= 0)
        return nullptr;
    const LibraryEntry *loose = nullptr;
    for (const auto &e: entries_) {
        if (e.nexus_file_id != file_id || !e.present())
            continue;
        if (iequals(e.source_id, source_id))
            return &e;
        if (e.source_id.empty() && !loose)
            loose = &e;
    }
    return loose;
}

const LibraryEntry *Library::find_folder(const fs::path &folder, const LibraryMod **mod) const {
    const fs::path f = normal(folder);
    for (const auto &e: entries_)
        for (const auto &m: e.mods)
            if (m.folder == f) {
                if (mod)
                    *mod = &m;
                return &e;
            }
    return nullptr;
}

std::string library_timestamp() {
    GDateTime *now = g_date_time_new_now_utc();
    char *text = g_date_time_format_iso8601(now);
    std::string result = text ? text : "";
    g_free(text);
    g_date_time_unref(now);
    return result;
}

} // namespace gtkdrop
