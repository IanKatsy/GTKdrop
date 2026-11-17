#include "log.h"
#include "build_info.h"

#include <glib.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#ifdef GTKDROP_STACKTRACES
#include <stacktrace>
#endif

namespace gtkdrop::log {

namespace {

std::FILE *g_file = nullptr;
std::mutex g_mutex; // GLib may log from worker threads

const char *field(const GLogField *fields, gsize n, const char *key) {
    for (gsize i = 0; i < n; ++i)
        if (g_str_equal(fields[i].key, key) && fields[i].length < 0)
            return static_cast<const char *>(fields[i].value);
    return nullptr;
}

const char *level_name(GLogLevelFlags level) {
    if (level & G_LOG_LEVEL_ERROR) return "ERROR";
    if (level & G_LOG_LEVEL_CRITICAL) return "CRITICAL";
    if (level & G_LOG_LEVEL_WARNING) return "WARNING";
    if (level & G_LOG_LEVEL_MESSAGE) return "MESSAGE";
    if (level & G_LOG_LEVEL_INFO) return "INFO";
    return "DEBUG";
}

#ifdef GTKDROP_STACKTRACES
bool is_problem(GLogLevelFlags level) {
    return level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING);
}

std::string stack_trace() {
    std::string out = "  Stack trace:\n";
    int shown = 0;
    for (const auto &frame: std::stacktrace::current(2)) {
        const std::string what = frame.description();
        // The logging machinery itself says nothing about who logged.
        if (what.starts_with("g_log") || what.starts_with("g_return_if_fail") || what.contains("writer"))
            continue;
        out += "    " + (what.empty() ? std::string("??") : what);
        if (!frame.source_file().empty())
            out += "  at " + frame.source_file() + ":" + std::to_string(frame.source_line());
        out += "\n";
        if (++shown == 25)
            break;
    }
    return out;
}
#endif

GLogWriterOutput writer(GLogLevelFlags level, const GLogField *fields, gsize n, gpointer) {
    // Debug output follows G_MESSAGES_DEBUG, the same as on the terminal.
    if ((level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO)) && g_log_writer_default_would_drop(level, field(fields, n, "GLIB_DOMAIN")))
        return G_LOG_WRITER_HANDLED;

    const GLogWriterOutput result = g_log_writer_default(level, fields, n, nullptr);

    const char *message = field(fields, n, "MESSAGE");
    const char *domain = field(fields, n, "GLIB_DOMAIN");
    std::string trace;
#ifdef GTKDROP_STACKTRACES
    // The frame-timings warning is GTK-internal and harmless (popovers closing with the GPU renderer).
    if (is_problem(level) && !(message && std::strstr(message, "gdk_frame_timings_presented"))) {
        trace = stack_trace();
        std::fputs(trace.c_str(), stderr);
    }
#endif

    std::lock_guard lock(g_mutex);
    if (g_file) {
        GDateTime *now = g_date_time_new_now_local();
        char *time = g_date_time_format(now, "%H:%M:%S.%f");
        time[12] = '\0'; // milliseconds are enough
        std::fprintf(g_file, "%s %-8s %s%s%s\n", time, level_name(level), domain ? domain : "",
                     domain ? ": " : "", message ? message : "");
        if (!trace.empty())
            std::fputs(trace.c_str(), g_file);
        g_free(time);
        g_date_time_unref(now);
    }
    return result;
}

} // namespace

std::filesystem::path file_path() {
    return std::filesystem::path(g_get_user_config_dir()) / "gtkdrop" / "log" / "gtkdrop.log";
}

void init() {
    const auto path = file_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    g_file = std::fopen(path.c_str(), "w+");
    if (g_file)
        std::setvbuf(g_file, nullptr, _IOLBF, 0); // line-buffered: a crash still leaves everything before it
    g_log_set_writer_func(writer, nullptr, nullptr);
    if (!g_file)
        g_warning("Could not open the log file %s", path.c_str());
    else
        g_message("GTKdrop %s started", gtkdrop::version_line().c_str());
}

} // namespace gtkdrop::log
