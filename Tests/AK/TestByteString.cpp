/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <cstring>

TEST_CASE(construct_empty)
{
    EXPECT(ByteString().is_empty());
    EXPECT(ByteString().characters() != nullptr);

    EXPECT(ByteString("").is_empty());
    EXPECT(ByteString("").characters() != nullptr);

    EXPECT(ByteString("").impl() == ByteString::empty().impl());
}

TEST_CASE(construct_contents)
{
    ByteString test_string = "ABCDEF";
    EXPECT(!test_string.is_empty());
    EXPECT_EQ(test_string.length(), 6u);
    EXPECT_EQ(test_string.length(), strlen(test_string.characters()));
    EXPECT(test_string.characters() != nullptr);
    EXPECT(!strcmp(test_string.characters(), "ABCDEF"));

    EXPECT(test_string == "ABCDEF");
    EXPECT(test_string != "ABCDE");
    EXPECT(test_string != "ABCDEFG");
}

TEST_CASE(equal)
{
    EXPECT_EQ(ByteString::empty(), ByteString {});
}

TEST_CASE(compare)
{
    EXPECT("a"_sv < ByteString("b"));
    EXPECT(!("a"_sv > ByteString("b")));
    EXPECT("b"_sv > ByteString("a"));
    EXPECT(!("b"_sv < ByteString("b")));
    EXPECT("a"_sv >= ByteString("a"));
    EXPECT(!("a"_sv >= ByteString("b")));
    EXPECT("a"_sv <= ByteString("a"));
    EXPECT(!("b"_sv <= ByteString("a")));

    EXPECT(ByteString("a") > ByteString());
    EXPECT(!(ByteString() > ByteString("a")));
    EXPECT(ByteString() < ByteString("a"));
    EXPECT(!(ByteString("a") < ByteString()));
    EXPECT(ByteString("a") >= ByteString());
    EXPECT(!(ByteString() >= ByteString("a")));
    EXPECT(ByteString() <= ByteString("a"));
    EXPECT(!(ByteString("a") <= ByteString()));

    EXPECT(!(ByteString() > ByteString()));
    EXPECT(!(ByteString() < ByteString()));
    EXPECT(ByteString() >= ByteString());
    EXPECT(ByteString() <= ByteString());
}

TEST_CASE(index_access)
{
    ByteString test_string = "ABCDEF";
    EXPECT_EQ(test_string[0], 'A');
    EXPECT_EQ(test_string[1], 'B');
}

