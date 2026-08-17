#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>

namespace robot_switch_server {

// JSON string escaping
std::string JsonEscape(std::string_view input);

// Check if string looks like a JSON object
bool LooksLikeJsonObject(std::string_view input);

// Get current Unix timestamp in milliseconds
int64_t NowUnixMs();

// JSON builder for constructing JSON strings efficiently
class JsonBuilder {
public:
    JsonBuilder();
    explicit JsonBuilder(std::size_t reserve_size);

    // Add a key-value pair
    JsonBuilder& Add(std::string_view key, std::string_view value);
    JsonBuilder& Add(std::string_view key, const std::string& value);
    JsonBuilder& Add(std::string_view key, int64_t value);
    JsonBuilder& Add(std::string_view key, bool value);

    // Add raw JSON value (no escaping, for nested objects/arrays)
    JsonBuilder& AddRaw(std::string_view key, std::string_view raw_json);

    // Add null value
    JsonBuilder& AddNull(std::string_view key);

    // Build the final JSON string
    std::string Build() const;

    // Check if any fields have been added
    bool IsEmpty() const { return first_field_; }

private:
    std::ostringstream stream_;
    bool first_field_;

    void WriteKey(std::string_view key);
    void WriteSeparator();
};

// Helper for building JSON arrays
class JsonArrayBuilder {
public:
    JsonArrayBuilder();
    explicit JsonArrayBuilder(std::size_t reserve_size);

    JsonArrayBuilder& Add(std::string_view value);
    JsonArrayBuilder& Add(const std::string& value);
    JsonArrayBuilder& Add(int64_t value);
    JsonArrayBuilder& Add(bool value);
    JsonArrayBuilder& AddRaw(std::string_view raw_json);

    std::string Build() const;

private:
    std::ostringstream stream_;
    bool first_element_;

    void WriteSeparator();
};

}  // namespace robot_switch_server
