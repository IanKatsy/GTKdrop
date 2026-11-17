#pragma once

#include <glib.h>
#include <sys/types.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace gtkdrop {

namespace fs = std::filesystem;

// How the game is started. Both run SMAPI (its installer replaces the game's StardewValley launcher) with GTKdrop's
// selected-mods folder, passed as SMAPI_MODS_PATH: SMAPI's launcher script drops all command-line arguments.
enum class LaunchMethod {
    Ask,   // not chosen yet: asked on the first launch from a Steam install
    Steam, // steam -applaunch 413150 …  (Steam overlay, playtime and Cloud saves keep working)
    Gog,   // runs <game>/StardewValley directly, as Stardrop does; for GOG's native Linux build
};

std::string to_string(LaunchMethod method);
LaunchMethod launch_method_from_string(std::string_view s);

namespace gog {
// GOG's native Linux installer default (~/GOG Games/Stardew Valley/game), when it holds the game.
fs::path find_stardew_dir();
} // namespace gog

// Steam drops extra -applaunch arguments, so a Steam launch can't pass --mods-path itself. Instead the game's Steam
// launch options run GTKdrop's wrapper script ("<script>" %command%). Right before a launch GTKdrop writes a
// one-time request (the profile's mods folder); the script uses it once, if it's fresh, and deletes it. Started from
// Steam itself there's no request, and the game runs as usual with the default Mods folder.
namespace steam_wrapper {
constexpr int kRequestSeconds = 120;        // older requests are stale (a launch that never reached the game)
fs::path script();                          // ~/.local/share/gtkdrop/steam-launch.sh
std::string launch_options();               // what goes into Steam's launch options
bool prepare(const fs::path &mods_path, std::string *error); // writes the script and the one-time request
void cancel_request();                      // the launch failed: don't let a later Steam start pick it up
bool configured();                          // Steam's settings have it, or a launch went through it before
void remember_verified(bool ok);            // a launch did (not) come through it
} // namespace steam_wrapper

// Starts the game and tracks the SMAPI process until it exits. Lives on the GLib main loop.
class GameLauncher {
public:
    struct Callbacks {
        std::function<void()> started;                 // SMAPI is running
        std::function<void()> exited;                  // SMAPI exited
        std::function<void(const std::string &)> failed; // launch failed or SMAPI never appeared
    };

    GameLauncher() = default;
    GameLauncher(const GameLauncher &) = delete;
    GameLauncher &operator=(const GameLauncher &) = delete;
    ~GameLauncher();

    // Returns false (with `error`) when the launch couldn't even be started.
    bool launch(LaunchMethod method, const fs::path &game_dir, const fs::path &mods_path, Callbacks callbacks,
                std::string *error);
    // Watches a SMAPI that is already running (e.g. GTKdrop was opened while playing).
    bool attach_to_running(Callbacks callbacks);

    bool busy() const { return state_ != State::Idle; }
    bool running() const { return state_ == State::Running; }

    // The PID of a running SMAPI process, found by its executable name in /proc.
    static std::optional<pid_t> find_smapi_process();
    // The mods folder SMAPI was started with (from /proc), once running: its --mods-path argument, else its
    // SMAPI_MODS_PATH variable (SMAPI's launcher script drops all arguments, so the variable is what gets through);
    // nullopt when it has neither, i.e. the default Mods folder.
    std::optional<std::string> mods_path_arg() const;

private:
    enum class State { Idle, Waiting, Running };

    static gboolean poll_cb(gpointer self);
    static gboolean pidfd_cb(gint fd, GIOCondition condition, gpointer self);
    void start_waiting(int timeout_seconds);
    void on_found(pid_t pid);
    void finish_exited();
    void reset();

    State state_ = State::Idle;
    Callbacks callbacks_;
    guint poll_id_ = 0;     // looking for SMAPI to appear, or (without pidfd) for it to go away
    guint fd_watch_id_ = 0; // pidfd readable = SMAPI exited
    int pidfd_ = -1;
    pid_t pid_ = 0;
    gint64 deadline_us_ = 0;
};

} // namespace gtkdrop
