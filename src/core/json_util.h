#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace gtkdrop {

using json = nlohmann::json;

// Parses JSON the way SMAPI (Newtonsoft) accepts it: a UTF-8 BOM, // and /* */ comments and
// trailing commas are all tolerated. Returns nullopt on anything else that is malformed.
std::optional<json> parse_lenient_json(std::string_view text);

std::optional<std::string> read_text_file(const std::filesystem::path &path);
std::optional<json> read_json_file(const std::filesystem::path &path);

// Writes atomically (temp file + rename), so a crash never leaves half a file behind.
bool write_text_file(const std::filesystem::path &path, const std::string &text);
bool write_json_file(const std::filesystem::path &path, const json &value, int indent = 2);

// Case-insensitive object lookup, as SMAPI manifests are inconsistent about key casing.
const json *find_key_ci(const json &object, std::string_view key);
std::string get_string_ci(const json &object, std::string_view key, const std::string &fallback = {});

bool iequals(std::string_view a, std::string_view b);
std::string to_lower(std::string_view s);

} // namespace gtkdrop
