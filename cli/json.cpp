#include "json.hpp"

namespace cli {

namespace {

// Length of the valid UTF-8 sequence starting at text[i], or 0 if it is invalid.
std::size_t utf8_sequence(std::string_view text, std::size_t i)
{
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t length = 0;
    std::uint32_t cp = 0;
    if (c < 0x80) {
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        length = 2;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        length = 3;
        cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        length = 4;
        cp = c & 0x07u;
    } else {
        return 0;
    }
    if (i + length > text.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const auto next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (next & 0x3Fu);
    }
    const bool overlong = (length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000);
    if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return length;
}

} // namespace

void JsonWriter::before_value()
{
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) {
            out_ << ',';
        }
        first_.back() = false;
    }
}

JsonWriter& JsonWriter::begin_object()
{
    before_value();
    out_ << '{';
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_object()
{
    first_.pop_back();
    out_ << '}';
    return *this;
}

JsonWriter& JsonWriter::begin_array()
{
    before_value();
    out_ << '[';
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_array()
{
    first_.pop_back();
    out_ << ']';
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name)
{
    before_value();
    write_string(name);
    out_ << ':';
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view text)
{
    before_value();
    write_string(text);
    return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t number)
{
    before_value();
    out_ << number;
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t number)
{
    before_value();
    out_ << number;
    return *this;
}

JsonWriter& JsonWriter::value(bool flag)
{
    before_value();
    out_ << (flag ? "true" : "false");
    return *this;
}

JsonWriter& JsonWriter::null()
{
    before_value();
    out_ << "null";
    return *this;
}

void JsonWriter::write_string(std::string_view text)
{
    static const char hex[] = "0123456789abcdef";
    out_ << '"';
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            out_ << '\\' << static_cast<char>(c);
        } else if (c == '\n') {
            out_ << "\\n";
        } else if (c == '\r') {
            out_ << "\\r";
        } else if (c == '\t') {
            out_ << "\\t";
        } else if (c < 0x20) {
            out_ << "\\u00" << hex[c >> 4] << hex[c & 15];
        } else if (c >= 0x80) {
            const std::size_t length = utf8_sequence(text, i);
            if (length == 0) {
                out_ << "\\ufffd";
                ++i;
                continue;
            }
            out_.write(text.data() + i, static_cast<std::streamsize>(length));
            i += length;
            continue;
        } else {
            out_ << static_cast<char>(c);
        }
        ++i;
    }
    out_ << '"';
}

} // namespace cli
