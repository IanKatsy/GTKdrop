#include "nxm_handler.h"

#include "json_util.h"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <filesystem>

namespace gtkdrop::nxm_handler {

namespace fs = std::filesystem;

namespace {

constexpr char kDesktopId[] = "dev.gtkdrop.GTKdrop.desktop";
constexpr char kMimeType[] = "x-scheme-handler/nxm";

fs::path desktop_file() { return fs::path(g_get_user_data_dir()) / "applications" / kDesktopId; }

// Desktop Entry Exec quoting: the whole path in double quotes, with ", `, $ and \ escaped.
std::string quote_exec(const std::string &path) {
    std::string out = "\"";
    for (char c: path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

} // namespace

bool is_default() {
    GAppInfo *info = g_app_info_get_default_for_uri_scheme("nxm");
    if (!info)
        return false;
    const bool ours = g_strcmp0(g_app_info_get_id(info), kDesktopId) == 0;
    g_object_unref(info);
    return ours;
}

bool make_default(std::string *error) {
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) {
        if (error)
            *error = "Couldn't find GTKdrop's executable: " + ec.message();
        return false;
    }

    const std::string entry = "[Desktop Entry]\n"
                              "Type=Application\n"
                              "Name=GTKdrop\n"
                              "Comment=Stardew Valley mod manager\n"
                              "Exec=" + quote_exec(exe.string()) + " %u\n"
                              "Icon=applications-games\n"
                              "Terminal=false\n"
                              "Categories=Game;Utility;\n"
                              "MimeType=" + std::string(kMimeType) + ";\n"
                              "StartupNotify=true\n";
    if (!write_text_file(desktop_file(), entry)) {
        if (error)
            *error = "Couldn't write " + desktop_file().string();
        return false;
    }

    GDesktopAppInfo *info = g_desktop_app_info_new_from_filename(desktop_file().c_str());
    if (!info) {
        if (error)
            *error = "The desktop entry GTKdrop wrote isn't valid";
        return false;
    }
    GError *err = nullptr;
    const bool ok = g_app_info_set_as_default_for_type(G_APP_INFO(info), kMimeType, &err);
    if (!ok && error)
        *error = err->message;
    if (err)
        g_error_free(err);
    g_object_unref(info);
    return ok;
}

void unset_default() {
    if (GDesktopAppInfo *info = g_desktop_app_info_new_from_filename(desktop_file().c_str())) {
        g_app_info_remove_supports_type(G_APP_INFO(info), kMimeType, nullptr);
        g_object_unref(info);
    }
    std::error_code ec;
    fs::remove(desktop_file(), ec);
}

} // namespace gtkdrop::nxm_handler
