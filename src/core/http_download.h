#pragma once

#include <gio/gio.h>

#include <filesystem>
#include <functional>
#include <string>

namespace gtkdrop {

// Downloads `url` into `dir` as `name`; without one, as the server's Content-Disposition name or the URL's last
// path segment (Nexus' CDN sometimes ends links in a random ID instead of the archive name, so pass the name). Writes <name>.part and renames it when complete, so a finished file is always whole; an existing
// <name> counts as already downloaded. `progress` gets bytes received and the total (0 if unknown).
void download_file(const std::string &url, const std::filesystem::path &dir, const std::string &name,
                   GCancellable *cancellable,
                   std::function<void(goffset received, goffset total)> progress,
                   std::function<void(std::filesystem::path path, const std::string &error)> done);

} // namespace gtkdrop
