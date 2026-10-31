#pragma once

// A small JSON writer for the tools' summaries. Nothing here reads JSON.

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

    void quote(std::string_view text) {
        text_ += '"';
        for (const char c : text) {
            switch (c) {
                case '"':
                    text_ += "\\\"";
                    break;
                case '\\':
                    text_ += "\\\\";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20U) {
                        text_ += std::format("\\u{:04x}", static_cast<unsigned>(c));
                    } else {
                        text_ += c;
                    }
            }
        }
        text_ += '"';
    }

    std::string text_;
    std::size_t depth_ = 0;
    bool first_in_container_ = false;
    bool after_key_ = false;
};

}  // namespace tick_store::harness
