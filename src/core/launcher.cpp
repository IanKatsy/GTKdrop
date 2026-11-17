#include "launcher.h"
#include "steam.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <vector>

namespace gtkdrop {

std::string to_string(LaunchMethod method) {
    switch (method) {
        case LaunchMethod::Steam: return "steam";
        case LaunchMethod::Gog: return "gog";
        case LaunchMethod::Ask: break;
    }
    return "";
}

LaunchMethod launch_method_from_string(std::string_view s) {
    if (s == "steam") return LaunchMethod::Steam;
    if (s == "gog") return LaunchMethod::Gog;
    return LaunchMethod::Ask;
}

fs::path gog::find_stardew_dir() {
    // Where GOG's Linux installer puts it by default; SMAPI's own build files look here too.
    const fs::path dir = fs::path(g_get_home_dir()) / "GOG Games" / "Stardew Valley" / "game";
    std::error_code ec;
    return fs::exists(dir / "Stardew Valley.dll", ec) ? dir : fs::path();
}

namespace {

constexpr int kSteamStartTimeout = 180; // Steam may have to start (or update) first
constexpr int kDirectStartTimeout = 60;

int pidfd_open(pid_t pid) {
#ifdef SYS_pidfd_open
    return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
#else
    errno = ENOSYS;
    return -1;
#endif
}

bool spawn(const std::vector<std::string> &argv, const fs::path &cwd, const std::vector<std::pair<const char *, std::string>> &env,
           bool silence, std::string *error) {
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
            silence ? GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE)
                    : G_SUBPROCESS_FLAGS_NONE);
    if (!cwd.empty())
        g_subprocess_launcher_set_cwd(launcher, cwd.c_str());
    for (const auto &[key, value]: env)
        g_subprocess_launcher_setenv(launcher, key, value.c_str(), TRUE);

    std::vector<const char *> args;
    for (const auto &a: argv)
        args.push_back(a.c_str());
    args.push_back(nullptr);

    GError *err = nullptr;
    GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, args.data(), &err);
    g_object_unref(launcher);
    if (!proc) {
        if (error)
            *error = err->message;
        g_error_free(err);
        return false;
    }
    // Only reaps it: the launcher (Steam, or SMAPI's script opening a terminal) can exit long before the game.
    g_subprocess_wait_async(proc, nullptr, [](GObject *src, GAsyncResult *res, gpointer) {
        g_subprocess_wait_finish(G_SUBPROCESS(src), res, nullptr);
        g_object_unref(src);
    }, nullptr);
    return true;
}

// The command that reaches the Steam client: native first, then the Flatpak.
std::vector<std::string> steam_command() {
    if (char *steam = g_find_program_in_path("steam")) {
        std::string path = steam;
        g_free(steam);
        return {path};
    }
    std::error_code ec;
    const auto flatpak = fs::path(g_get_home_dir()) / ".var/app/com.valvesoftware.Steam";
    if (fs::is_directory(flatpak, ec))
        if (char *fp = g_find_program_in_path("flatpak")) {
            std::string path = fp;
            g_free(fp);
            return {path, "run", "com.valvesoftware.Steam"};
        }
    return {};
}

} // namespace

namespace steam_wrapper {

namespace {
fs::path request_file() { return fs::path(g_get_user_config_dir()) / "gtkdrop" / "steam-launch-request"; }
fs::path verified_file() { return fs::path(g_get_user_data_dir()) / "gtkdrop" / "steam-launch.ok"; }

// Steam's per-user localconfig.vdf: …"413150" { … "LaunchOptions" "…" … }
bool vdf_has_wrapper(const fs::path &vdf) {
    gchar *text = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(vdf.c_str(), &text, &len, nullptr))
        return false;
    const std::string s(text, len);
    g_free(text);
    for (size_t at = s.find("\"413150\""); at != std::string::npos; at = s.find("\"413150\"", at + 1)) {
        size_t open = s.find_first_not_of(" \t\r\n", at + 8);
        if (open == std::string::npos || s[open] != '{')
            continue;
        int depth = 0;
        size_t end = open;
        for (; end < s.size(); ++end) {
            if (s[end] == '{')
                ++depth;
            else if (s[end] == '}' && --depth == 0)
                break;
        }
        const std::string block = s.substr(open, end - open);
        const auto opt = block.find("\"LaunchOptions\"");
        if (opt != std::string::npos && block.find("steam-launch.sh", opt) != std::string::npos)
            return true;
    }
    return false;
}
} // namespace

fs::path script() { return fs::path(g_get_user_data_dir()) / "gtkdrop" / "steam-launch.sh"; }

