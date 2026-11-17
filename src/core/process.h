#pragma once

#include <gio/gio.h>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gtkdrop {

struct ProcessOptions {
    std::filesystem::path cwd;
    std::vector<std::pair<std::string, std::string>> env;
    // Each output line as it arrives (terminal colour codes removed); `done` still gets the whole output.
    std::function<void(const std::string &line)> on_line;
};

// "$ cmd arg 'arg with spaces'": how a command is shown in logs.
std::string command_line(const std::vector<std::string> &argv);
std::string strip_ansi(const std::string &text);

// Runs a command with stdout+stderr collected. `done` gets the exit code (130 when killed by a signal) and the
// output. Returns the running process (a new reference, e.g. to signal it), or nullptr with `error` set.
GSubprocess *run_process(const std::vector<std::string> &argv, const ProcessOptions &options,
                         std::function<void(int code, std::string output)> done, std::string *error);

std::string last_line(const std::string &text);

} // namespace gtkdrop
