#include "json_util.h"

#include <glib.h>

#include <algorithm>

namespace gtkdrop {

namespace {

// Drops comments and trailing commas outside string literals.
std::string strip_json_extensions(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool in_string = false;
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (in_string) {
            out += c;
            if (c == '\\' && i + 1 < in.size())
                out += in[++i];
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
            out += c;
        } else if (c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            while (i < in.size() && in[i] != '\n')
                ++i;
            out += '\n';
        } else if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/'))
                ++i;
            ++i; // skip the closing '/'
        } else if (c == ',') {
            // A comma followed (after whitespace/comments, already stripped or not) by } or ] is dropped.
            size_t j = i + 1;
            while (j < in.size()) {
                if (g_ascii_isspace(in[j])) {
                    ++j;
                } else if (in[j] == '/' && j + 1 < in.size() && in[j + 1] == '/') {
                    while (j < in.size() && in[j] != '\n') ++j;
                } else if (in[j] == '/' && j + 1 < in.size() && in[j + 1] == '*') {
                    j += 2;
                    while (j + 1 < in.size() && !(in[j] == '*' && in[j + 1] == '/')) ++j;
                    j += 2;
                } else {
                    break;
                }
            }
            if (j < in.size() && (in[j] == '}' || in[j] == ']'))
                continue;
            out += c;
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

std::optional<json> parse_lenient_json(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    json doc = json::parse(strip_json_extensions(text), nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded())
        return std::nullopt;
    return doc;
}

std::optional<std::string> read_text_file(const std::filesystem::path &path) {
    char *contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &length, nullptr))
        return std::nullopt;
    std::string text(contents, length);
    g_free(contents);
    return text;
}

std::optional<json> read_json_file(const std::filesystem::path &path) {
    auto text = read_text_file(path);
    return text ? parse_lenient_json(*text) : std::nullopt;
}

bool write_text_file(const std::filesystem::path &path, const std::string &text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    GError *error = nullptr;
    if (!g_file_set_contents(path.c_str(), text.data(), static_cast<gssize>(text.size()), &error)) {
        g_warning("Could not write %s: %s", path.c_str(), error->message);
        g_error_free(error);
        return false;
    }
    return true;
}

bool write_json_file(const std::filesystem::path &path, const json &value, int indent) {
    return write_text_file(path, value.dump(indent) + "\n");
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && g_ascii_strncasecmp(a.data(), b.data(), a.size()) == 0;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
    return out;
}

const json *find_key_ci(const json &object, std::string_view key) {
    if (!object.is_object())
        return nullptr;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (iequals(it.key(), key))
            return &it.value();
    return nullptr;
}

std::string get_string_ci(const json &object, std::string_view key, const std::string &fallback) {
    const json *value = find_key_ci(object, key);
    return value && value->is_string() ? value->get<std::string>() : fallback;
}

} // namespace gtkdrop
