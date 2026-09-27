#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace lemon {
using Json = nlohmann::json;
using EventSink = std::function<void(Json)>;
inline constexpr std::size_t max_frame = 128 * 1024;
inline constexpr std::size_t chunk_size = 64 * 1024;
inline constexpr std::size_t transfer_window = 8 * chunk_size;
inline constexpr std::size_t max_text = 16 * 1024;
inline constexpr std::uint64_t default_max_file = 1024ULL * 1024 * 1024;
inline constexpr int protocol_version = 2;

inline std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline std::string string_field(const Json& value, const char* key, std::size_t limit = 256,
                                bool allow_empty = false) {
    if (!value.contains(key) || !value[key].is_string()) throw std::runtime_error(std::string("Missing text field: ") + key);
    auto result = value[key].get<std::string>();
    if ((!allow_empty && result.empty()) || result.size() > limit)
        throw std::runtime_error(std::string("Invalid length: ") + key);
    return result;
}
inline std::uint64_t uint_field(const Json& value, const char* key, std::uint64_t limit) {
    if (!value.contains(key) || !value[key].is_number_unsigned())
        throw std::runtime_error(std::string("Invalid unsigned integer: ") + key);
    auto result = value[key].get<std::uint64_t>();
    if (result > limit) throw std::runtime_error(std::string("Value exceeds limit: ") + key);
    return result;
}
inline bool simple_name(const std::string& text, std::size_t limit = 48) {
    if (text.empty() || text.size() > limit) return false;
    return std::none_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch <= 32 || ch == 127 || ch == '/' || ch == '\\' || ch == '#';
    });
}
inline std::string terminal_text(const std::string& text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto ch = static_cast<unsigned char>(text[i]);
        // UTF-8 encoded C1 controls can also affect terminal emulators.
        if (ch == 0xc2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
            static_cast<unsigned char>(text[i + 1]) <= 0x9f) { result += '?'; ++i; continue; }
        if (ch == '\n' || ch == '\t' || (ch >= 32 && ch != 127)) result += static_cast<char>(ch);
        else result += '?';
    }
    return result;
}
inline bool safe_filename(const std::string& name) {
    return !name.empty() && name.size() <= 240 && name != "." && name != ".." &&
        std::none_of(name.begin(), name.end(), [](unsigned char ch) {
            return ch < 32 || ch == 127 || ch == '/' || ch == '\\' || ch == ':';
        });
}
} // namespace lemon
