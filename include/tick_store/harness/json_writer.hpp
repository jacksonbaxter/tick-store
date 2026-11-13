#pragma once

// A small JSON writer for the tools' summaries. Nothing here reads JSON.
//
// Strings are bytes, and the output is always valid UTF-8. A well-formed UTF-8 sequence is
// written as it is, so a path with non-ASCII names reads back as the same path. A byte that
// is not part of one, as a corrupt store's type byte or a path in another encoding can be, is
// written as \udcXX: the lone low surrogate Python's surrogateescape gives that byte, so
// json.loads holds the same string os.listdir would, and os.fsencode gives the byte back.
// `"`, `\` and bytes below 0x20 are escaped as JSON requires.

#include <concepts>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tick_store::harness {

class JsonWriter {
public:
    void begin_object() { open('{'); }
    void end_object() { close('}'); }
    void begin_array() { open('['); }
    void end_array() { close(']'); }

    // Starts a member of the current object. The next value or container belongs to it.
    void key(std::string_view name) {
        separate();
        quote(name);
        text_ += ": ";
        after_key_ = true;
    }

    void value(std::string_view text) {
        separate();
        quote(text);
    }

    template <std::integral T>
        requires(!std::same_as<T, bool>)
    void value(T number) {
        separate();
        text_ += std::format("{}", number);
    }

    void value(bool flag) {
        separate();
        text_ += flag ? "true" : "false";
    }

    void value(double number) {
        separate();
        text_ += std::format("{:.6f}", number);
    }

    void null() {
        separate();
        text_ += "null";
    }

    template <class T>
    void field(std::string_view name, const T& item) {
        key(name);
        value(item);
    }

    template <class T>
    void field(std::string_view name, const std::optional<T>& item) {
        key(name);
        if (item.has_value()) {
            value(*item);
        } else {
            null();
        }
    }

    // The document so far, ending in a newline.
    [[nodiscard]] std::string finish() { return std::move(text_) + "\n"; }

private:
    void open(char bracket) {
        separate();
        text_ += bracket;
        ++depth_;
        first_in_container_ = true;
    }

    void close(char bracket) {
        --depth_;
        if (!first_in_container_) {
            line_break();
        }
        text_ += bracket;
        first_in_container_ = false;
    }

    // Everything before a value or key: a comma and a new line, unless a key or an
    // opening bracket has just set the place.
    void separate() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (depth_ == 0) {
            return;
        }
        if (!first_in_container_) {
            text_ += ',';
        }
        line_break();
        first_in_container_ = false;
    }

    void line_break() {
        text_ += '\n';
        text_.append(depth_ * 2, ' ');
    }

    // The length of the well-formed UTF-8 sequence starting at `at`, or 0 if none does
    // (Unicode's table 3-7: no overlong form, no surrogate, nothing past U+10FFFF).
    [[nodiscard]] static std::size_t utf8_length(std::string_view text, std::size_t at) noexcept {
        const auto byte = [&](std::size_t i) -> unsigned {
            return at + i < text.size() ? static_cast<unsigned char>(text[at + i]) : 0x100U;
        };
        const auto in = [](unsigned b, unsigned low, unsigned high) { return b >= low && b <= high; };
        const unsigned lead = byte(0);
        std::size_t length = 0;
        unsigned low = 0x80U;  // the second byte's range, which the lead narrows
        unsigned high = 0xBFU;
        if (lead < 0x80U) {
            return 1;
        }
        if (in(lead, 0xC2U, 0xDFU)) {
            length = 2;
        } else if (in(lead, 0xE0U, 0xEFU)) {
            length = 3;
            low = lead == 0xE0U ? 0xA0U : low;
            high = lead == 0xEDU ? 0x9FU : high;
        } else if (in(lead, 0xF0U, 0xF4U)) {
            length = 4;
            low = lead == 0xF0U ? 0x90U : low;
            high = lead == 0xF4U ? 0x8FU : high;
        } else {
            return 0;
        }
        if (!in(byte(1), low, high)) {
            return 0;
        }
        for (std::size_t i = 2; i < length; ++i) {
            if (!in(byte(i), 0x80U, 0xBFU)) {
                return 0;
            }
        }
        return length;
    }

    void quote(std::string_view text) {
        text_ += '"';
        for (std::size_t at = 0; at < text.size();) {
            const auto code = static_cast<unsigned char>(text[at]);
            std::size_t length = 1;
            if (code == '"') {
                text_ += "\\\"";
            } else if (code == '\\') {
                text_ += "\\\\";
            } else if (code < 0x20U) {
                text_ += std::format("\\u{:04x}", static_cast<unsigned>(code));
            } else if (code < 0x80U) {
                text_ += text[at];
            } else if (length = utf8_length(text, at); length != 0) {
                text_ += text.substr(at, length);
            } else {
                text_ += std::format("\\udc{:02x}", static_cast<unsigned>(code));
                length = 1;
            }
            at += length;
        }
        text_ += '"';
    }

    std::string text_;
    std::size_t depth_ = 0;
    bool first_in_container_ = false;
    bool after_key_ = false;
};

}  // namespace tick_store::harness
