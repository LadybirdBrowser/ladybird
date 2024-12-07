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
    EXPECT("a"sv < ByteString("b"));
    EXPECT(!("a"sv > ByteString("b")));
    EXPECT("b"sv > ByteString("a"));
    EXPECT(!("b"sv < ByteString("b")));
    EXPECT("a"sv >= ByteString("a"));
    EXPECT(!("a"sv >= ByteString("b")));
    EXPECT("a"sv <= ByteString("a"));
    EXPECT(!("b"sv <= ByteString("a")));

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
    EXPECT(test_string.starts_with("AB"sv));
    EXPECT(test_string.starts_with('A'));
    EXPECT(!test_string.starts_with('B'));
    EXPECT(test_string.starts_with("ABCDEF"sv));
    EXPECT(!test_string.starts_with("DEF"sv));
    EXPECT(test_string.starts_with("abc"sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!test_string.starts_with("abc"sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(ends_with)
{
    ByteString test_string = "ABCDEF";
    EXPECT(test_string.ends_with("EF"sv));
    EXPECT(test_string.ends_with('F'));
    EXPECT(!test_string.ends_with('E'));
    EXPECT(test_string.ends_with("ABCDEF"sv));
    EXPECT(!test_string.ends_with("ABC"sv));
    EXPECT(test_string.ends_with("def"sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!test_string.ends_with("def"sv, CaseSensitivity::CaseSensitive));
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

    test_string = test_string.replace("Friends"sv, "Testers"sv, ReplaceMode::FirstOnly);
    EXPECT(test_string == "Well, hello Testers!");

    test_string = test_string.replace("ell"sv, "e're"sv, ReplaceMode::All);
    EXPECT(test_string == "We're, he'reo Testers!");

    test_string = test_string.replace("!"sv, " :^)"sv, ReplaceMode::FirstOnly);
    EXPECT(test_string == "We're, he'reo Testers :^)");

    test_string = ByteString("111._.111._.111");
    test_string = test_string.replace("111"sv, "|||"sv, ReplaceMode::All);
    EXPECT(test_string == "|||._.|||._.|||");

    test_string = test_string.replace("|||"sv, "111"sv, ReplaceMode::FirstOnly);
    EXPECT(test_string == "111._.|||._.|||");
}

TEST_CASE(count)
{
    ByteString test_string = "Well, hello Friends!";
    u32 count = test_string.count("Friends"sv);
    EXPECT(count == 1);

    count = test_string.count("ell"sv);
    EXPECT(count == 2);

    count = test_string.count("!"sv);
    EXPECT(count == 1);

    test_string = ByteString("111._.111._.111");
    count = test_string.count("111"sv);
    EXPECT(count == 3);

    count = test_string.count("._."sv);
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
    builder.append(""sv);
    auto built = builder.to_byte_string();
    EXPECT_EQ(built.length(), 0u);
}

TEST_CASE(find)
{
    ByteString a = "foobarbar";
    EXPECT_EQ(a.find("bar"sv), Optional<size_t> { 3 });
    EXPECT_EQ(a.find("baz"sv), Optional<size_t> {});
    EXPECT_EQ(a.find("bar"sv, 4), Optional<size_t> { 6 });
    EXPECT_EQ(a.find("bar"sv, 9), Optional<size_t> {});

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
    EXPECT_EQ(string.find(""sv), 0u);
    EXPECT_EQ(string.find_all(""sv), (Vector<size_t> { 0u }));

    string = "abc";
    EXPECT_EQ(string.find(""sv), 0u);
    EXPECT_EQ(string.find_all(""sv), (Vector<size_t> { 0u, 1u, 2u, 3u }));
}

TEST_CASE(bijective_base)
{
    EXPECT_EQ(ByteString::bijective_base_from(0), "A");
    EXPECT_EQ(ByteString::bijective_base_from(25), "Z");
    EXPECT_EQ(ByteString::bijective_base_from(26), "AA");
    EXPECT_EQ(ByteString::bijective_base_from(52), "BA");
    EXPECT_EQ(ByteString::bijective_base_from(701), "ZZ");
    EXPECT_EQ(ByteString::bijective_base_from(702), "AAA");
    EXPECT_EQ(ByteString::bijective_base_from(730), "ABC");
    EXPECT_EQ(ByteString::bijective_base_from(18277), "ZZZ");
}

TEST_CASE(roman_numerals)
{
    auto zero = ByteString::roman_number_from(0);
    EXPECT_EQ(zero, "");

    auto one = ByteString::roman_number_from(1);
    EXPECT_EQ(one, "I");

    auto nine = ByteString::roman_number_from(9);
    EXPECT_EQ(nine, "IX");

    auto fourty_eight = ByteString::roman_number_from(48);
    EXPECT_EQ(fourty_eight, "XLVIII");

    auto one_thousand_nine_hundred_ninety_eight = ByteString::roman_number_from(1998);
    EXPECT_EQ(one_thousand_nine_hundred_ninety_eight, "MCMXCVIII");

    auto four_thousand = ByteString::roman_number_from(4000);
    EXPECT_EQ(four_thousand, "4000");
}
