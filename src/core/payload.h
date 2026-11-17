#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace gtkdrop {

std::filesystem::path payloads_dir(); // ~/.config/gtkdrop/payloads

// A download list for the user: {"nxm": "<link it came from>", "mods": [{"<mod name>": "<url>"}, …]}, written to
// payloads_dir()/<file_name> ("nxm" only when made from a link). Returns the file's path, or empty if it couldn't
// be written.
std::filesystem::path write_payload(const std::string &file_name,
                                    const std::vector<std::pair<std::string, std::string>> &mods,
                                    const std::string &nxm_url = {});

} // namespace gtkdrop
