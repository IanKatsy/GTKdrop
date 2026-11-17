#include "payload.h"

#include "json_util.h"
#include "settings.h"

namespace gtkdrop {

std::filesystem::path payloads_dir() { return paths::data_dir() / "payloads"; }

std::filesystem::path write_payload(const std::string &file_name,
                                    const std::vector<std::pair<std::string, std::string>> &mods,
                                    const std::string &nxm_url) {
    json list = json::array();
    for (const auto &[name, url]: mods)
        list.push_back({{name, url}});
    const auto path = payloads_dir() / file_name;
    json doc = json::object();
    if (!nxm_url.empty())
        doc["nxm"] = nxm_url;
    doc["mods"] = list;
    return write_json_file(path, doc) ? path : std::filesystem::path();
}

} // namespace gtkdrop
