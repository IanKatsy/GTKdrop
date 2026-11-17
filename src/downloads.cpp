#include "downloads.h"

#include "core/http_download.h"
#include "core/installer.h"
#include "core/json_util.h"
#include "core/payload.h"
#include "core/settings.h"
#include "nexus_browser.h"

#include <cstdlib>

namespace gtkdrop::downloads {

namespace {

std::string mb(goffset bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    return buf;
}

// "https://www.nexusmods.com/<domain>/mods/<id>?tab=files&file_id=<file>" -> domain, id, file
bool parse_mod_url(const std::string &url, std::string &domain, int &mod_id, int &file_id) {
    const auto mods = url.find("/mods/");
    if (mods == std::string::npos)
        return false;
    const auto host = url.find("nexusmods.com/");
    if (host != std::string::npos && host + 14 < mods)
        domain = url.substr(host + 14, mods - host - 14);
    mod_id = std::atoi(url.c_str() + mods + 6);
    file_id = 0;
    if (const auto f = url.find("file_id="); f != std::string::npos)
        file_id = std::atoi(url.c_str() + f + 8);
    return mod_id > 0;
}

} // namespace

fs::path downloads_dir() { return paths::data_dir() / "downloads"; }

fs::path downloads_dir_for(const fs::path &payload) { return downloads_dir() / payload.stem(); }

void move_old_downloads() {
    const char *home_downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    const fs::path old = fs::path(home_downloads ? home_downloads : g_get_home_dir()) / "GTKdrop";
    std::error_code ec;
    if (!fs::is_directory(old, ec))
        return;
    fs::create_directories(downloads_dir(), ec);
    for (const auto &e: fs::directory_iterator(old, ec)) {
        if (!e.is_directory())
            continue;
        const fs::path to = downloads_dir() / e.path().filename();
        std::error_code move_ec;
        if (fs::exists(to, move_ec)) {
            g_warning("Downloads: not moving %s, %s exists", e.path().c_str(), to.c_str());
            continue;
        }
        fs::rename(e.path(), to, move_ec);
        if (move_ec == std::errc::cross_device_link) {
            move_ec.clear();
            fs::copy(e.path(), to, fs::copy_options::recursive, move_ec);
            if (!move_ec)
                fs::remove_all(e.path(), move_ec);
        }
        if (move_ec) {
            g_warning("Downloads: couldn't move %s to %s: %s", e.path().c_str(), to.c_str(), move_ec.message().c_str());
            continue;
        }
        remove_installed_archives(to);
        g_message("Downloads: moved %s to %s", e.path().c_str(), to.c_str());
    }
    fs::remove(old, ec); // only when empty
}

namespace {
std::vector<std::string> lines_of(const fs::path &file) {
    std::vector<std::string> out;
    gchar *text = nullptr;
    if (!g_file_get_contents(file.c_str(), &text, nullptr, nullptr))
        return out;
    gchar **lines = g_strsplit(text, "\n", -1);
    for (gchar **l = lines; *l; ++l)
        if (**l)
            out.emplace_back(*l);
    g_strfreev(lines);
    g_free(text);
    return out;
}
} // namespace

void record_file(const fs::path &dir, int file_id, const std::string &archive) {
    auto files = recorded_files(dir);
    if (files[file_id] == archive)
        return;
    files[file_id] = archive;
    std::string text;
    for (const auto &[id, name]: files)
        text += std::to_string(id) + "\t" + name + "\n";
    g_file_set_contents((dir / ".nexus-files").c_str(), text.c_str(), gssize(text.size()), nullptr);
}

std::map<int, std::string> recorded_files(const fs::path &dir) {
    std::map<int, std::string> files;
    for (const auto &line: lines_of(dir / ".nexus-files"))
        if (const auto tab = line.find('\t'); tab != std::string::npos)
            if (const int id = std::atoi(line.c_str()); id > 0)
                files[id] = line.substr(tab + 1);
    return files;
}

std::vector<std::string> installed_archives(const fs::path &dir) { return installed_archive_names(dir); }

std::vector<std::string> payload_urls(const fs::path &payload) {
    std::vector<std::string> urls;
    if (auto doc = read_json_file(payload); doc && doc->is_object())
        if (auto mods = doc->find("mods"); mods != doc->end() && mods->is_array())
            for (const auto &entry: *mods)
                if (entry.is_object())
                    for (auto it = entry.begin(); it != entry.end(); ++it)
                        if (it.value().is_string())
                            urls.push_back(it.value().get<std::string>());
    return urls;
}

fs::path payload_with_file(int mod_id, int file_id, int *list_file_id) {
    std::error_code ec;
    fs::path by_mod;
    std::vector<int> by_mod_files;
    for (const auto &e: fs::directory_iterator(payloads_dir(), ec)) {
        if (e.path().extension() != ".json")
            continue;
        std::vector<int> files;
        for (const auto &url: payload_urls(e.path())) {
            std::string domain;
            int m = 0, f = 0;
            if (!parse_mod_url(url, domain, m, f) || m != mod_id)
                continue;
            if (f == file_id) {
                if (list_file_id)
                    *list_file_id = file_id;
                return e.path();
            }
            files.push_back(f);
        }
        if (!files.empty() && by_mod.empty()) {
            by_mod = e.path();
            by_mod_files = std::move(files);
        }
    }
    if (list_file_id)
        *list_file_id = by_mod_files.size() == 1 ? by_mod_files.front() : 0;
    return by_mod;
}

void open_in_browser(GtkWindow *parent, const std::vector<std::string> &urls) {
    struct Opening {
        GtkWindow *parent;
        std::vector<std::string> urls;
        size_t next = 0;
    };
    // Files tab, no file_id; one tab per mod (a collection can list several files of one mod).
    std::vector<std::string> pages;
    for (std::string url: urls) {
        if (const auto f = url.find("&file_id="); f != std::string::npos)
            url.erase(f, url.find('&', f + 1) == std::string::npos ? std::string::npos : url.find('&', f + 1) - f);
        if (std::ranges::find(pages, url) == pages.end())
            pages.push_back(std::move(url));
    }
    if (pages.empty())
        return;
    auto *o = new Opening{parent, std::move(pages)};
    if (parent)
        g_object_add_weak_pointer(G_OBJECT(parent), reinterpret_cast<gpointer *>(&o->parent));
    auto open_next = [](gpointer d) -> gboolean {
        auto *o = static_cast<Opening *>(d);
        if (o->next < o->urls.size()) {
            GtkUriLauncher *launcher = gtk_uri_launcher_new(o->urls[o->next++].c_str());
            gtk_uri_launcher_launch(launcher, o->parent, nullptr, nullptr, nullptr);
            g_object_unref(launcher);
        }
        if (o->next < o->urls.size())
            return G_SOURCE_CONTINUE;
        if (o->parent)
            g_object_remove_weak_pointer(G_OBJECT(o->parent), reinterpret_cast<gpointer *>(&o->parent));
        delete o;
        return G_SOURCE_REMOVE;
    };
    if (open_next(o) == G_SOURCE_CONTINUE)
        g_timeout_add(700, open_next, o);
}

std::shared_ptr<Job> &Job::current() {
    static std::shared_ptr<Job> job;
    return job;
}

std::shared_ptr<Job> Job::start(const fs::path &payload, const nexus::Account &account, GtkWindow *parent) {
    auto job = std::shared_ptr<Job>(new Job());
    job->weak_self_ = job;
    job->payload_ = payload;
    job->out_dir_ = downloads_dir_for(payload);
    job->account_ = &account;
    job->account_alive_ = account.alive_token();
    job->parent_ = parent;
    if (auto doc = read_json_file(payload); doc && doc->is_object())
        if (auto mods = doc->find("mods"); mods != doc->end() && mods->is_array())
            for (const auto &entry: *mods)
                if (entry.is_object())
                    for (auto it = entry.begin(); it != entry.end(); ++it) {
                        Item item{it.key()};
                        if (it.value().is_string() &&
                            parse_mod_url(it.value().get<std::string>(), job->domain_, item.mod_id, item.file_id))
                            job->items_.push_back(std::move(item));
                    }
    if (job->items_.empty())
        return nullptr;

    current() = job;
    std::error_code ec;
    fs::create_directories(job->out_dir_, ec);
    const bool premium = account.user() && account.user()->premium;
    job->append_log("— " + std::to_string(job->items_.size()) + " files from " + payload.filename().string() +
                    (premium ? " (Premium: straight from the API)"
                             : " (through the Nexus Mods window: “Slow download”, then the API)"));
    job->next();
    return job;
}

Job::~Job() {
    if (pause_)
        g_source_remove(pause_);
    g_cancellable_cancel(cancellable_);
    g_object_unref(cancellable_);
}

int Job::listen(std::function<void()> fn) {
    listeners_[++next_listener_] = std::move(fn);
    return next_listener_;
}
void Job::unlisten(int id) { listeners_.erase(id); }

int Job::listen_log(std::function<void(const std::string &)> fn) {
    log_listeners_[++next_listener_] = std::move(fn);
    return next_listener_;
}
void Job::unlisten_log(int id) { log_listeners_.erase(id); }

void Job::append_log(std::string line) {
    if (log_.size() >= 20000)
        log_.erase(log_.begin(), log_.begin() + 5000);
    log_.push_back(std::move(line));
    auto listeners = log_listeners_;
    for (auto &[id, fn]: listeners)
        fn(log_.back());
}

void Job::set(State state, std::string status, double fraction) {
    state_ = state;
    status_ = std::move(status);
    fraction_ = fraction;
    auto listeners = listeners_;
    for (auto &[id, fn]: listeners)
        fn();
}

void Job::finish(State state, std::string status) {
    if (pause_)
        g_source_remove(pause_);
    pause_ = 0;
    // Closes the Nexus window, later: this may run inside one of its own callbacks.
    if (auto *browser = browser_.release())
        g_idle_add_once([](gpointer b) { delete static_cast<ui::NexusBrowser *>(b); }, browser);
    g_message("Downloads: %s", status.c_str());
    append_log("— " + status);
    set(state, std::move(status));
    if (on_finished)
        on_finished(*this);
}

void Job::cancel() {
    if (!active())
        return;
    cancelled_ = true;
    g_cancellable_cancel(cancellable_);
    if (browser_)
        browser_->cancel(); // reports back through the request's callback, which ends the job
    if (!active())
        return;
    if (pause_ || state_ == State::Waiting) {
        if (pause_)
            g_source_remove(pause_);
        pause_ = 0;
        next();
    }
}

void Job::next() {
    if (cancelled_) {
        finish(State::Cancelled, "Stopped after " + std::to_string(succeeded_) + " of " +
                                         std::to_string(items_.size()) + " files");
        return;
    }
    if (account_alive_.expired()) {
        finish(State::Failed, "GTKdrop’s Nexus Mods account went away");
        return;
    }
    if (index_ >= items_.size()) {
        std::string status = "Downloaded " + std::to_string(succeeded_) + " of " + std::to_string(items_.size()) +
                             " files";
        if (!failed_.empty())
            status += " (" + std::to_string(failed_.size()) + " failed)";
        finish(State::Done, status);
        return;
    }
    Item &item = items_[index_];
    set(State::Downloading, std::to_string(index_ + 1) + " of " + std::to_string(items_.size()) + ": " + item.name,
        double(index_) / double(items_.size()));
    append_log("$ " + item.name + " — " + nexus::mod_page_url(item.mod_id, item.file_id));
    g_message("Downloads %s: starting (%d done, %zu failed so far)", tag(index_).c_str(), succeeded_, failed_.size());

    if (item.file_id > 0) {
        check_existing(index_);
        return;
    }
    nexus::fetch_main_file(*account_, domain_, item.mod_id,
                           [w = self(), i = index_](std::optional<int> file_id, const std::string &error) {
        auto job = w.lock();
        if (!job || job->index_ != i)
            return;
        if (!file_id) {
            job->item_failed(i, error);
            return;
        }
        job->items_[i].file_id = *file_id;
        job->append_log("  newest main file: " + std::to_string(*file_id));
        job->check_existing(i);
    });
}

// Already in the folder (an earlier, interrupted run): skip it without opening its page.
void Job::check_existing(size_t i) {
    const Item &item = items_[i];
    nexus::fetch_file_name(*account_, domain_, item.mod_id, item.file_id,
                           [w = self(), i](std::optional<std::string> name, const std::string &) {
        auto job = w.lock();
        if (!job || job->index_ != i)
            return;
        if (name) {
            job->items_[i].file_name = *name;
            std::error_code ec;
            if (std::ranges::contains(installed_archives(job->out_dir_), *name)) {
                record_file(job->out_dir_, job->items_[i].file_id, *name);
                ++job->succeeded_;
                job->append_log("  already installed: " + *name);
                job->advance(false);
                return;
            }
            if (fs::exists(job->out_dir_ / *name, ec)) {
                record_file(job->out_dir_, job->items_[i].file_id, *name);
                ++job->succeeded_;
                job->append_log("  already downloaded: " + *name);
                g_message("Downloads %s: already downloaded", job->tag(i).c_str());
                job->advance(false);
                return;
            }
        }
        job->get_links(i);
    });
}

void Job::get_links(size_t i) {
    const Item &item = items_[i];
    auto got_links = [w = self(), i](std::vector<std::string> urls, const std::string &error) {
        auto job = w.lock();
        if (!job || job->index_ != i)
            return;
        if (urls.empty())
            job->item_failed(i, error);
        else
            job->download(i, urls.front());
    };

    if (account_->user() && account_->user()->premium) {
        nexus::fetch_download_links(*account_, domain_, item.mod_id, item.file_id, {}, {}, got_links);
        return;
    }
    if (!browser_)
        browser_ = std::make_unique<ui::NexusBrowser>(parent_);
    append_log("  opening the file page, clicking “Slow download”…");
    browser_->request(domain_, item.mod_id, item.file_id,
                      [w = self(), i, got_links](std::optional<std::string> nxm, const std::string &error) {
        auto job = w.lock();
        if (!job || job->index_ != i)
            return;
        if (!nxm) {
            if (job->cancelled_) {
                job->next();
            } else {
                job->append_log("  " + error);
                job->cancelled_ = true; // closing the window stops the downloads
                job->next();
            }
            return;
        }
        const auto link = nexus::parse_nxm(*nxm);
        if (!link || link->key.empty()) {
            job->item_failed(i, "the page gave a link GTKdrop can't use");
            return;
        }
        job->set(State::Downloading, job->status_, job->fraction_);
        job->append_log("  got the download key (valid until " + link->expires + ")");
        g_message("Downloads %s: got the Mod Manager Download link", job->tag(i).c_str());
        nexus::fetch_download_links(*job->account_, job->domain_, job->items_[i].mod_id, job->items_[i].file_id,
                                    link->key, link->expires, got_links);
    }, [w = self(), i](const std::string &what) {
        auto job = w.lock();
        if (!job || job->index_ != i)
            return;
        job->append_log("  waiting: " + what);
        job->set(State::Waiting, what, job->fraction_);
    });
}

void Job::download(size_t i, const std::string &url) {
    const std::string base = std::to_string(i + 1) + " of " + std::to_string(items_.size()) + ": " + items_[i].name;
    GUri *uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, nullptr);
    append_log(std::string("  downloading from ") + (uri ? g_uri_get_host(uri) : "the Nexus CDN"));
    if (uri)
        g_uri_unref(uri);
    gint64 last_update = 0;
    download_file(url, out_dir_, items_[i].file_name, cancellable_,
                  [w = self(), i, base, last_update](goffset received, goffset total) mutable {
        const gint64 now = g_get_monotonic_time();
        if (now - last_update < 250000)
            return;
        last_update = now;
        if (auto job = w.lock(); job && job->index_ == i) {
            const double part = total > 0 ? double(received) / double(total) : 0;
            job->set(State::Downloading, base + " · " + mb(received) + (total > 0 ? " of " + mb(total) : ""),
                     (double(i) + part) / double(job->items_.size()));
        }
    }, [w = self(), i](fs::path path, const std::string &error) {
        auto job = w.lock();
        if (!job || job->index_ != i)
            return;
        if (job->cancelled_) {
            job->next();
            return;
        }
        if (!error.empty()) {
            job->item_failed(i, error);
            return;
        }
        ++job->succeeded_;
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        job->append_log("  saved " + path.filename().string());
        record_file(job->out_dir_, job->items_[i].file_id, path.filename().string());
        g_message("Downloads %s: saved %s (%s)", job->tag(i).c_str(), path.filename().c_str(),
                  ec ? "?" : mb(goffset(size)).c_str());
        job->advance();
    });
}

std::string Job::tag(size_t i) const {
    return "[" + std::to_string(i + 1) + "/" + std::to_string(items_.size()) + "] " + items_[i].name;
}

void Job::item_failed(size_t i, const std::string &error) {
    g_warning("Downloads %s: failed: %s", tag(i).c_str(), error.c_str());
    failed_.push_back(items_[i].name + ": " + error);
    failed_urls_.push_back(nexus::mod_page_url(items_[i].mod_id, items_[i].file_id));
    append_log("  failed: " + error);
    advance();
}

void Job::advance(bool pause) {
    ++index_;
    if (!pause || cancelled_ || index_ >= items_.size()) {
        next();
        return;
    }
    set(State::Downloading, "Next file in " + std::to_string(kPauseSeconds) + " s…",
        double(index_) / double(items_.size()));
    pause_ = g_timeout_add_seconds(kPauseSeconds, [](gpointer d) -> gboolean {
        auto *job = static_cast<Job *>(d);
        job->pause_ = 0;
        job->next();
        return G_SOURCE_REMOVE;
    }, this);
}

} // namespace gtkdrop::downloads
