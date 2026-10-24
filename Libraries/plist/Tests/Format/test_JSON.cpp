/**
 Copyright (c) 2015-present, Facebook, Inc.
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <plist/Format/JSON.h>
#include <plist/Objects.h>

using plist::Format::JSON;
using plist::Format::Encoding;
using plist::String;
using plist::Boolean;
using plist::Integer;
using plist::Real;
using plist::Dictionary;
using plist::Array;

static std::vector<uint8_t>
Contents(std::string const &string)
{
    return std::vector<uint8_t>(string.begin(), string.end());
}

TEST(JSON, Serialize)
{
    auto dictionary = Dictionary::New();
    dictionary->set("boolean", Boolean::New(true));
    dictionary->set("integer", Integer::New(42));
    dictionary->set("real", Real::New(3.14));

    auto serialize = JSON::Serialize(dictionary.get(), JSON::Create());
    ASSERT_NE(serialize.first, nullptr);
    EXPECT_EQ(*serialize.first, Contents("{\n\t\"boolean\": true,\n\t\"integer\": 42,\n\t\"real\": 3.14\n}"));
}

TEST(JSON, SerializeCollections)
{
    auto dict = Dictionary::New();
    dict->set("one", String::New("1"));
    dict->set("two", Integer::New(1));

    auto array = Array::New();
    array->append(String::New("test"));
    array->append(Integer::New(99));

    auto dictionary = Dictionary::New();
    dictionary->set("dict", std::move(dict));
    dictionary->set("array", std::move(array));

    auto serialize = JSON::Serialize(dictionary.get(), JSON::Create());
    ASSERT_NE(serialize.first, nullptr);
    EXPECT_EQ(*serialize.first, Contents("{\n\t\"dict\": {\n\t\t\"one\": \"1\",\n\t\t\"two\": 1\n\t},\n\t\"array\": [\n\t\t\"test\",\n\t\t99\n\t]\n}"));
}

TEST(JSON, String)
{
    auto contents = Contents("\"str*ng\"");

    auto deserialize = JSON::Deserialize(contents, JSON::Create());
    ASSERT_NE(deserialize.first, nullptr);

    auto string = String::New("str*ng");
    EXPECT_TRUE(deserialize.first->equals(string.get()));

    auto serialize = JSON::Serialize(deserialize.first.get(), JSON::Create());
    ASSERT_NE(serialize.first, nullptr);
    EXPECT_EQ(*serialize.first, contents);
}

TEST(JSON, SerializeEscapedStrings)
{
    struct EscapeCase {
        char const *name;
        std::string value;
        char const *json;
        char const *alternative;
    };

    EscapeCase const cases[] = {
        /* Control characters must be escaped, including embedded NUL. */
        { "newline",         "a\nb", R"("a\nb")", R"("a\u000ab")" },
        { "carriage return", "a\rb", R"("a\rb")", R"("a\u000db")" },
        { "tab",             "a\tb", R"("a\tb")", R"("a\u0009b")" },
        { "backspace",       "a\bb", R"("a\bb")", R"("a\u0008b")" },
        { "form feed",       "a\fb", R"("a\fb")", R"("a\u000cb")" },
        { "NUL", std::string("a\0b", 3), R"("a\u0000b")", nullptr },
        { "unit separator",  "a\x1f" "b", R"("a\u001fb")", nullptr },

        /* Quotes and backslashes must preserve the original string. */
        { "quote",     "a\"b",  R"("a\"b")", nullptr },
        { "backslash", "a\\b", R"("a\\b")", nullptr },

        /* UTF-8 bytes must not be mistaken for control characters. */
        { "UTF-8", u8"café ☃ 😀", R"("café ☃ 😀")", R"("caf\u00e9 \u2603 \ud83d\ude00")" },
    };

    auto document = [](std::string const &json) {
        return "{\n\t" + json + ": " + json + "\n}";
    };

    for (auto const &test : cases) {
        SCOPED_TRACE(test.name);

        /* Exercise the same string as both a dictionary key and a value. */
        auto dictionary = Dictionary::New();
        dictionary->set(test.value, String::New(test.value));

        auto serialize = JSON::Serialize(dictionary.get(), JSON::Create());
        ASSERT_NE(serialize.first, nullptr);
        std::string actual(serialize.first->begin(), serialize.first->end());

        /* JSON permits both short escapes and Unicode escapes. */
        if (test.alternative != nullptr && actual == document(test.alternative)) {
            continue;
        }

        EXPECT_EQ(actual, document(test.json));
    }
}

TEST(JSON, BooleanNumber)
{
    auto contents = Contents("{\n\t\"boolean\": true,\n\t\"integer\": 42,\n\t\"real\": 3.14\n}");

    auto deserialize = JSON::Deserialize(contents, JSON::Create());
    ASSERT_NE(deserialize.first, nullptr);

    auto dictionary = Dictionary::New();
    dictionary->set("boolean", Boolean::New(true));
    dictionary->set("integer", Integer::New(42));
    dictionary->set("real", Real::New(3.14));
    EXPECT_TRUE(deserialize.first->equals(dictionary.get()));

    auto serialize = JSON::Serialize(dictionary.get(), JSON::Create());
    ASSERT_NE(serialize.first, nullptr);
    EXPECT_EQ(*serialize.first, contents);
}

TEST(JSON, Empty)
{
    auto contents = Contents("\n\n");

    auto deserialize = JSON::Deserialize(contents, JSON::Create());
    ASSERT_EQ(deserialize.first, nullptr);
}

TEST(JSON, SingleQuotes)
{
    /* Single quotes are not allowed in JSON. */
    auto contents = Contents("{ 'one': 1 }");
    auto deserialize = JSON::Deserialize(contents, JSON::Create());
    EXPECT_EQ(deserialize.first, nullptr);
}

TEST(JSON, Number)
{
    /* Numbers can be zero. */
    auto contents1 = Contents("{ \"key\" : 0 }");
    auto deserialize1 = JSON::Deserialize(contents1, JSON::Create());
    EXPECT_NE(deserialize1.first, nullptr);

    /* Numbers cannot start with zero. */
    auto contents2 = Contents("{ \"key\" : 01 }");
    auto deserialize2 = JSON::Deserialize(contents2, JSON::Create());
    EXPECT_EQ(deserialize2.first, nullptr);

    /* Numbers cannot have a positive sign. */
    auto contents3 = Contents("{ \"key\" : +1 }");
    auto deserialize3 = JSON::Deserialize(contents3, JSON::Create());
    EXPECT_EQ(deserialize3.first, nullptr);

    /* Numbers can have a negative sign. */
    auto contents4 = Contents("{ \"key\" : -1 }");
    auto deserialize4 = JSON::Deserialize(contents4, JSON::Create());
    EXPECT_NE(deserialize4.first, nullptr);

    /* Decimals must have a value. */
    auto contents5 = Contents("{ \"key\" : 1. }");
    auto deserialize5 = JSON::Deserialize(contents5, JSON::Create());
    EXPECT_EQ(deserialize5.first, nullptr);

    /* Exponents must have a value. */
    auto contents6 = Contents("{ \"key\" : 1e+ }");
    auto deserialize6 = JSON::Deserialize(contents6, JSON::Create());
    EXPECT_EQ(deserialize6.first, nullptr);
}