std::string launch_options() { return "\"" + script().string() + "\" %command%"; }

bool prepare(const fs::path &mods_path, std::string *error) {
    std::error_code ec;
    fs::create_directories(script().parent_path(), ec);
    fs::create_directories(request_file().parent_path(), ec);
    fs::remove(request_file().parent_path() / "steam-mods-path", ec); // earlier versions' always-on file
    const std::string body =
            "#!/bin/sh\n"
            "# GTKdrop: Stardew Valley's Steam launch options run this as  \"" + script().string() + "\" %command%\n"
            "# Launched from GTKdrop, it left a one-time request with the profile's mods folder: use it once. Started\n"
            "# from Steam itself there's none, and the game runs as usual (the default Mods folder).\n"
            "f=\"${XDG_CONFIG_HOME:-$HOME/.config}/gtkdrop/steam-launch-request\"\n"
            "if [ -f \"$f\" ]; then\n"
            "    p=$(cat \"$f\")\n"
            "    age=$(( $(date +%s) - $(stat -c %Y \"$f\") ))\n"
            "    rm -f \"$f\"\n"
            "    if [ \"$age\" -le " + std::to_string(kRequestSeconds) + " ] && [ -d \"$p\" ]; then\n"
            "        export SMAPI_MODS_PATH=\"$p\"\n"
            "        exec \"$@\" --mods-path \"$p\"\n"
            "    fi\n"
            "fi\n"
            "exec \"$@\"\n";
    GError *err = nullptr;
    if (!g_file_set_contents(script().c_str(), body.c_str(), gssize(body.size()), &err) ||
        !g_file_set_contents(request_file().c_str(), mods_path.c_str(), -1, &err)) {
        if (error)
            *error = err ? err->message : "couldn't write the Steam launch script";
        g_clear_error(&err);
        return false;
    }
    fs::permissions(script(), fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec, ec);
    return true;
}

void cancel_request() {
    std::error_code ec;
    fs::remove(request_file(), ec);
}

bool configured() {
    std::error_code ec;
    if (fs::exists(verified_file(), ec))
        return true;
    for (const auto &root: steam::steam_roots())
        for (const auto &user: fs::directory_iterator(root / "userdata", ec))
            if (vdf_has_wrapper(user.path() / "config" / "localconfig.vdf"))
                return true;
    return false;
}

void remember_verified(bool ok) {
    std::error_code ec;
    if (ok) {
        fs::create_directories(verified_file().parent_path(), ec);
        g_file_set_contents(verified_file().c_str(), "", 0, nullptr);
    } else {
        fs::remove(verified_file(), ec);
    }
}

} // namespace steam_wrapper

std::optional<std::string> GameLauncher::mods_path_arg() const {
    if (pid_ <= 0)
        return std::nullopt;
    gchar *raw = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(("/proc/" + std::to_string(pid_) + "/cmdline").c_str(), &raw, &len, nullptr))
        return std::nullopt;
    std::vector<std::string> args;
    for (gsize i = 0; i < len;) {
        args.emplace_back(raw + i);
        i += args.back().size() + 1;
    }
    g_free(raw);
    std::optional<std::string> found;
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--mods-path")
            found = args[i + 1]; // the last one wins, as in SMAPI
    if (found)
        return found;
    if (g_file_get_contents(("/proc/" + std::to_string(pid_) + "/environ").c_str(), &raw, &len, nullptr)) {
        constexpr std::string_view key = "SMAPI_MODS_PATH=";
        for (gsize i = 0; i < len;) {
            const std::string_view var(raw + i);
            if (var.starts_with(key) && var.size() > key.size())
                found = std::string(var.substr(key.size()));
            i += var.size() + 1;
        }
        g_free(raw);
    }
    return found;
}

GameLauncher::~GameLauncher() { reset(); }

void GameLauncher::reset() {
    g_clear_handle_id(&poll_id_, g_source_remove);
    g_clear_handle_id(&fd_watch_id_, g_source_remove);
    if (pidfd_ >= 0) {
        close(pidfd_);
        pidfd_ = -1;
    }
    pid_ = 0;
    state_ = State::Idle;
}

std::optional<pid_t> GameLauncher::find_smapi_process() {
    // Matched on the executable, not the 15-character comm name. SMAPI is a .NET apphost, so the process image
    // is the StardewModdingAPI file itself however it was started (Steam, a terminal, gamemoderun …).
    const uid_t uid = getuid();
    std::error_code ec;
    for (const auto &entry: fs::directory_iterator("/proc", ec)) {
        const auto name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::error_code e;
        struct stat st {};
        if (stat(entry.path().c_str(), &st) != 0 || st.st_uid != uid)
            continue;
        const auto exe = fs::read_symlink(entry.path() / "exe", e);
        if (!e && exe.filename() == "StardewModdingAPI")
            return static_cast<pid_t>(std::atoi(name.c_str()));
    }
    return std::nullopt;
}

