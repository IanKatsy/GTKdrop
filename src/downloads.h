#pragma once

// Downloads every mod in a download list (payload) one at a time, through the Nexus API:
//  - Premium accounts: the API hands out download links directly.
//  - Others: GTKdrop's Nexus window (NexusBrowser) clicks "Slow download" on the file's page and the nxm:// link it
//    produces carries a one-time key for the API.
// One job runs at a time (current()); it keeps running when the window that started it closes.

#include "core/nexus.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gtkdrop::ui {
class NexusBrowser;
}

namespace gtkdrop::downloads {

namespace fs = std::filesystem;

constexpr int kPauseSeconds = 5; // between files: one at a time, at a human pace

fs::path downloads_dir(); // ~/.config/gtkdrop/downloads
// Where a list's archives download to, until they're installed: downloads_dir()/<payload name>.
fs::path downloads_dir_for(const fs::path &payload);
// Moves the download folders older versions kept in ~/Downloads/GTKdrop here, deleting installed archives.
void move_old_downloads();

// Which Nexus file each archive in a download folder is (.nexus-files: "file_id<TAB>archive<TAB>mod_id"), so the
// install can record it in the library. .installed lists the archives installed from the folder; since the
// library exists, only lists from before it rely on that.
struct RecordedFile {
    int file_id = 0;
    int mod_id = 0; // 0 in records from before it was kept
    std::string archive;
};
void record_file(const fs::path &dir, int file_id, const std::string &archive, int mod_id);
std::map<int, std::string> recorded_files(const fs::path &dir); // file_id -> archive
std::map<std::string, RecordedFile> recorded_archives(const fs::path &dir); // archive -> its file
std::vector<std::string> installed_archives(const fs::path &dir);

// A download list's mod pages, in order.
std::vector<std::string> payload_urls(const fs::path &payload);
// The Nexus mod a download list names for a file, or 0.
int payload_mod_for_file(const fs::path &payload, int file_id);
// The download list (in payloads_dir()) a file from the browser belongs to: one with this exact file, else one with
// this mod (the user picked a file on its Files tab). `list_file_id` gets the file the list names for it when that
// is unambiguous (the list has one file of the mod), else 0. Empty if no list has the mod.
fs::path payload_with_file(int mod_id, int file_id, int *list_file_id = nullptr);
// Opens each mod's Files tab in the default browser, a moment apart (browsers drop tabs opened all at once). A
// file_id in the URL is dropped: Nexus hides "Mod Manager Download" on a single-file view.
void open_in_browser(GtkWindow *parent, const std::vector<std::string> &urls);

class Job {
public:
    enum class State { Downloading, Waiting, Done, Cancelled, Failed };

    static std::shared_ptr<Job> &current();
    // nullptr when the list has no mods it can download.
    static std::shared_ptr<Job> start(const fs::path &payload, const nexus::Account &account, GtkWindow *parent);
    static inline std::function<void(const Job &)> on_finished;
    // Whether a Nexus file is installed where this list's mods go (the library); such files are skipped.
    static inline std::function<bool(const fs::path &payload, int file_id)> is_installed;

    const fs::path &payload() const { return payload_; }
    const fs::path &out_dir() const { return out_dir_; }
    State state() const { return state_; }
    bool active() const { return state_ == State::Downloading || state_ == State::Waiting; }
    const std::string &status() const { return status_; }
    double fraction() const { return fraction_; } // < 0: unknown
    size_t total() const { return items_.size(); }
    int succeeded() const { return succeeded_; }
    const std::vector<std::string> &failed() const { return failed_; }
    const std::vector<std::string> &failed_urls() const { return failed_urls_; } // their file pages

    void cancel();

    int listen(std::function<void()> fn);
    void unlisten(int id);
    const std::vector<std::string> &log() const { return log_; }
    void append_log(std::string line);
    int listen_log(std::function<void(const std::string &)> fn);
    void unlisten_log(int id);

    ~Job();

private:
    struct Item {
        std::string name;
        int mod_id = 0;
        int file_id = 0; // 0: the mod's newest main file (update lists)
        std::string file_name; // the archive's name on Nexus, once known
    };

    Job() = default;
    void next();
    void check_existing(size_t index);
    void get_links(size_t index);
    void download(size_t index, const std::string &url);
    void item_failed(size_t index, const std::string &error);
    std::string tag(size_t index) const; // "[3/79] Animated Fish" for the log file
    void advance(bool pause = true); // a pause before the next file, unless nothing was asked of Nexus' site
    void set(State state, std::string status, double fraction = -1);
    void finish(State state, std::string status);
    std::weak_ptr<Job> self() { return weak_self_; }

    fs::path payload_, out_dir_;
    std::string domain_ = nexus::kGameDomain;
    std::vector<Item> items_;
    size_t index_ = 0;
    int succeeded_ = 0;
    std::vector<std::string> failed_, failed_urls_;
    State state_ = State::Downloading;
    std::string status_;
    double fraction_ = -1;
    bool cancelled_ = false;
    const nexus::Account *account_ = nullptr;
    std::weak_ptr<bool> account_alive_;
    GtkWindow *parent_ = nullptr;
    std::unique_ptr<ui::NexusBrowser> browser_;
    GCancellable *cancellable_ = g_cancellable_new();
    guint pause_ = 0;
    std::weak_ptr<Job> weak_self_;
    std::map<int, std::function<void()>> listeners_;
    std::map<int, std::function<void(const std::string &)>> log_listeners_;
    std::vector<std::string> log_;
    int next_listener_ = 0;
};

} // namespace gtkdrop::downloads
