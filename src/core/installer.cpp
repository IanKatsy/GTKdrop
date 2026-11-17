#include "installer.h"

#include "json_util.h"
#include "mod.h"
#include "process.h"

#include <glib.h>
#include <gio/gio.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace gtkdrop {

namespace {

std::string archive_kind(const fs::path &p) {
    const std::string name = to_lower(p.filename().string());
    for (const char *ext: {".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".tgz", ".tar", ".zip", ".7z", ".rar"})
        if (name.size() > std::strlen(ext) && name.ends_with(ext))
            return ext;
    return {};
}

std::string archive_stem(const fs::path &p) {
    const std::string name = p.filename().string();
    return name.substr(0, name.size() - archive_kind(p).size());
}

std::string tool(std::initializer_list<const char *> names) {
    for (const char *n: names)
        if (char *path = g_find_program_in_path(n)) {
            std::string result = path;
            g_free(path);
            return result;
        }
    return {};
}

// Archives made on Windows can carry the read-only attribute, which unzip turns into folders without write
// permission: those can't be moved into place (renaming a folder rewrites its "..") or deleted later.
void make_writable(const fs::path &root) {
    std::error_code ec;
    auto fix = [](const fs::directory_entry &e) {
        std::error_code ec2;
        if (e.is_symlink(ec2))
            return;
        const auto add = e.is_directory(ec2) ? fs::perms::owner_all : fs::perms::owner_read | fs::perms::owner_write;
        fs::permissions(e.path(), add, fs::perm_options::add, ec2);
    };
    fix(fs::directory_entry(root, ec));
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        fix(*it);
        if (ec)
            break;
    }
}

// Moves a folder, copying when it crosses file systems (Downloads and the game are often on different disks).
bool move_dir(const fs::path &from, const fs::path &to, std::string &error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (!ec)
        return true;
    if (ec != std::errc::cross_device_link) {
        error = to.filename().string() + ": " + ec.message();
        return false;
    }
    fs::copy(from, to, fs::copy_options::recursive, ec);
    if (ec) {
        error = to.filename().string() + ": " + ec.message();
        fs::remove_all(to, ec);
        return false;
    }
    fs::remove_all(from, ec);
    return true;
}

// Old copies go to the trash so an update can be undone; a mods folder without a trash keeps them as backups.
bool retire(const fs::path &old_dir, const fs::path &backup_dir, std::string &error) {
    GFile *file = g_file_new_for_path(old_dir.c_str());
    const bool trashed = g_file_trash(file, nullptr, nullptr);
    g_object_unref(file);
    if (trashed)
        return true;
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    const fs::path to = backup_dir / (old_dir.filename().string() + "-" + stamp);
    g_free(stamp);
    g_date_time_unref(now);
    return move_dir(old_dir, to, error);
}

// Folders holding a manifest.json (not looking inside them: a mod's own subfolders aren't mods).
void find_mod_dirs(const fs::path &dir, std::vector<fs::path> &out) {
    std::error_code ec;
    if (fs::exists(dir / "manifest.json", ec)) {
        out.push_back(dir);
        return;
    }
    for (const auto &e: fs::directory_iterator(dir, ec))
        if (e.is_directory())
            find_mod_dirs(e.path(), out);
}

bool contains_mod(const fs::path &dir) {
    std::vector<fs::path> found;
    find_mod_dirs(dir, found);
    return !found.empty();
}

struct Run {
    fs::path work_dir; // the download folder, or a scratch folder for the user's own files
    InstallTarget target;
    std::function<void(InstallResult)> done;
    std::function<void(const std::string &)> log;
    std::vector<fs::path> archives; // archives, or (the user's own files) mod folders
    bool downloads = true; // downloaded archives: recorded in .installed and deleted once installed
    size_t next = 0;
    std::vector<fs::path> overlays; // config-only archives, laid over the installed mods last
    InstallResult result;
    PlacedArchive current; // what the archive being placed put where

    void note(const std::string &line) const {
        if (log)
            log(line);
    }

    fs::path unpack_dir() const { return work_dir / ".unpack"; }
    fs::path backup_dir() const { return work_dir / "replaced"; }
};

// Places what one archive unpacked to. Returns false (with errors added) if nothing could be installed.
bool place(Run &run, const fs::path &archive) {
    std::error_code ec;
    fs::path root = run.unpack_dir();
    run.current = {archive};
    make_writable(root);
    // Some archives have manifest.json at the top instead of in a folder: give them one.
    if (fs::exists(root / "manifest.json", ec)) {
        const fs::path wrapped = run.work_dir / ".unpack-wrap";
        fs::remove_all(wrapped, ec);
        fs::create_directories(wrapped, ec);
        fs::rename(root, wrapped / archive_stem(archive), ec);
        fs::rename(wrapped, root, ec);
    }

    const int before = run.result.mods;
    // The mod folders under `from`, as they'll be under `to`.
    auto mods_moving = [](const fs::path &from, const fs::path &to) {
        std::vector<fs::path> found, out;
        find_mod_dirs(from, found);
        for (const auto &f: found)
            out.push_back(f == from ? to : to / f.lexically_relative(from));
        return out;
    };
    auto install_at = [&](const fs::path &from, const fs::path &to) {
        std::string error;
        if (fs::exists(to, ec) && !retire(to, run.backup_dir(), error)) {
            run.result.errors.push_back("Couldn't replace " + error);
            return;
        }
        const auto mods = mods_moving(from, to);
        if (move_dir(from, to, error)) {
            ++run.result.mods;
            run.current.mods.insert(run.current.mods.end(), mods.begin(), mods.end());
            run.note("  installed " + to.string());
        } else {
            run.result.errors.push_back("Couldn't install " + error);
            run.note("  couldn't install " + error);
        }
    };

    if (run.target.kind == InstallTarget::Kind::Collection) {
        // Keep the archive's own layout (one folder may group several mods), skipping loose readmes. A collection
        // can hold several files of one mod (main file + patch) that unpack into the same folder: merge into it
        // instead of replacing what an earlier archive put there.
        for (const auto &e: fs::directory_iterator(root, ec)) {
            if (!e.is_directory() || !contains_mod(e.path()))
                continue;
            const fs::path to = run.target.dest / e.path().filename();
            if (!fs::exists(to, ec)) {
                install_at(e.path(), to);
                continue;
            }
            std::error_code copy_ec;
            const auto mods = mods_moving(e.path(), to);
            fs::copy(e.path(), to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, copy_ec);
            if (copy_ec) {
                run.result.errors.push_back("Couldn't add to " + to.filename().string() + ": " + copy_ec.message());
                run.note("  couldn't add to " + to.string() + ": " + copy_ec.message());
            } else {
                ++run.result.mods;
                run.current.merged.insert(run.current.merged.end(), mods.begin(), mods.end());
                run.note("  added to " + to.string());
            }
        }
    } else {
        std::vector<fs::path> mods;
        find_mod_dirs(root, mods);
        for (const auto &dir: mods) {
            auto doc = read_json_file(dir / "manifest.json");
            auto manifest = doc ? Manifest::parse(*doc) : std::nullopt;
            auto it = manifest ? run.target.installed.find(to_lower(manifest->unique_id)) : run.target.installed.end();
            if (it == run.target.installed.end()) {
                install_at(dir, run.target.dest / dir.filename());
                continue;
            }
            // The update keeps the user's settings: config.json isn't shipped, SMAPI writes it.
            if (fs::exists(it->second / "config.json", ec) && !fs::exists(dir / "config.json", ec))
                fs::copy_file(it->second / "config.json", dir / "config.json", ec);
            install_at(dir, it->second);
        }
    }
    if (run.result.mods == before) {
        // A collection's own settings pack (e.g. "VERY Configured"): no manifests, just config.json files in the
        // same folders as the mods. Laid over them once every other archive is in.
        if (run.target.kind == InstallTarget::Kind::Collection && !contains_mod(root)) {
            const bool second_pass = std::ranges::find(run.overlays, archive) != run.overlays.end();
            int matching = 0;
            for (const auto &e: fs::directory_iterator(root, ec))
                if (e.is_directory() && fs::is_directory(run.target.dest / e.path().filename(), ec))
                    ++matching;
            if (matching > 0 && !second_pass) {
                run.note("  only settings for mods: applied after the other archives");
                run.overlays.push_back(archive);
                run.archives.push_back(archive);
                return false; // not done yet; not an error either
            }
            if (matching > 0) {
                for (const auto &e: fs::directory_iterator(root, ec)) {
                    const fs::path to = run.target.dest / e.path().filename();
                    if (!e.is_directory() || !fs::is_directory(to, ec))
                        continue;
                    std::error_code copy_ec;
                    fs::copy(e.path(), to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, copy_ec);
                    if (copy_ec)
                        run.result.errors.push_back("Couldn't apply settings to " + to.filename().string() + ": " +
                                                    copy_ec.message());
                    else {
                        run.current.settings_for.push_back(to);
                        run.note("  settings applied to " + to.string());
                    }
                }
                return true;
            }
        }
        if (!contains_mod(root) && !run.downloads) {
            run.result.errors.push_back(archive.filename().string() + ": not a SMAPI mod (no manifest.json inside)");
            run.note("  " + run.result.errors.back());
            return false;
        }
        if (!contains_mod(root)) {
            // Not a SMAPI mod (collections list SMAPI's own installer, for one): nothing to install, nothing to retry.
            run.note("  skipped: no manifest.json inside (not a SMAPI mod, e.g. the SMAPI installer)");
            return true;
        }
        run.result.errors.push_back(archive.filename().string() + ": couldn't install its mods");
        return false;
    }
    return true;
}

void next_archive(std::shared_ptr<Run> run) {
    std::error_code ec;
    if (run->next >= run->archives.size()) {
        fs::remove_all(run->unpack_dir(), ec);
        fs::remove_all(run->work_dir / ".unpack-wrap", ec);
        run->done(std::move(run->result));
        return;
    }
    const fs::path archive = run->archives[run->next++];
    fs::remove_all(run->unpack_dir(), ec);
    fs::create_directories(run->unpack_dir(), ec);
    // A mod folder the user picked: a copy goes through the same placing as an unpacked archive.
    if (fs::is_directory(archive, ec)) {
        run->note("$ copy " + archive.string());
        fs::copy(archive, run->unpack_dir() / archive.filename(),
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec)
            run->result.errors.push_back(archive.filename().string() + ": " + ec.message());
        else if (place(*run, archive)) {
            ++run->result.archives;
            if (!run->current.empty())
                run->result.placed.push_back(std::move(run->current));
        }
        next_archive(run);
        return;
    }
    const auto argv = extract_command(archive, run->unpack_dir());
    if (argv.empty()) {
        run->result.errors.push_back(archive.filename().string() + ": no tool to unpack " + archive_kind(archive) +
                                     " archives (install 7-Zip, unrar or libarchive's bsdtar)");
        run->note("  " + run->result.errors.back());
        next_archive(run);
        return;
    }
    std::string error;
    run->note(command_line(argv));
    ProcessOptions options;
    if (run->log)
        options.on_line = [run](const std::string &line) { run->note("  " + line); };
    GSubprocess *p = run_process(argv, options, [run, archive](int code, std::string output) {
        if (code != 0) {
            g_warning("Unpacking %s failed (exit %d):\n%s", archive.c_str(), code, output.c_str());
            run->result.errors.push_back(archive.filename().string() + ": " + last_line(output));
        } else if (place(*run, archive)) {
            ++run->result.archives;
            if (!run->current.empty())
                run->result.placed.push_back(std::move(run->current));
            if (run->downloads) {
                // Recorded, then deleted: the record is what download lists and the collection check go by.
                const std::string list = read_text_file(run->work_dir / ".installed").value_or("");
                write_text_file(run->work_dir / ".installed", list + archive.filename().string() + "\n");
                std::error_code rm;
                fs::remove(archive, rm);
            }
            g_message("Installed %s", archive.filename().c_str());
        }
        next_archive(run);
    }, &error);
    if (p)
        g_object_unref(p);
    else {
        run->result.errors.push_back(archive.filename().string() + ": " + error);
        next_archive(run);
    }
}

} // namespace

std::vector<std::string> extract_command(const fs::path &archive, const fs::path &into) {
    const std::string kind = archive_kind(archive);
    const std::string a = archive.string(), d = into.string();
    auto seven = [&](const std::string &t) { return std::vector<std::string>{t, "x", "-y", "-o" + d, a}; };
    auto bsd = [&](const std::string &t) { return std::vector<std::string>{t, "-xf", a, "-C", d}; };
    if (kind == ".zip") {
        if (auto t = tool({"unzip"}); !t.empty())
            return {t, "-q", "-o", a, "-d", d};
        if (auto t = tool({"7z", "7zz", "7za"}); !t.empty())
            return seven(t);
        if (auto t = tool({"bsdtar"}); !t.empty())
            return bsd(t);
    } else if (kind == ".7z") {
        if (auto t = tool({"7z", "7zz", "7za"}); !t.empty())
            return seven(t);
        if (auto t = tool({"bsdtar"}); !t.empty())
            return bsd(t);
    } else if (kind == ".rar") {
        if (auto t = tool({"unrar"}); !t.empty())
            return {t, "x", "-o+", "-idq", a, d + "/"};
        if (auto t = tool({"7z", "7zz"}); !t.empty()) // 7za has no RAR support
            return seven(t);
        if (auto t = tool({"bsdtar"}); !t.empty())
            return bsd(t);
    } else if (!kind.empty()) {
        if (auto t = tool({"bsdtar", "tar"}); !t.empty())
            return bsd(t);
    }
    return {};
}

void install_archives(const fs::path &download_dir, InstallTarget target, std::function<void(InstallResult)> done,
                      std::function<void(const std::string &)> log) {
    auto run = std::make_shared<Run>(Run{download_dir, std::move(target), std::move(done), std::move(log)});
    remove_installed_archives(download_dir); // left by versions of GTKdrop that kept them
    std::error_code ec;
    for (const auto &e: fs::directory_iterator(download_dir, ec))
        if (e.is_regular_file() && !archive_kind(e.path()).empty())
            run->archives.push_back(e.path());
    std::ranges::sort(run->archives);
    next_archive(run);
}

void install_files(std::vector<fs::path> sources, const fs::path &work_dir, InstallTarget target,
                   std::function<void(InstallResult)> done, std::function<void(const std::string &)> log) {
    auto run = std::make_shared<Run>(Run{work_dir, std::move(target), std::move(done), std::move(log)});
    run->downloads = false;
    std::error_code ec;
    fs::create_directories(work_dir, ec);
    for (auto &s: sources) {
        if (!fs::is_directory(s, ec) && archive_kind(s).empty()) {
            run->result.errors.push_back(s.filename().string() + ": not an archive or a folder");
            continue;
        }
        run->archives.push_back(std::move(s));
    }
    next_archive(run);
}

bool copy_mod_folder(const fs::path &from, const fs::path &to, std::string &error) {
    std::error_code ec;
    if (fs::exists(to, ec)) {
        error = to.string() + " already exists";
        return false;
    }
    const fs::path tmp = to.parent_path() / ("." + to.filename().string() + ".copying");
    fs::remove_all(tmp, ec);
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, tmp, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (!ec) {
        make_writable(tmp);
        fs::rename(tmp, to, ec);
    }
    if (ec) {
        error = ec.message();
        std::error_code ignore;
        fs::remove_all(tmp, ignore);
        return false;
    }
    return true;
}

std::vector<std::string> installed_archive_names(const fs::path &download_dir) {
    std::vector<std::string> names;
    if (auto list = read_text_file(download_dir / ".installed"))
        for (size_t start = 0; start < list->size();) {
            const size_t end = list->find('\n', start);
            if (end != start)
                names.push_back(list->substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    return names;
}

void remove_installed_archives(const fs::path &download_dir) {
    std::error_code ec;
    for (const auto &name: installed_archive_names(download_dir))
        if (!name.empty() && name.find('/') == std::string::npos)
            fs::remove(download_dir / name, ec);
}

} // namespace gtkdrop
