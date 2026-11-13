#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

namespace cli {

// Minimal streaming JSON writer: compact output, strings escaped, invalid UTF-8 replaced by U+FFFD.
class JsonWriter {
public:
    explicit JsonWriter(std::ostream& out) : out_(out) {}

    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view name);

    JsonWriter& value(std::string_view text);
    JsonWriter& value(const char* text) { return value(std::string_view(text)); }
    JsonWriter& value(std::uint64_t number);
    JsonWriter& value(std::int64_t number);
    JsonWriter& value(bool flag);
    JsonWriter& null();

private:
    void before_value();
    void write_string(std::string_view text);

    std::ostream& out_;
    std::vector<bool> first_; // per open container: no element written yet
    bool after_key_ = false;
};

} // namespace cli
