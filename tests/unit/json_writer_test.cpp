// The tools' JSON writer: what it does with bytes a string holds, since a store's type byte or
// a path can be any byte, and the Python callers read the output as UTF-8 text.

#include "tick_store/harness/json_writer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace tick_store::harness {
namespace {

// The JSON text of a one-string document, as `{"s": <text>}` with the writer's layout.
std::string quoted(std::string_view text) {
    JsonWriter json;
    json.begin_object();
    json.field("s", text);
    json.end_object();
    return json.finish();
}

std::string expect(std::string_view inside) { return "{\n  \"s\": \"" + std::string{inside} + "\"\n}\n"; }

TEST(JsonWriter, EscapesQuotesBackslashesAndControlBytes) {
    EXPECT_EQ(quoted("a\"b\\c\n\x01"), expect(R"(a\"b\\c\u000a\u0001)"));
}

// Text that is already UTF-8, a path with non-ASCII names among it, goes out unchanged, so a
// reader decoding UTF-8 gets the same characters back.
TEST(JsonWriter, KeepsValidUtf8AsItIs) {
    for (const std::string_view text :
         {"caf\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC/\xC3\xBC", "\xF0\x9D\x84\x9E", "\xC2\x80",
          "\xDF\xBF", "\xE0\xA0\x80", "\xED\x9F\xBF", "\xEE\x80\x80", "\xEF\xBF\xBF",
          "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF", "/data/r\xC3\xA9sum\xC3\xA9/store.tsc"}) {
        EXPECT_EQ(quoted(text), expect(text)) << text;
    }
}

// A byte that is not part of a well-formed UTF-8 sequence is written as \udcXX, the lone low
// surrogate Python's surrogateescape gives that byte: json.loads then holds the same string
// os.listdir would, and os.fsencode gives the byte back. The output is ASCII there, so the
// document stays valid UTF-8 whatever the input held.
TEST(JsonWriter, EscapesEachByteOutsideWellFormedUtf8AsASurrogate) {
    EXPECT_EQ(quoted("\xC3"), expect(R"(\udcc3)"));                   // a lead byte at the end
    EXPECT_EQ(quoted("\x80"), expect(R"(\udc80)"));                   // a lone continuation
    EXPECT_EQ(quoted("\xFF\xFE"), expect(R"(\udcff\udcfe)"));         // never in UTF-8
    EXPECT_EQ(quoted("\xC0\x80"), expect(R"(\udcc0\udc80)"));         // overlong NUL
    EXPECT_EQ(quoted("\xC1\xBF"), expect(R"(\udcc1\udcbf)"));         // overlong
    EXPECT_EQ(quoted("\xE0\x9F\xBF"), expect(R"(\udce0\udc9f\udcbf)"));        // overlong
    EXPECT_EQ(quoted("\xED\xA0\x80"), expect(R"(\udced\udca0\udc80)"));        // a surrogate
    EXPECT_EQ(quoted("\xF0\x8F\xBF\xBF"), expect(R"(\udcf0\udc8f\udcbf\udcbf)"));  // overlong
    EXPECT_EQ(quoted("\xF4\x90\x80\x80"), expect(R"(\udcf4\udc90\udc80\udc80)"));  // past U+10FFFF
    EXPECT_EQ(quoted("\xF5\x80\x80\x80"), expect(R"(\udcf5\udc80\udc80\udc80)"));
    EXPECT_EQ(quoted("\xE2\x82" "A"), expect(R"(\udce2\udc82A)"));    // cut short by ASCII
    EXPECT_EQ(quoted("\xE2\x82\xAC\xE2\x82"), expect("\xE2\x82\xAC" R"(\udce2\udc82)"));
    EXPECT_EQ(quoted("caf\xE9/\xC3\xA9"), expect("caf" R"(\udce9)" "/\xC3\xA9"));  // Latin-1 é
}

}  // namespace
}  // namespace tick_store::harness
