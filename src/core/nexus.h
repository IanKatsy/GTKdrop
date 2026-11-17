#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gtkdrop::nexus {

constexpr char kApiKeyPage[] = "https://www.nexusmods.com/users/myaccount?tab=api";

struct User {
    std::string name;
    int user_id = 0;
    bool premium = false;
    bool supporter = false;
};

// The user's Nexus Mods login: a personal API key kept in the system keyring (libsecret / GNOME Keyring,
// KWallet through the Secret Service), never in GTKdrop's own files. Callbacks run on the GLib main loop and
// are dropped if the account is destroyed first.
class Account {
public:
    enum class Status {
        Unknown,  // keyring not read yet
        NoKey,    // no key stored
        Checking, // validating with Nexus
        Connected,
        Invalid,  // Nexus rejected the key
        Offline,  // couldn't reach Nexus; the key is kept
    };

    Account();
    ~Account();
    Account(const Account &) = delete;
    Account &operator=(const Account &) = delete;

    // Reads the key from the keyring and validates it.
    void load();
    // Validates `key` with Nexus and stores it in the keyring only if Nexus accepts it.
    void set_key(const std::string &key, std::function<void(bool ok, const std::string &message)> done);
    void remove_key(std::function<void()> done = {});

    Status status() const { return status_; }
    const std::optional<User> &user() const { return user_; }
    const std::string &key() const { return key_; } // for API requests; never log it
    std::string status_text() const;               // "Connected as X (Premium)", "Disconnected", …

    std::function<void()> on_changed; // status or user changed
    std::weak_ptr<bool> alive_token() const { return alive_; }

private:
    void set_status(Status status);
    void validate(const std::string &key, std::function<void(std::optional<User>, Status, std::string)> done);

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    Status status_ = Status::Unknown;
    std::optional<User> user_;
    std::string key_;
    std::string error_;
};

constexpr char kGameDomain[] = "stardewvalley";
constexpr int kSmapiModId = 2400; // collections list SMAPI as a mod; it's never installed into a mods folder

// https://www.nexusmods.com/stardewvalley/mods/<id>?tab=files (&file_id=<file> to point at one file)
std::string mod_page_url(int mod_id, int file_id = 0);

// ---------------------------------------------------------------------------------------------------------------
// API calls (GraphQL v2). The key goes along with every call; callbacks are dropped if the account is gone.
// ---------------------------------------------------------------------------------------------------------------
struct LatestVersion {
    int mod_id = 0;
    std::string name;
    std::string version;
};
// Latest version of each Nexus mod, looked up in batches. Mods Nexus doesn't know are left out.
void fetch_latest_versions(const Account &account, const std::vector<int> &mod_ids,
                           std::function<void(std::optional<std::vector<LatestVersion>>, const std::string &error)> done);

struct CollectionFile {
    int mod_id = 0;
    int file_id = 0;
    std::string mod_name;
    std::string file_name;
    std::string version;
    bool optional = false;
};
struct Collection {
    std::string name;
    std::string slug;
    int revision = 0;
    std::vector<CollectionFile> files;
};
// A collection revision's file list; no revision means the latest published one.
void fetch_collection(const Account &account, const std::string &slug, std::optional<int> revision,
                      std::function<void(std::optional<Collection>, const std::string &error)> done);

// ---------------------------------------------------------------------------------------------------------------
// nxm:// links ("Mod Manager Download" on the Nexus site)
// ---------------------------------------------------------------------------------------------------------------
struct NxmLink {
    enum class Kind { Collection, ModFile } kind;
    std::string domain;
    std::string slug;       // collection
    std::optional<int> revision;
    int mod_id = 0;         // mod file
    int file_id = 0;
    // Mod file links from "Mod Manager Download": a one-time key that lets the API hand out the file even to
    // accounts without Premium.
    std::string key;
    std::string expires;
};
// nxm://stardewvalley/collections/<slug>/revisions/<n> or nxm://stardewvalley/mods/<id>/files/<file>?…
std::optional<NxmLink> parse_nxm(std::string_view uri);

// ---------------------------------------------------------------------------------------------------------------
// Downloads (API v1)
// ---------------------------------------------------------------------------------------------------------------
// The newest main file of a mod (what "Download" on the mod page gives), for update lists without a file ID.
void fetch_main_file(const Account &account, const std::string &domain, int mod_id,
                     std::function<void(std::optional<int> file_id, const std::string &error)> done);

// A file's archive name as Nexus stores it (e.g. "Animated Fish-5735-1-4-0-1735766904.zip").
void fetch_file_name(const Account &account, const std::string &domain, int mod_id, int file_id,
                     std::function<void(std::optional<std::string> file_name, const std::string &error)> done);

// Download URLs (CDN mirrors) for a file. Without Premium Nexus needs the key + expires from the file's nxm link.
void fetch_download_links(const Account &account, const std::string &domain, int mod_id, int file_id,
                          const std::string &nxm_key, const std::string &nxm_expires,
                          std::function<void(std::vector<std::string> urls, const std::string &error)> done);

} // namespace gtkdrop::nexus
