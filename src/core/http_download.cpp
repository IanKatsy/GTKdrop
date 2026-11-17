#include "http_download.h"
#include "build_info.h"

#include <libsoup/soup.h>

#include <array>
#include <cstring>
#include <memory>

namespace gtkdrop {

namespace fs = std::filesystem;

namespace {

SoupSession *download_session() {
    static SoupSession *s = [] {
        SoupSession *session = soup_session_new();
        soup_session_set_user_agent(session, "GTKdrop/" GTKDROP_VERSION " (Linux)");
        soup_session_set_timeout(session, 60); // per read: big files take as long as they take
        return session;
    }();
    return s;
}

std::string safe_name(std::string name) {
    for (char &c: name)
        if (c == '/' || c == '\0')
            c = '_';
    return name.empty() || name == "." || name == ".." ? "download" : name;
}

std::string file_name_from(const std::string &url) {
    GUri *uri = g_uri_parse(url.c_str(), G_URI_FLAGS_ENCODED, nullptr);
    std::string name;
    if (uri) {
        const char *path = g_uri_get_path(uri);
        if (const char *slash = path ? std::strrchr(path, '/') : nullptr) {
            char *decoded = g_uri_unescape_string(slash + 1, nullptr);
            name = decoded ? decoded : "";
            g_free(decoded);
        }
        g_uri_unref(uri);
    }
    for (char &c: name)
        if (c == '/' || c == '\0')
            c = '_';
    return name.empty() || name == "." || name == ".." ? "download" : name;
}

struct Transfer {
    SoupMessage *msg = nullptr;
    GInputStream *in = nullptr;
    GOutputStream *out = nullptr;
    GCancellable *cancellable = nullptr;
    fs::path part, final_path;
    goffset received = 0, total = 0;
    bool named = false;
    std::array<char, 256 * 1024> buffer{};
    std::function<void(goffset, goffset)> progress;
    std::function<void(fs::path, const std::string &)> done;

    void finish(const std::string &error) {
        if (out) {
            g_output_stream_close(out, nullptr, nullptr);
            g_object_unref(out);
        }
        if (in)
            g_object_unref(in);
        g_object_unref(msg);
        if (cancellable)
            g_object_unref(cancellable);
        std::error_code ec;
        if (error.empty())
            fs::rename(part, final_path, ec);
        if (!error.empty() || ec) {
            fs::remove(part, ec);
            done({}, error.empty() ? "couldn't save " + final_path.filename().string() : error);
        } else {
            done(final_path, {});
        }
        delete this;
    }

    void read_next() {
        g_input_stream_read_async(in, buffer.data(), buffer.size(), G_PRIORITY_DEFAULT, cancellable,
                                  [](GObject *src, GAsyncResult *res, gpointer d) {
            auto *t = static_cast<Transfer *>(d);
            GError *error = nullptr;
            const gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &error);
            if (n < 0) {
                std::string message = error->message;
                g_error_free(error);
                t->finish(message);
                return;
            }
            if (n == 0) {
                t->finish({});
                return;
            }
            gsize written = 0;
            if (!g_output_stream_write_all(t->out, t->buffer.data(), gsize(n), &written, nullptr, &error)) {
                std::string message = error->message;
                g_error_free(error);
                t->finish(message);
                return;
            }
            t->received += n;
            if (t->progress)
                t->progress(t->received, t->total);
            t->read_next();
        }, this);
    }
};

} // namespace

void download_file(const std::string &url, const fs::path &dir, const std::string &name, GCancellable *cancellable,
                   std::function<void(goffset, goffset)> progress,
                   std::function<void(fs::path, const std::string &)> done) {
    const bool named = !name.empty();
    const fs::path final_path = dir / (named ? safe_name(name) : file_name_from(url));
    std::error_code ec;
    if (fs::exists(final_path, ec)) {
        done(final_path, {});
        return;
    }
    fs::create_directories(dir, ec);

    auto *t = new Transfer;
    t->msg = soup_message_new("GET", url.c_str());
    if (!t->msg) {
        delete t;
        done({}, "invalid download URL");
        return;
    }
    t->cancellable = cancellable ? G_CANCELLABLE(g_object_ref(cancellable)) : nullptr;
    t->part = fs::path(final_path.string() + ".part");
    t->final_path = final_path;
    t->named = named;
    t->progress = std::move(progress);
    t->done = std::move(done);
    soup_session_send_async(download_session(), t->msg, G_PRIORITY_DEFAULT, t->cancellable,
                            [](GObject *src, GAsyncResult *res, gpointer d) {
        auto *t = static_cast<Transfer *>(d);
        GError *error = nullptr;
        t->in = soup_session_send_finish(SOUP_SESSION(src), res, &error);
        if (!t->in) {
            std::string message = error->message;
            g_error_free(error);
            t->finish(message);
            return;
        }
        const guint status = soup_message_get_status(t->msg);
        if (status != 200) {
            t->finish("the download server answered " + std::to_string(status));
            return;
        }
        SoupMessageHeaders *headers = soup_message_get_response_headers(t->msg);
        t->total = soup_message_headers_get_content_length(headers);
        // No name given: the server's own file name beats a URL ending in an ID.
        GHashTable *params = nullptr;
        if (!t->named && soup_message_headers_get_content_disposition(headers, nullptr, &params)) {
            if (const char *fn = static_cast<const char *>(g_hash_table_lookup(params, "filename")); fn && *fn) {
                t->final_path = t->final_path.parent_path() / safe_name(fn);
                t->part = fs::path(t->final_path.string() + ".part");
            }
            g_hash_table_destroy(params);
        }
        GFile *file = g_file_new_for_path(t->part.c_str());
        t->out = G_OUTPUT_STREAM(g_file_replace(file, nullptr, FALSE, G_FILE_CREATE_NONE, nullptr, &error));
        g_object_unref(file);
        if (!t->out) {
            std::string message = error->message;
            g_error_free(error);
            t->finish(message);
            return;
        }
        t->read_next();
    }, t);
}

} // namespace gtkdrop
