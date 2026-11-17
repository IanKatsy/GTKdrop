#include "nexus.h"
#include "build_info.h"

#include "json_util.h"

#include <libsecret/secret.h>
#include <libsoup/soup.h>

#include <cstdlib>

namespace gtkdrop::nexus {

namespace {

constexpr char kApiBase[] = "https://api.nexusmods.com/v1";
constexpr char kGraphQL[] = "https://api.nexusmods.com/v2/graphql";

const SecretSchema *key_schema() {
    static const SecretSchema schema = {
        "dev.gtkdrop.GTKdrop.NexusApiKey",
        SECRET_SCHEMA_NONE,
        {{"service", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    };
    return &schema;
}

constexpr char kService[] = "nexusmods";

SoupSession *session() {
    static SoupSession *s = [] {
        SoupSession *session = soup_session_new();
        soup_session_set_user_agent(session, "GTKdrop/" GTKDROP_VERSION " (Linux)");
        soup_session_set_timeout(session, 30);
        return session;
    }();
    return s;
}

// Identifies GTKdrop to Nexus on every request, as their API policy asks.
void add_identity_headers(SoupMessage *msg, const std::string &key);

SoupMessage *api_request(const char *method, const std::string &path, const std::string &key) {
    SoupMessage *msg = soup_message_new(method, (std::string(kApiBase) + path).c_str());
    add_identity_headers(msg, key);
    return msg;
}

void add_identity_headers(SoupMessage *msg, const std::string &key) {
    SoupMessageHeaders *h = soup_message_get_request_headers(msg);
    if (!key.empty())
        soup_message_headers_append(h, "apikey", key.c_str());
    soup_message_headers_append(h, "Application-Name", "GTKdrop");
    soup_message_headers_append(h, "Application-Version", GTKDROP_VERSION);
    soup_message_headers_append(h, "Accept", "application/json");
}

using GraphQLDone = std::function<void(std::optional<json> data, std::string error)>;

// POSTs one GraphQL query. `data` is the reply's "data" object; any "errors" fail the call.
void graphql(const std::string &key, const std::string &query, const json &variables, std::weak_ptr<bool> alive,
             GraphQLDone done) {
    SoupMessage *msg = soup_message_new("POST", kGraphQL);
    add_identity_headers(msg, key);
    const std::string body = json{{"query", query}, {"variables", variables}}.dump();
    GBytes *bytes = g_bytes_new(body.data(), body.size());
    soup_message_set_request_body_from_bytes(msg, "application/json", bytes);
    g_bytes_unref(bytes);

    struct Req {
        std::weak_ptr<bool> alive;
        GraphQLDone done;
        SoupMessage *msg;
    };
    auto *req = new Req{std::move(alive), std::move(done), msg};
    soup_session_send_and_read_async(session(), msg, G_PRIORITY_DEFAULT, nullptr,
                                     [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *req = static_cast<Req *>(data);
        GError *error = nullptr;
        GBytes *reply = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &error);
        const guint code = soup_message_get_status(req->msg);
        std::optional<json> result;
        std::string message;
        if (error) {
            message = error->message;
            g_error_free(error);
        } else {
            gsize size = 0;
            const auto *p = static_cast<const char *>(g_bytes_get_data(reply, &size));
            auto doc = parse_lenient_json(std::string_view(p, size));
            if (code == 401 || code == 403) {
                message = "Nexus Mods rejected the API key";
            } else if (!doc || !doc->is_object()) {
                message = "Unexpected reply from Nexus Mods (HTTP " + std::to_string(code) + ")";
            } else if (auto errors = doc->find("errors"); errors != doc->end() && errors->is_array() && !errors->empty()) {
                message = (*errors)[0].value("message", "Nexus Mods returned an error");
            } else if (auto d = doc->find("data"); d != doc->end() && d->is_object()) {
                result = *d;
            } else {
                message = "Nexus Mods returned no data";
            }
        }
        if (reply)
            g_bytes_unref(reply);
        g_object_unref(req->msg);
        if (!message.empty())
            g_warning("Nexus Mods request failed: %s", message.c_str());
        if (!req->alive.expired())
            req->done(std::move(result), message);
        delete req;
    }, req);
}

// Keeps a callback alive across an async call, and drops it if the owner went away meanwhile.
template<typename F>
struct Pending {
    std::weak_ptr<bool> alive;
    F callback;
    SoupMessage *msg = nullptr;
};

} // namespace

Account::Account() = default;
Account::~Account() { *alive_ = false; }

void Account::set_status(Status status) {
    status_ = status;
    if (on_changed)
        on_changed();
}

std::string Account::status_text() const {
    switch (status_) {
        case Status::Unknown: return "Nexus Mods: …";
        case Status::NoKey: return "Nexus Mods: Disconnected";
        case Status::Checking: return "Nexus Mods: Connecting…";
        case Status::Invalid: return "Nexus Mods: Key Rejected";
        case Status::Offline: return "Nexus Mods: Offline";
        case Status::Connected:
            return "Nexus Mods: " + (user_ ? user_->name : std::string("Connected")) +
                   (user_ && user_->premium ? " (Premium)" : "");
    }
    return {};
}

void Account::validate(const std::string &key, std::function<void(std::optional<User>, Status, std::string)> done) {
    using Cb = std::function<void(std::optional<User>, Status, std::string)>;
    auto *pending = new Pending<Cb>{alive_, std::move(done), api_request("GET", "/users/validate.json", key)};
    soup_session_send_and_read_async(session(), pending->msg, G_PRIORITY_DEFAULT, nullptr,
                                     [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *p = static_cast<Pending<Cb> *>(data);
        GError *error = nullptr;
        GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &error);
        const guint code = soup_message_get_status(p->msg);

        std::optional<User> user;
        Status status = Status::Offline;
        std::string message;
        if (error) {
            message = error->message;
            g_error_free(error);
        } else if (code == 401 || code == 403) {
            status = Status::Invalid;
            message = "Nexus Mods rejected the key";
        } else if (code != 200) {
            message = "Nexus Mods answered " + std::to_string(code);
        } else {
            gsize size = 0;
            const auto *data_ptr = static_cast<const char *>(g_bytes_get_data(body, &size));
            // The reply also carries the account's email and the key itself; only what's shown is kept.
            if (auto doc = parse_lenient_json(std::string_view(data_ptr, size)); doc && doc->is_object()) {
                User u;
                u.name = doc->value("name", "");
                u.user_id = doc->value("user_id", 0);
                u.premium = doc->value("is_premium", false);
                u.supporter = doc->value("is_supporter", false);
                user = u;
                status = Status::Connected;
            } else {
                message = "Unexpected reply from Nexus Mods";
            }
        }
        if (body)
            g_bytes_unref(body);
        g_object_unref(p->msg);
        if (!p->alive.expired())
            p->callback(user, status, message);
        delete p;
    }, pending);
}

void Account::load() {
    using Cb = std::function<void(std::optional<std::string>)>;
    auto *pending = new Pending<Cb>{alive_, [this](std::optional<std::string> key) {
        if (!key) {
            key_.clear();
            user_.reset();
            set_status(Status::NoKey);
            return;
        }
        key_ = *key;
        set_status(Status::Checking);
        validate(key_, [this](std::optional<User> user, Status status, std::string message) {
            user_ = std::move(user);
            error_ = std::move(message);
            set_status(status);
        });
    }};
    secret_password_lookup(key_schema(), nullptr, [](GObject *, GAsyncResult *res, gpointer data) {
        auto *p = static_cast<Pending<Cb> *>(data);
        GError *error = nullptr;
        char *password = secret_password_lookup_finish(res, &error);
        if (error) {
            g_warning("Could not read the Nexus key from the keyring: %s", error->message);
            g_error_free(error);
        }
        std::optional<std::string> key;
        if (password) {
            key = password;
            secret_password_free(password);
        }
        if (!p->alive.expired())
            p->callback(std::move(key));
        delete p;
    }, pending, "service", kService, nullptr);
}

void Account::set_key(const std::string &key, std::function<void(bool, const std::string &)> done) {
    const Status previous = status_;
    set_status(Status::Checking);
    validate(key, [this, key, previous, done = std::move(done)](std::optional<User> user, Status status,
                                                                 std::string message) {
        if (status != Status::Connected) {
            set_status(previous); // a rejected key leaves the previous login as it was
            done(false, message);
            return;
        }
        using Cb = std::function<void(bool, std::string)>;
        auto *pending = new Pending<Cb>{alive_, [this, key, user, previous, done](bool stored, std::string error) {
            if (!stored) {
                set_status(previous);
                done(false, "Couldn't save the key in the system keyring: " + error);
                return;
            }
            key_ = key;
            user_ = user;
            set_status(Status::Connected);
            done(true, "Connected as " + user->name);
        }};
        secret_password_store(key_schema(), SECRET_COLLECTION_DEFAULT, "GTKdrop: Nexus Mods API key", key.c_str(),
                              nullptr, [](GObject *, GAsyncResult *res, gpointer data) {
            auto *p = static_cast<Pending<Cb> *>(data);
            GError *error = nullptr;
            const bool ok = secret_password_store_finish(res, &error);
            std::string message = error ? error->message : "";
            if (error)
                g_error_free(error);
            if (!p->alive.expired())
                p->callback(ok, message);
            delete p;
        }, pending, "service", kService, nullptr);
    });
}

void Account::remove_key(std::function<void()> done) {
    using Cb = std::function<void()>;
    auto *pending = new Pending<Cb>{alive_, [this, done = std::move(done)] {
        key_.clear();
        user_.reset();
        set_status(Status::NoKey);
        if (done)
            done();
    }};
    secret_password_clear(key_schema(), nullptr, [](GObject *, GAsyncResult *res, gpointer data) {
        auto *p = static_cast<Pending<Cb> *>(data);
        GError *error = nullptr;
        secret_password_clear_finish(res, &error);
        if (error) {
            g_warning("Could not remove the Nexus key from the keyring: %s", error->message);
            g_error_free(error);
        }
        if (!p->alive.expired())
            p->callback();
        delete p;
    }, pending, "service", kService, nullptr);
}

std::string mod_page_url(int mod_id, int file_id) {
    std::string url = "https://www.nexusmods.com/" + std::string(kGameDomain) + "/mods/" + std::to_string(mod_id) +
                      "?tab=files";
    if (file_id > 0)
        url += "&file_id=" + std::to_string(file_id);
    return url;
}

void fetch_latest_versions(const Account &account, const std::vector<int> &mod_ids,
                           std::function<void(std::optional<std::vector<LatestVersion>>, const std::string &)> done) {
    constexpr size_t kBatch = 50;
    static const std::string query =
            "query($ids:[CompositeDomainWithIdInput!]!){legacyModsByDomain(ids:$ids){nodes{modId name version}}}";

    struct State {
        std::vector<LatestVersion> results;
        size_t pending = 0;
        std::string error;
        std::function<void(std::optional<std::vector<LatestVersion>>, const std::string &)> done;
    };
    auto state = std::make_shared<State>();
    state->done = std::move(done);
    if (mod_ids.empty()) {
        state->done(std::vector<LatestVersion>{}, {});
        return;
    }

    for (size_t start = 0; start < mod_ids.size(); start += kBatch) {
        json ids = json::array();
        for (size_t i = start; i < std::min(mod_ids.size(), start + kBatch); ++i)
            ids.push_back({{"gameDomain", kGameDomain}, {"modId", mod_ids[i]}});
        ++state->pending;
        graphql(account.key(), query, {{"ids", ids}}, account.alive_token(),
                [state](std::optional<json> data, std::string error) {
            if (!data) {
                state->error = error;
            } else if (auto nodes = (*data)["legacyModsByDomain"]["nodes"]; nodes.is_array()) {
                for (const auto &n: nodes)
                    if (n.is_object() && n["modId"].is_number_integer())
                        state->results.push_back({n["modId"].get<int>(),
                                                  n.value("name", ""),
                                                  n["version"].is_string() ? n["version"].get<std::string>() : ""});
            }
            if (--state->pending == 0) {
                if (state->error.empty())
                    state->done(std::move(state->results), {});
                else
                    state->done(std::nullopt, state->error);
            }
        });
    }
}

void fetch_collection(const Account &account, const std::string &slug, std::optional<int> revision,
                      std::function<void(std::optional<Collection>, const std::string &)> done) {
    static const std::string query =
            "query($slug:String,$domainName:String,$revision:Int){"
            "collectionRevision(slug:$slug,domainName:$domainName,revision:$revision,viewAdultContent:true){"
            "revisionNumber collection{name slug} "
            "modFiles{fileId optional file{name version modId mod{name modId}}}}}";
    json variables = {{"slug", slug}, {"domainName", kGameDomain},
                      {"revision", revision ? json(*revision) : json(nullptr)}};
    graphql(account.key(), query, variables, account.alive_token(),
            [done = std::move(done), slug](std::optional<json> data, std::string error) {
        if (!data) {
            done(std::nullopt, error);
            return;
        }
        const json &rev = (*data)["collectionRevision"];
        if (!rev.is_object()) {
            done(std::nullopt, "Nexus Mods has no collection “" + slug + "”");
            return;
        }
        Collection c;
        c.slug = slug;
        c.revision = rev.value("revisionNumber", 0);
        if (rev["collection"].is_object())
            c.name = rev["collection"].value("name", slug);
        if (rev["modFiles"].is_array()) {
            for (const auto &mf: rev["modFiles"]) {
                const json &file = mf["file"];
                if (!file.is_object())
                    continue;
                CollectionFile f;
                f.file_id = mf.value("fileId", 0);
                f.optional = mf.value("optional", false);
                f.file_name = file["name"].is_string() ? file["name"].get<std::string>() : "";
                f.version = file["version"].is_string() ? file["version"].get<std::string>() : "";
                f.mod_id = file.value("modId", 0);
                if (file["mod"].is_object() && file["mod"]["name"].is_string())
                    f.mod_name = file["mod"]["name"].get<std::string>();
                if (f.mod_name.empty())
                    f.mod_name = f.file_name;
                c.files.push_back(std::move(f));
            }
        }
        done(std::move(c), {});
    });
}

std::optional<NxmLink> parse_nxm(std::string_view uri) {
    constexpr std::string_view scheme = "nxm://";
    if (uri.size() < scheme.size() || !iequals(uri.substr(0, scheme.size()), scheme))
        return std::nullopt;
    uri.remove_prefix(scheme.size());
    std::string_view query;
    if (auto q = uri.find('?'); q != std::string_view::npos) {
        query = uri.substr(q + 1);
        uri = uri.substr(0, q);
    }

    std::vector<std::string> parts;
    for (size_t start = 0; start <= uri.size();) {
        const size_t slash = uri.find('/', start);
        const auto part = uri.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!part.empty())
            parts.emplace_back(part);
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    auto number = [](const std::string &s) -> std::optional<int> {
        if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
            return std::nullopt;
        return std::atoi(s.c_str());
    };

    NxmLink link{};
    if (parts.size() >= 3 && iequals(parts[1], "collections")) {
        link.kind = NxmLink::Kind::Collection;
        link.domain = to_lower(parts[0]);
        link.slug = parts[2];
        if (parts.size() >= 5 && iequals(parts[3], "revisions"))
            link.revision = number(parts[4]);
        return link;
    }
    if (parts.size() >= 5 && iequals(parts[1], "mods") && iequals(parts[3], "files")) {
        auto mod = number(parts[2]), file = number(parts[4]);
        if (!mod || !file)
            return std::nullopt;
        link.kind = NxmLink::Kind::ModFile;
        link.domain = to_lower(parts[0]);
        link.mod_id = *mod;
        link.file_id = *file;
        for (size_t start = 0; start < query.size();) {
            const size_t amp = query.find('&', start);
            const auto pair = query.substr(start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
            const size_t eq = pair.find('=');
            if (eq != std::string_view::npos) {
                char *value = g_uri_unescape_string(std::string(pair.substr(eq + 1)).c_str(), nullptr);
                const auto name = pair.substr(0, eq);
                if (name == "key")
                    link.key = value ? value : "";
                else if (name == "expires")
                    link.expires = value ? value : "";
                g_free(value);
            }
            if (amp == std::string_view::npos)
                break;
            start = amp + 1;
        }
        return link;
    }
    return std::nullopt;
}

namespace {

// GET on API v1; `done` gets the parsed reply or an error. Dropped if the account went away.
void api_get(const Account &account, const std::string &path,
             std::function<void(std::optional<json>, const std::string &)> done) {
    using Cb = std::function<void(std::optional<json>, const std::string &)>;
    auto *pending = new Pending<Cb>{account.alive_token(), std::move(done), api_request("GET", path, account.key())};
    soup_session_send_and_read_async(session(), pending->msg, G_PRIORITY_DEFAULT, nullptr,
                                     [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *p = static_cast<Pending<Cb> *>(data);
        GError *error = nullptr;
        GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &error);
        const guint code = soup_message_get_status(p->msg);
        std::optional<json> doc;
        std::string message;
        if (error) {
            message = error->message;
            g_error_free(error);
        } else {
            gsize size = 0;
            const auto *bytes = static_cast<const char *>(g_bytes_get_data(body, &size));
            doc = parse_lenient_json(std::string_view(bytes, size));
            if (code != 200) {
                // Nexus explains refusals in {"message": "..."} (e.g. Premium needed, expired key).
                message = doc && doc->is_object() && (*doc)["message"].is_string()
                                  ? (*doc)["message"].get<std::string>()
                                  : "Nexus Mods answered " + std::to_string(code);
                doc.reset();
            } else if (!doc) {
                message = "Unexpected reply from Nexus Mods";
            }
        }
        if (body)
            g_bytes_unref(body);
        g_object_unref(p->msg);
        if (!message.empty())
            g_warning("Nexus Mods request failed: %s", message.c_str());
        if (!p->alive.expired())
            p->callback(std::move(doc), message);
        delete p;
    }, pending);
}

} // namespace

void fetch_main_file(const Account &account, const std::string &domain, int mod_id,
                     std::function<void(std::optional<int>, const std::string &)> done) {
    api_get(account, "/games/" + domain + "/mods/" + std::to_string(mod_id) + "/files.json?category=main",
            [done = std::move(done)](std::optional<json> doc, const std::string &error) {
        if (!doc) {
            done(std::nullopt, error);
            return;
        }
        // Newest upload wins; a file marked primary breaks ties.
        std::optional<int> best;
        long long best_time = -1;
        bool best_primary = false;
        if (auto files = doc->find("files"); files != doc->end() && files->is_array())
            for (const auto &f: *files) {
                if (!f.is_object() || !f["file_id"].is_number_integer())
                    continue;
                const long long t = f.value("uploaded_timestamp", 0LL);
                const bool primary = f.value("is_primary", false);
                if (t > best_time || (t == best_time && primary && !best_primary)) {
                    best = f["file_id"].get<int>();
                    best_time = t;
                    best_primary = primary;
                }
            }
        done(best, best ? std::string() : "the mod has no main file");
    });
}

void fetch_file_name(const Account &account, const std::string &domain, int mod_id, int file_id,
                     std::function<void(std::optional<std::string>, const std::string &)> done) {
    api_get(account, "/games/" + domain + "/mods/" + std::to_string(mod_id) + "/files/" + std::to_string(file_id) + ".json",
            [done = std::move(done)](std::optional<json> doc, const std::string &error) {
        if (doc && doc->is_object() && (*doc)["file_name"].is_string() && !(*doc)["file_name"].get<std::string>().empty())
            done((*doc)["file_name"].get<std::string>(), {});
        else
            done(std::nullopt, error.empty() ? "Nexus Mods didn't give the file's name" : error);
    });
}

void fetch_download_links(const Account &account, const std::string &domain, int mod_id, int file_id,
                          const std::string &nxm_key, const std::string &nxm_expires,
                          std::function<void(std::vector<std::string>, const std::string &)> done) {
    std::string path = "/games/" + domain + "/mods/" + std::to_string(mod_id) + "/files/" + std::to_string(file_id) +
                       "/download_link.json";
    if (!nxm_key.empty()) {
        char *key = g_uri_escape_string(nxm_key.c_str(), nullptr, FALSE);
        char *expires = g_uri_escape_string(nxm_expires.c_str(), nullptr, FALSE);
        path += std::string("?key=") + key + "&expires=" + expires;
        g_free(key);
        g_free(expires);
    }
    api_get(account, path, [done = std::move(done)](std::optional<json> doc, const std::string &error) {
        std::vector<std::string> urls;
        if (doc && doc->is_array())
            for (const auto &mirror: *doc)
                if (mirror.is_object() && mirror["URI"].is_string())
                    urls.push_back(mirror["URI"].get<std::string>());
        done(urls, urls.empty() && error.empty() ? "Nexus Mods returned no download link" : error);
    });
}

} // namespace gtkdrop::nexus