TEST_CASE(starts_with)
{
    ByteString test_string = "ABCDEF";
    EXPECT(test_string.starts_with("AB"_sv));
    EXPECT(test_string.starts_with('A'));
    EXPECT(!test_string.starts_with('B'));
    EXPECT(test_string.starts_with("ABCDEF"_sv));
    EXPECT(!test_string.starts_with("DEF"_sv));
    EXPECT(test_string.starts_with("abc"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!test_string.starts_with("abc"_sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(ends_with)
{
    ByteString test_string = "ABCDEF";
    EXPECT(test_string.ends_with("EF"_sv));
    EXPECT(test_string.ends_with('F'));
    EXPECT(!test_string.ends_with('E'));
    EXPECT(test_string.ends_with("ABCDEF"_sv));
    EXPECT(!test_string.ends_with("ABC"_sv));
    EXPECT(test_string.ends_with("def"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!test_string.ends_with("def"_sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(copy_string)
{
    ByteString test_string = "ABCDEF";
    auto test_string_copy = test_string;
    EXPECT_EQ(test_string, test_string_copy);
    EXPECT_EQ(test_string.characters(), test_string_copy.characters());
}

TEST_CASE(move_string)
{
    ByteString test_string = "ABCDEF";
    auto test_string_copy = test_string;
    auto test_string_move = move(test_string_copy);
    EXPECT_EQ(test_string, test_string_move);
    EXPECT(test_string_copy.is_empty());
}

TEST_CASE(repeated)
{
    EXPECT_EQ(ByteString::repeated('x', 0), "");
    EXPECT_EQ(ByteString::repeated('x', 1), "x");
    EXPECT_EQ(ByteString::repeated('x', 2), "xx");
}

TEST_CASE(to_int)
{
    EXPECT_EQ(ByteString("123").to_number<int>().value(), 123);
    EXPECT_EQ(ByteString("-123").to_number<int>().value(), -123);
}

TEST_CASE(to_lowercase)
{
    EXPECT(ByteString("ABC").to_lowercase() == "abc");
}

TEST_CASE(to_uppercase)
{
    EXPECT(ByteString("AbC").to_uppercase() == "ABC");
}

TEST_CASE(replace)
{
    ByteString test_string = "Well, hello Friends!";

    test_string = test_string.replace("Friends"_sv, "Testers"_sv, ReplaceMode::FirstOnly);
    EXPECT(test_string == "Well, hello Testers!");

    test_string = test_string.replace("ell"_sv, "e're"_sv, ReplaceMode::All);
    EXPECT(test_string == "We're, he'reo Testers!");

    test_string = test_string.replace("!"_sv, " :^)"_sv, ReplaceMode::FirstOnly);
    EXPECT(test_string == "We're, he'reo Testers :^)");

    test_string = ByteString("111._.111._.111");
    test_string = test_string.replace("111"_sv, "|||"_sv, ReplaceMode::All);
    EXPECT(test_string == "|||._.|||._.|||");

    test_string = test_string.replace("|||"_sv, "111"_sv, ReplaceMode::FirstOnly);
    EXPECT(test_string == "111._.|||._.|||");
}

TEST_CASE(count)
{
    ByteString test_string = "Well, hello Friends!";
    u32 count = test_string.count("Friends"_sv);
    EXPECT(count == 1);

    count = test_string.count("ell"_sv);
    EXPECT(count == 2);

    count = test_string.count("!"_sv);
    EXPECT(count == 1);

    test_string = ByteString("111._.111._.111");
    count = test_string.count("111"_sv);
    EXPECT(count == 3);

    count = test_string.count("._."_sv);
    EXPECT(count == 2);
}

TEST_CASE(substring)
{
    ByteString test = "abcdef";
    EXPECT_EQ(test.substring(0, 6), test);
    EXPECT_EQ(test.substring(0, 3), "abc");
    EXPECT_EQ(test.substring(3, 3), "def");
    EXPECT_EQ(test.substring(3, 0), "");
    EXPECT_EQ(test.substring(6, 0), "");
}

TEST_CASE(split)
{
    ByteString test = "foo bar baz";
    auto parts = test.split(' ');
    EXPECT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "foo");
    EXPECT_EQ(parts[1], "bar");
    EXPECT_EQ(parts[2], "baz");

    EXPECT_EQ(parts[0].characters()[3], '\0');
    EXPECT_EQ(parts[1].characters()[3], '\0');
    EXPECT_EQ(parts[2].characters()[3], '\0');

    test = "a    b";

    parts = test.split(' ');
    EXPECT_EQ(parts.size(), 2u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");

    parts = test.split(' ', SplitBehavior::KeepEmpty);
    EXPECT_EQ(parts.size(), 5u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "");
    EXPECT_EQ(parts[2], "");
    EXPECT_EQ(parts[3], "");
    EXPECT_EQ(parts[4], "b");

    test = "axxbx";
    EXPECT_EQ(test.split('x').size(), 2u);
    EXPECT_EQ(test.split('x', SplitBehavior::KeepEmpty).size(), 4u);
    EXPECT_EQ(test.split_view('x').size(), 2u);
    EXPECT_EQ(test.split_view('x', SplitBehavior::KeepEmpty).size(), 4u);
}

TEST_CASE(builder_zero_initial_capacity)
{
    StringBuilder builder(0);
    builder.append(""_sv);
    auto built = builder.to_byte_string();
    EXPECT_EQ(built.length(), 0u);
}

TEST_CASE(find)
{
    ByteString a = "foobarbar";
    EXPECT_EQ(a.find("bar"_sv), Optional<size_t> { 3 });
    EXPECT_EQ(a.find("baz"_sv), Optional<size_t> {});
    EXPECT_EQ(a.find("bar"_sv, 4), Optional<size_t> { 6 });
    EXPECT_EQ(a.find("bar"_sv, 9), Optional<size_t> {});

    EXPECT_EQ(a.find('f'), Optional<size_t> { 0 });
    EXPECT_EQ(a.find('x'), Optional<size_t> {});
    EXPECT_EQ(a.find('f', 1), Optional<size_t> {});
    EXPECT_EQ(a.find('b'), Optional<size_t> { 3 });
    EXPECT_EQ(a.find('b', 4), Optional<size_t> { 6 });
    EXPECT_EQ(a.find('b', 9), Optional<size_t> {});
}

TEST_CASE(find_with_empty_needle)
{
    ByteString string = "";
    EXPECT_EQ(string.find(""_sv), 0u);
    EXPECT_EQ(string.find_all(""_sv), (Vector<size_t> { 0u }));

    string = "abc";
    EXPECT_EQ(string.find(""_sv), 0u);
    EXPECT_EQ(string.find_all(""_sv), (Vector<size_t> { 0u, 1u, 2u, 3u }));
}