bool GameLauncher::launch(LaunchMethod method, const fs::path &game_dir, const fs::path &mods_path,
                          Callbacks callbacks, std::string *error) {
    if (busy()) {
        if (error)
            *error = "The game is already starting or running";
        return false;
    }

    bool ok = false;
    int timeout = kDirectStartTimeout;
    if (method == LaunchMethod::Steam) {
        auto cmd = steam_command();
        if (cmd.empty()) {
            if (error)
                *error = "Steam isn't installed (no steam command and no Steam Flatpak)";
            return false;
        }
        // The mods folder goes through the wrapper in the game's Steam launch options (Steam drops arguments
        // after -applaunch). If Steam isn't running this starts it first.
        if (!steam_wrapper::prepare(mods_path, error))
            return false;
        cmd.emplace_back("-applaunch");
        cmd.emplace_back("413150");
        ok = spawn(cmd, {}, {}, /*silence=*/true, error);
        timeout = kSteamStartTimeout;
    } else if (method == LaunchMethod::Gog) {
        // As Stardrop does: SMAPI's launcher script (installed over the game's StardewValley), which opens SMAPI's
        // console in a terminal. The env variable and the argument both set the mods folder.
        std::error_code ec;
        fs::path script = game_dir / "StardewValley";
        if (!fs::exists(script, ec))
            script = game_dir / "StardewModdingAPI";
        if (!fs::exists(script, ec)) {
            if (error)
                *error = "SMAPI's launcher wasn't found in " + game_dir.string();
            return false;
        }
        ok = spawn({script.string(), "--mods-path", mods_path.string()}, game_dir,
                   {{"SMAPI_MODS_PATH", mods_path.string()}}, /*silence=*/false, error);
    } else {
        if (error)
            *error = "No launch method chosen";
        return false;
    }
    if (!ok) {
        if (method == LaunchMethod::Steam)
            steam_wrapper::cancel_request();
        return false;
    }

    callbacks_ = std::move(callbacks);
    start_waiting(timeout);
    return true;
}

bool GameLauncher::attach_to_running(Callbacks callbacks) {
    if (busy())
        return false;
    const auto pid = find_smapi_process();
    if (!pid)
        return false;
    callbacks_ = std::move(callbacks);
    on_found(*pid);
    return true;
}

void GameLauncher::start_waiting(int timeout_seconds) {
    state_ = State::Waiting;
    deadline_us_ = g_get_monotonic_time() + gint64(timeout_seconds) * G_USEC_PER_SEC;
    poll_id_ = g_timeout_add(500, poll_cb, this);
}

gboolean GameLauncher::poll_cb(gpointer data) {
    auto *self = static_cast<GameLauncher *>(data);
    if (self->state_ == State::Waiting) {
        if (const auto pid = find_smapi_process()) {
            self->poll_id_ = 0;
            self->on_found(*pid);
            return G_SOURCE_REMOVE;
        }
        if (g_get_monotonic_time() > self->deadline_us_) {
            self->poll_id_ = 0;
            auto failed = self->callbacks_.failed;
            self->reset();
            if (failed)
                failed("SMAPI didn't start. Check the SMAPI log, or try starting the game once without GTKdrop.");
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }
    // Running without a pidfd: poll for the process to disappear.
    std::error_code ec;
    if (!fs::exists("/proc/" + std::to_string(self->pid_), ec)) {
        self->poll_id_ = 0;
        self->finish_exited();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

gboolean GameLauncher::pidfd_cb(gint, GIOCondition, gpointer data) {
    auto *self = static_cast<GameLauncher *>(data);
    self->fd_watch_id_ = 0;
    self->finish_exited();
    return G_SOURCE_REMOVE;
}

void GameLauncher::on_found(pid_t pid) {
    state_ = State::Running;
    pid_ = pid;
    pidfd_ = pidfd_open(pid);
    if (pidfd_ >= 0)
        fd_watch_id_ = g_unix_fd_add(pidfd_, G_IO_IN, pidfd_cb, this); // readable once the process exits
    else
        poll_id_ = g_timeout_add_seconds(1, poll_cb, this);
    if (callbacks_.started)
        callbacks_.started();
}

void GameLauncher::finish_exited() {
    auto exited = callbacks_.exited;
    reset();
    if (exited)
        exited();
}

} // namespace gtkdrop
