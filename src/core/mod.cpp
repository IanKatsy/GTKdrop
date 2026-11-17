#include "mod.h"

#include <glib.h>

#include <cstring>
#include <map>

namespace gtkdrop {

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------
namespace {

struct ParsedVersion {
    std::vector<long> numbers;
    std::string prerelease; // after '-', without build metadata
};

ParsedVersion parse_version(std::string_view v) {
    ParsedVersion p;
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
        v.remove_prefix(1);
    if (auto plus = v.find('+'); plus != std::string_view::npos)
        v = v.substr(0, plus);
    if (auto dash = v.find('-'); dash != std::string_view::npos) {
        p.prerelease = std::string(v.substr(dash + 1));
        v = v.substr(0, dash);
    }
    size_t start = 0;
    while (start <= v.size()) {
        const size_t dot = v.find('.', start);
        const auto part = v.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        p.numbers.push_back(std::strtol(std::string(part).c_str(), nullptr, 10));
        if (dot == std::string_view::npos)
            break;
        start = dot + 1;
    }
    return p;
}

} // namespace

int compare_versions(std::string_view a, std::string_view b) {
    const auto pa = parse_version(a), pb = parse_version(b);
    const size_t n = std::max(pa.numbers.size(), pb.numbers.size());
    for (size_t i = 0; i < n; ++i) {
        const long x = i < pa.numbers.size() ? pa.numbers[i] : 0;
        const long y = i < pb.numbers.size() ? pb.numbers[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    // A pre-release sorts before the release it leads up to.
    if (pa.prerelease.empty() != pb.prerelease.empty())
        return pa.prerelease.empty() ? 1 : -1;
    return g_ascii_strcasecmp(pa.prerelease.c_str(), pb.prerelease.c_str());
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------
std::optional<Manifest> Manifest::parse(const json &doc) {
    if (!doc.is_object())
        return std::nullopt;

    Manifest m;
    m.unique_id = get_string_ci(doc, "UniqueID");
    if (m.unique_id.empty())
        return std::nullopt;
    m.name = get_string_ci(doc, "Name", m.unique_id);
    m.author = get_string_ci(doc, "Author");
    m.description = get_string_ci(doc, "Description");

    // Versions are usually strings, but old manifests use {"MajorVersion":1,"MinorVersion":2,"PatchVersion":3}.
    if (const json *v = find_key_ci(doc, "Version")) {
        if (v->is_string()) {
            m.version = v->get<std::string>();
        } else if (v->is_object()) {
            auto num = [&](const char *k) {
                const json *x = find_key_ci(*v, k);
                return x && x->is_number_integer() ? x->get<long>() : 0L;
            };
            m.version = std::to_string(num("MajorVersion")) + "." + std::to_string(num("MinorVersion")) + "." +
                        std::to_string(num("PatchVersion"));
        }
    }

    if (const json *cpf = find_key_ci(doc, "ContentPackFor"); cpf && cpf->is_object())
        m.content_pack_for = get_string_ci(*cpf, "UniqueID");

    if (const json *deps = find_key_ci(doc, "Dependencies"); deps && deps->is_array()) {
        for (const json &d: *deps) {
            Dependency dep;
            dep.unique_id = get_string_ci(d, "UniqueID");
            if (dep.unique_id.empty())
                continue;
            dep.minimum_version = get_string_ci(d, "MinimumVersion");
            if (const json *req = find_key_ci(d, "IsRequired"); req && req->is_boolean())
                dep.required = req->get<bool>();
            m.dependencies.push_back(std::move(dep));
        }
    }

    if (const json *keys = find_key_ci(doc, "UpdateKeys"); keys && keys->is_array())
        for (const json &k: *keys)
            if (k.is_string())
                m.update_keys.push_back(k.get<std::string>());

    return m;
}

// ---------------------------------------------------------------------------
// Mod
// ---------------------------------------------------------------------------
std::string Mod::missing_requirements_text() const {
    std::string text;
    for (const auto &r: requirements)
        if (r.missing)
            text += (text.empty() ? "" : ", ") + r.name;
    return text;
}

int Mod::nexus_id() const {
    for (const auto &key: manifest.update_keys) {
        const auto colon = key.find(':');
        if (colon == std::string::npos)
            continue;
        std::string site = key.substr(0, colon);
        g_strstrip(site.data());
        site.resize(std::strlen(site.c_str()));
        if (!iequals(site, "Nexus"))
            continue;
        const long id = std::strtol(key.c_str() + colon + 1, nullptr, 10); // stops at "@subkey"
        if (id > 0)
            return static_cast<int>(id);
    }
    return 0;
}

bool Mod::has_update() const {
    return !latest_version.empty() && compare_versions(manifest.version, latest_version) < 0;
}

std::string Mod::requirements_text() const {
    std::string text;
    for (const auto &r: requirements)
        text += (text.empty() ? "" : ", ") + r.name;
    return text;
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------
namespace {

bool is_same_or_under(const fs::path &path, const fs::path &parent) {
    std::error_code ec;
    const auto p = fs::weakly_canonical(path, ec);
    const auto r = fs::weakly_canonical(parent, ec);
    auto [pi, ri] = std::mismatch(p.begin(), p.end(), r.begin(), r.end());
    return ri == r.end();
}

// Whether any folder between the scan root and the mod starts with '.', which SMAPI skips. Measured from the
// root, as the root itself may sit under a dotted folder (the collections root under ~/.config).
bool has_hidden_segment(const fs::path &root, const fs::path &mod_folder) {
    std::error_code ec;
    const auto rel = fs::relative(mod_folder, root, ec);
    if (ec)
        return false;
    for (const auto &segment: rel)
        if (const auto s = segment.string(); !s.empty() && s[0] == '.' && s != "." && s != "..")
            return true;
    return false;
}

struct FoundFolder {
    fs::path root;
    fs::path manifest;
    fs::path config;
};

void walk(const fs::path &root, const fs::path &dir, std::vector<FoundFolder> &out, int depth = 0) {
    if (depth > 16) // guards against symlink loops
        return;
    std::error_code ec;
    for (const auto &entry: fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_directory(ec))
            continue;
        fs::path manifest, config;
        for (const auto &file: fs::directory_iterator(entry.path(), fs::directory_options::skip_permission_denied, ec)) {
            const auto name = file.path().filename().string();
            if (iequals(name, "manifest.json"))
                manifest = file.path();
            else if (iequals(name, "config.json"))
                config = file.path();
        }
        if (manifest.empty())
            walk(root, entry.path(), out, depth + 1); // a group folder: look inside
        else
            out.push_back({root, manifest, config});
    }
}

// The collection a mod belongs to: the first folder below the collections root.
std::string collection_source_id(const fs::path &mod_folder, const fs::path &collections_root) {
    if (collections_root.empty() || !is_same_or_under(mod_folder, collections_root))
        return {};
    std::error_code ec;
    const auto rel = fs::relative(fs::weakly_canonical(mod_folder, ec), fs::weakly_canonical(collections_root, ec), ec);
    if (ec || rel.empty() || *rel.begin() == ".")
        return {};
    return rel.begin()->string();
}

std::map<std::string, std::string> load_collection_names(const fs::path &cache_dir) {
    std::map<std::string, std::string> names;
    std::error_code ec;
    for (const auto &entry: fs::directory_iterator(cache_dir, ec)) {
        if (entry.path().extension() != ".json")
            continue;
        if (auto doc = read_json_file(entry.path())) {
            const auto source = get_string_ci(*doc, "SourceId", entry.path().stem().string());
            auto name = get_string_ci(*doc, "Name");
            if (name.empty())
                name = get_string_ci(*doc, "Slug", source);
            names[to_lower(source)] = name;
        }
    }
    return names;
}

} // namespace

std::vector<Mod> discover_mods(const DiscoveryOptions &options) {
    std::vector<fs::path> roots;
    std::error_code ec;
    if (!options.mods_dir.empty() && fs::is_directory(options.mods_dir, ec))
        roots.push_back(options.mods_dir);
    // Skipped when the mods folder already contains it, which would walk every collection twice.
    if (!options.collections_dir.empty() && fs::is_directory(options.collections_dir, ec) &&
        !(roots.size() == 1 && is_same_or_under(options.collections_dir, roots[0])))
        roots.push_back(options.collections_dir);

    std::vector<FoundFolder> found;
    for (const auto &root: roots)
        walk(root, root, found);

    const auto collection_names = load_collection_names(options.collection_cache_dir);

    std::vector<Mod> mods;
    std::map<std::pair<std::string, std::string>, size_t> index; // (uid, source) lower-cased -> position
    for (const auto &f: found) {
        const auto folder = f.manifest.parent_path();
        if (options.ignore_hidden_folders && has_hidden_segment(f.root, folder))
            continue;

        auto doc = read_json_file(f.manifest);
        auto manifest = doc ? Manifest::parse(*doc) : std::nullopt;
        if (!manifest) {
            g_warning("Skipping %s: manifest.json is missing a UniqueID or isn't valid JSON", folder.c_str());
            continue;
        }

        Mod mod;
        mod.manifest = std::move(*manifest);
        mod.folder = folder;
        mod.config_file = f.config;
        mod.source_id = collection_source_id(folder, options.collections_dir);
        if (mod.is_from_collection()) {
            auto it = collection_names.find(to_lower(mod.source_id));
            mod.collection_name = it != collection_names.end() ? it->second : mod.source_id;
        }
        const auto mtime = fs::last_write_time(f.manifest, ec);
        if (!ec)
            mod.modified = std::chrono::clock_cast<std::chrono::system_clock>(mtime);

        if (!mod.manifest.content_pack_for.empty())
            mod.requirements.push_back({mod.manifest.content_pack_for, {}, mod.manifest.content_pack_for});
        for (const auto &d: mod.manifest.dependencies) {
            if (!d.required)
                continue;
            const bool dup = std::ranges::any_of(mod.requirements, [&](const Requirement &r) {
                return iequals(r.unique_id, d.unique_id);
            });
            if (!dup)
                mod.requirements.push_back({d.unique_id, d.minimum_version, d.unique_id});
        }

        const auto key = std::make_pair(to_lower(mod.unique_id()), to_lower(mod.source_id));
        if (auto it = index.find(key); it == index.end()) {
            index[key] = mods.size();
            mods.push_back(std::move(mod));
        } else if (compare_versions(mods[it->second].manifest.version, mod.manifest.version) < 0) {
            mods[it->second] = std::move(mod); // keep the newer copy
        }
    }

    evaluate_requirements(mods);
    return mods;
}

void evaluate_requirements(std::vector<Mod> &mods) {
    for (auto &mod: mods) {
        for (auto &req: mod.requirements) {
            // Prefer a copy from the same collection, then any copy.
            const Mod *match = nullptr;
            for (const auto &other: mods) {
                if (!iequals(other.unique_id(), req.unique_id))
                    continue;
                if (!match || iequals(other.source_id, mod.source_id))
                    match = &other;
            }
            if (match)
                req.name = match->name();
            req.missing = !match || (!req.minimum_version.empty() &&
                                     compare_versions(match->manifest.version, req.minimum_version) < 0);
        }
    }
}

} // namespace gtkdrop
