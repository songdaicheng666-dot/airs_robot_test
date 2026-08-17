#include "robot_switch_server/utils/json_utils.hpp"

#include <chrono>

namespace robot_switch_server {

std::string JsonEscape(std::string_view input) {
    std::string escaped;
    escaped.reserve(input.size() + 8);

    constexpr char kHex[] = "0123456789abcdef";
    for (unsigned char ch : input) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    escaped += "\\u00";
                    escaped += kHex[(ch >> 4) & 0x0F];
                    escaped += kHex[ch & 0x0F];
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

bool LooksLikeJsonObject(std::string_view input) {
    std::size_t first = 0;
    while (first < input.size() &&
           std::isspace(static_cast<unsigned char>(input[first]))) {
        ++first;
    }
    if (first >= input.size() || input[first] != '{') {
        return false;
    }

    std::size_t last = input.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(input[last - 1]))) {
        --last;
    }

    return last > first && input[last - 1] == '}';
}

int64_t NowUnixMs() {
    const auto now = std::chrono::system_clock::now();
    const auto epoch =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return static_cast<int64_t>(epoch.count());
}

// JsonBuilder implementation

JsonBuilder::JsonBuilder() : first_field_(true) {
    stream_ << '{';
}

JsonBuilder::JsonBuilder(std::size_t reserve_size) : first_field_(true) {
    stream_.str().reserve(reserve_size);
    stream_ << '{';
}

void JsonBuilder::WriteSeparator() {
    if (!first_field_) {
        stream_ << ',';
    }
    first_field_ = false;
}

void JsonBuilder::WriteKey(std::string_view key) {
    WriteSeparator();
    stream_ << '"' << JsonEscape(key) << "\":";
}

JsonBuilder& JsonBuilder::Add(std::string_view key, std::string_view value) {
    WriteKey(key);
    stream_ << '"' << JsonEscape(value) << '"';
    return *this;
}

JsonBuilder& JsonBuilder::Add(std::string_view key, const std::string& value) {
    WriteKey(key);
    stream_ << '"' << JsonEscape(value) << '"';
    return *this;
}

JsonBuilder& JsonBuilder::Add(std::string_view key, int64_t value) {
    WriteKey(key);
    stream_ << value;
    return *this;
}

JsonBuilder& JsonBuilder::Add(std::string_view key, bool value) {
    WriteKey(key);
    stream_ << (value ? "true" : "false");
    return *this;
}

JsonBuilder& JsonBuilder::AddRaw(std::string_view key, std::string_view raw_json) {
    WriteKey(key);
    stream_ << raw_json;
    return *this;
}

JsonBuilder& JsonBuilder::AddNull(std::string_view key) {
    WriteKey(key);
    stream_ << "null";
    return *this;
}

std::string JsonBuilder::Build() const {
    std::string result = stream_.str();
    result += '}';
    return result;
}

// JsonArrayBuilder implementation

JsonArrayBuilder::JsonArrayBuilder() : first_element_(true) {
    stream_ << '[';
}

JsonArrayBuilder::JsonArrayBuilder(std::size_t reserve_size) : first_element_(true) {
    stream_.str().reserve(reserve_size);
    stream_ << '[';
}

void JsonArrayBuilder::WriteSeparator() {
    if (!first_element_) {
        stream_ << ',';
    }
    first_element_ = false;
}

JsonArrayBuilder& JsonArrayBuilder::Add(std::string_view value) {
    WriteSeparator();
    stream_ << '"' << JsonEscape(value) << '"';
    return *this;
}

JsonArrayBuilder& JsonArrayBuilder::Add(const std::string& value) {
    WriteSeparator();
    stream_ << '"' << JsonEscape(value) << '"';
    return *this;
}

JsonArrayBuilder& JsonArrayBuilder::Add(int64_t value) {
    WriteSeparator();
    stream_ << value;
    return *this;
}

JsonArrayBuilder& JsonArrayBuilder::Add(bool value) {
    WriteSeparator();
    stream_ << (value ? "true" : "false");
    return *this;
}

JsonArrayBuilder& JsonArrayBuilder::AddRaw(std::string_view raw_json) {
    WriteSeparator();
    stream_ << raw_json;
    return *this;
}

std::string JsonArrayBuilder::Build() const {
    std::string result = stream_.str();
    result += ']';
    return result;
}

}  // namespace robot_switch_server
