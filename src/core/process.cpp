#include "process.h"

namespace gtkdrop {

GSubprocess *run_process(const std::vector<std::string> &argv, const ProcessOptions &options,
                         std::function<void(int, std::string)> done, std::string *error) {
    GSubprocessLauncher *launcher =
            g_subprocess_launcher_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE));
    for (const auto &[key, value]: options.env)
        g_subprocess_launcher_setenv(launcher, key.c_str(), value.c_str(), TRUE);
    if (!options.cwd.empty())
        g_subprocess_launcher_set_cwd(launcher, options.cwd.c_str());
    std::vector<const char *> args;
    for (const auto &a: argv)
        args.push_back(a.c_str());
    args.push_back(nullptr);
    GError *err = nullptr;
    GSubprocess *process = g_subprocess_launcher_spawnv(launcher, args.data(), &err);
    g_object_unref(launcher);
    if (!process) {
        if (error)
            *error = err->message;
        g_error_free(err);
        return nullptr;
    }
    if (!options.on_line) {
        auto *cb = new std::function<void(int, std::string)>(std::move(done));
        g_subprocess_communicate_utf8_async(process, nullptr, nullptr, [](GObject *src, GAsyncResult *res, gpointer d) {
            auto *cb = static_cast<std::function<void(int, std::string)> *>(d);
            auto *process = G_SUBPROCESS(src);
            char *out = nullptr;
            g_subprocess_communicate_utf8_finish(process, res, &out, nullptr, nullptr);
            const int code = g_subprocess_get_if_exited(process) ? g_subprocess_get_exit_status(process) : 130;
            std::string output = out ? out : "";
            g_free(out);
            (*cb)(code, std::move(output));
            delete cb;
        }, cb);
        return process;
    }

    // Streaming: read lines until EOF, then wait for the exit status.
    struct Stream {
        GSubprocess *process;
        GDataInputStream *in;
        std::string output;
        std::function<void(const std::string &)> on_line;
        std::function<void(int, std::string)> done;
        static void read(Stream *s) {
            g_data_input_stream_read_line_async(s->in, G_PRIORITY_DEFAULT, nullptr, [](GObject *, GAsyncResult *res, gpointer d) {
                auto *s = static_cast<Stream *>(d);
                gsize length = 0;
                char *line = g_data_input_stream_read_line_finish_utf8(s->in, res, &length, nullptr);
                if (!line) { // EOF or error
                    g_subprocess_wait_async(s->process, nullptr, [](GObject *src, GAsyncResult *res, gpointer d) {
                        auto *s = static_cast<Stream *>(d);
                        auto *process = G_SUBPROCESS(src);
                        g_subprocess_wait_finish(process, res, nullptr);
                        const int code = g_subprocess_get_if_exited(process) ? g_subprocess_get_exit_status(process) : 130;
                        s->done(code, std::move(s->output));
                        g_object_unref(s->in);
                        g_object_unref(s->process);
                        delete s;
                    }, s);
                    return;
                }
                std::string text = strip_ansi(line);
                g_free(line);
                // Progress bars redraw with '\r': show the last state of the line.
                if (auto cr = text.find_last_of('\r'); cr != std::string::npos && cr + 1 < text.size())
                    text = text.substr(cr + 1);
                while (!text.empty() && (text.back() == '\r' || text.back() == ' '))
                    text.pop_back();
                s->output += text + "\n";
                if (!text.empty())
                    s->on_line(text);
                read(s);
            }, s);
        }
    };
    auto *stream = new Stream{G_SUBPROCESS(g_object_ref(process)),
                              g_data_input_stream_new(g_subprocess_get_stdout_pipe(process)), {}, options.on_line,
                              std::move(done)};
    Stream::read(stream);
    return process;
}

std::string command_line(const std::vector<std::string> &argv) {
    std::string out = "$";
    for (const auto &a: argv) {
        char *quoted = a.find_first_of(" '\"$&;|<>()*?[]#~") == std::string::npos ? g_strdup(a.c_str())
                                                                                     : g_shell_quote(a.c_str());
        out += " ";
        out += quoted;
        g_free(quoted);
    }
    return out;
}

std::string strip_ansi(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() && !(text[i] >= '@' && text[i] <= '~'))
                ++i;
            continue;
        }
        out += text[i];
    }
    return out;
}

std::string last_line(const std::string &text) {
    const size_t end = text.find_last_not_of(" \r\n");
    if (end == std::string::npos)
        return {};
    const size_t nl = text.find_last_of("\r\n", end);
    const size_t start = nl == std::string::npos ? 0 : nl + 1;
    return text.substr(start, end - start + 1);
}

} // namespace gtkdrop
