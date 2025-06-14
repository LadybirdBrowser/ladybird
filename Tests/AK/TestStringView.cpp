/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/Vector.h>

TEST_CASE(construct_empty)
{
    EXPECT(StringView().is_null());
    EXPECT(StringView().is_empty());
    EXPECT(!StringView().characters_without_null_termination());
    EXPECT_EQ(StringView().length(), 0u);
}

TEST_CASE(view_literal)
{
    char const* truth = "cats rule dogs drool";
    StringView view { truth, strlen(truth) };
    EXPECT_EQ(view.is_null(), false);
    EXPECT_EQ(view.characters_without_null_termination(), truth);
    EXPECT_EQ(view, view);
    EXPECT_EQ(view, truth);
}

TEST_CASE(compare_views)
{
    ByteString foo1 = "foo";
    ByteString foo2 = "foo";
    auto view1 = foo1.view();
    auto view2 = foo2.view();

    EXPECT_EQ(view1, view2);
    EXPECT_EQ(view1, foo1);
    EXPECT_EQ(view1, foo2);
    EXPECT_EQ(view1, "foo");

    ByteString empty = "";
    auto empty_view = view1.substring_view(0, 0);
    StringView default_view = {};
    EXPECT_EQ(empty.view(), ""_sv);
    EXPECT_EQ(empty_view, ""_sv);
    EXPECT_EQ(default_view, ""_sv);
}

TEST_CASE(conforms_to_iterator_protocol)
{
    static_assert(std::random_access_iterator<StringView::ConstIterator>);
}

TEST_CASE(string_view_literal_operator)
{
    StringView literal_view = "foo"_sv;
    ByteString test_string = "foo";

    EXPECT_EQ(literal_view.length(), test_string.length());
    EXPECT_EQ(literal_view, test_string);
}

TEST_CASE(starts_with)
{
    ByteString test_string = "ABCDEF";
    StringView test_string_view = test_string.view();
    EXPECT(test_string_view.starts_with('A'));
    EXPECT(!test_string_view.starts_with('B'));
    EXPECT(test_string_view.starts_with("AB"_sv));
    EXPECT(test_string_view.starts_with("ABCDEF"_sv));
    EXPECT(!test_string_view.starts_with("DEF"_sv));
    EXPECT(test_string_view.starts_with("abc"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!test_string_view.starts_with("abc"_sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(ends_with)
{
    ByteString test_string = "ABCDEF";
    StringView test_string_view = test_string.view();
    EXPECT(test_string_view.ends_with("DEF"_sv));
    EXPECT(test_string_view.ends_with('F'));
    EXPECT(!test_string_view.ends_with('E'));
    EXPECT(test_string_view.ends_with("ABCDEF"_sv));
    EXPECT(!test_string_view.ends_with("ABCDE"_sv));
    EXPECT(!test_string_view.ends_with("ABCDEFG"_sv));
    EXPECT(test_string_view.ends_with("def"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!test_string_view.ends_with("def"_sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(lines)
{
    ByteString test_string = "a\rb\nc\r\nd";
    StringView test_string_view = test_string.view();
    Vector<StringView> test_string_vector = test_string_view.lines();
    EXPECT_EQ(test_string_vector.size(), 4u);
    EXPECT(test_string_vector.at(0) == ByteString("a"));
    EXPECT(test_string_vector.at(1) == ByteString("b"));
    EXPECT(test_string_vector.at(2) == ByteString("c"));
    EXPECT(test_string_vector.at(3) == ByteString("d"));

    test_string = "```\nHello there\r\nHello there\n```";
    test_string_view = test_string.view();
    test_string_vector = test_string_view.lines();
    EXPECT_EQ(test_string_vector.size(), 4u);
    EXPECT(test_string_vector.at(0) == ByteString("```"));
    EXPECT(test_string_vector.at(1) == ByteString("Hello there"));
    EXPECT(test_string_vector.at(2) == ByteString("Hello there"));
    EXPECT(test_string_vector.at(3) == ByteString("```"));

    test_string = "\n\n\n";
    test_string_view = test_string.view();
    test_string_vector = test_string_view.lines();
    EXPECT_EQ(test_string_vector.size(), 3u);
    EXPECT_EQ(test_string_vector.at(0).is_empty(), true);
    EXPECT_EQ(test_string_vector.at(1).is_empty(), true);
    EXPECT_EQ(test_string_vector.at(2).is_empty(), true);
}

TEST_CASE(count_lines)
{
    EXPECT_EQ(""_sv.count_lines(), 1u);
    EXPECT_EQ("foo"_sv.count_lines(), 1u);

    EXPECT_EQ("foo\nbar"_sv.count_lines(), 2u);
    EXPECT_EQ("foo\rbar"_sv.count_lines(), 2u);
    EXPECT_EQ("foo\rbar"_sv.count_lines(StringView::ConsiderCarriageReturn::No), 1u);
    EXPECT_EQ("foo\r\nbar"_sv.count_lines(), 2u);
    EXPECT_EQ("foo\r\nbar"_sv.count_lines(StringView::ConsiderCarriageReturn::No), 2u);

    EXPECT_EQ("foo\nbar\nbax"_sv.count_lines(), 3u);
    EXPECT_EQ("foo\rbar\rbaz"_sv.count_lines(), 3u);
    EXPECT_EQ("foo\rbar\rbaz"_sv.count_lines(StringView::ConsiderCarriageReturn::No), 1u);
    EXPECT_EQ("foo\r\nbar\r\nbaz"_sv.count_lines(), 3u);
    EXPECT_EQ("foo\r\nbar\r\nbaz"_sv.count_lines(StringView::ConsiderCarriageReturn::No), 3u);
}

TEST_CASE(find)
{
    auto test_string_view = "aabbcc_xy_ccbbaa"_sv;
    EXPECT_EQ(test_string_view.find('b'), 2U);
    EXPECT_EQ(test_string_view.find('_'), 6U);
    EXPECT_EQ(test_string_view.find('n').has_value(), false);
}

TEST_CASE(find_last)
{
    auto test_string_view = "aabbcc_xy_ccbbaa"_sv;
    EXPECT_EQ(test_string_view.find_last('b'), 13U);
    EXPECT_EQ(test_string_view.find_last('_'), 9U);
    EXPECT_EQ(test_string_view.find_last('3').has_value(), false);

    test_string_view = "/"_sv;
    EXPECT_EQ(test_string_view.find_last('/'), 0U);
}

TEST_CASE(find_any_of)
{
    auto test_string_view = "aabbcc_xy_ccbbaa"_sv;
    EXPECT_EQ(test_string_view.find_any_of("bc"_sv, StringView::SearchDirection::Forward), 2U);
    EXPECT_EQ(test_string_view.find_any_of("yx"_sv, StringView::SearchDirection::Forward), 7U);
    EXPECT_EQ(test_string_view.find_any_of("defg"_sv, StringView::SearchDirection::Forward).has_value(), false);
    EXPECT_EQ(test_string_view.find_any_of("bc"_sv, StringView::SearchDirection::Backward), 13U);
    EXPECT_EQ(test_string_view.find_any_of("yx"_sv, StringView::SearchDirection::Backward), 8U);
    EXPECT_EQ(test_string_view.find_any_of("fghi"_sv, StringView::SearchDirection::Backward).has_value(), false);

    test_string_view = "/"_sv;
    EXPECT_EQ(test_string_view.find_any_of("/"_sv, StringView::SearchDirection::Forward), 0U);
    EXPECT_EQ(test_string_view.find_any_of("/"_sv, StringView::SearchDirection::Backward), 0U);
}

TEST_CASE(split_view)
{
    StringView test_string_view = "axxbxcxd"_sv;
    EXPECT_EQ(test_string_view.split_view('x'), Vector({ "a"_sv, "b"_sv, "c"_sv, "d"_sv }));
    EXPECT_EQ(test_string_view.split_view('x', SplitBehavior::KeepEmpty), Vector({ "a"_sv, ""_sv, "b"_sv, "c"_sv, "d"_sv }));
    EXPECT_EQ(test_string_view.split_view("x"_sv), Vector({ "a"_sv, "b"_sv, "c"_sv, "d"_sv }));
    EXPECT_EQ(test_string_view.split_view("x"_sv, SplitBehavior::KeepEmpty), Vector({ "a"_sv, ""_sv, "b"_sv, "c"_sv, "d"_sv }));

    test_string_view = "axxbx"_sv;
    EXPECT_EQ(test_string_view.split_view('x'), Vector({ "a"_sv, "b"_sv }));
    EXPECT_EQ(test_string_view.split_view('x', SplitBehavior::KeepEmpty), Vector({ "a"_sv, ""_sv, "b"_sv, ""_sv }));
    EXPECT_EQ(test_string_view.split_view("x"_sv), Vector({ "a"_sv, "b"_sv }));
    EXPECT_EQ(test_string_view.split_view("x"_sv, SplitBehavior::KeepEmpty), Vector({ "a"_sv, ""_sv, "b"_sv, ""_sv }));

    test_string_view = "axxbcxxdxx"_sv;
    EXPECT_EQ(test_string_view.split_view("xx"_sv), Vector({ "a"_sv, "bc"_sv, "d"_sv }));
    EXPECT_EQ(test_string_view.split_view("xx"_sv, SplitBehavior::KeepEmpty), Vector({ "a"_sv, "bc"_sv, "d"_sv, ""_sv }));

    test_string_view = "ax_b_cxd"_sv;
    Function<bool(char)> predicate = [](char ch) { return ch == 'x' || ch == '_'; };
    EXPECT_EQ(test_string_view.split_view_if(predicate), Vector({ "a"_sv, "b"_sv, "c"_sv, "d"_sv }));
    EXPECT_EQ(test_string_view.split_view_if(predicate, SplitBehavior::KeepEmpty), Vector({ "a"_sv, ""_sv, "b"_sv, "c"_sv, "d"_sv }));

    test_string_view = "a,,,b"_sv;
    EXPECT_EQ(test_string_view.split_view(","_sv, SplitBehavior::KeepEmpty), Vector({ "a"_sv, ""_sv, ""_sv, "b"_sv }));
    EXPECT_EQ(test_string_view.split_view(","_sv, SplitBehavior::KeepTrailingSeparator), Vector({ "a,"_sv, "b"_sv }));
    EXPECT_EQ(test_string_view.split_view(","_sv, SplitBehavior::KeepTrailingSeparator | SplitBehavior::KeepEmpty), Vector({ "a,"_sv, ","_sv, ","_sv, "b"_sv }));
}

TEST_CASE(constexpr_stuff)
{
#define do_test()                                                          \
    static_assert(test_constexpr.length() == 3);                           \
    static_assert(!test_constexpr.is_empty());                             \
    static_assert(test_constexpr.is_one_of("foo", "bar", "baz"));          \
    static_assert(test_constexpr.is_one_of("foo"_sv, "bar"_sv, "baz"_sv)); \
    static_assert(test_constexpr != "fob"_sv);                             \
    static_assert(test_constexpr != "fob");                                \
    static_assert(test_constexpr.substring_view(1).is_one_of("oo"_sv));

    {
        // Can initialize from ""_sv.
        constexpr StringView test_constexpr { "foo"_sv };
        do_test();
    }
#undef do_test
}

TEST_CASE(case_insensitive_hash)
{
    auto string1 = "abcdef"_sv;
    auto string2 = "ABCDEF"_sv;
    auto string3 = "aBcDeF"_sv;
    auto string4 = "foo"_sv;

    EXPECT_EQ(CaseInsensitiveASCIIStringViewTraits::hash(string1), CaseInsensitiveASCIIStringViewTraits::hash(string2));
    EXPECT_EQ(CaseInsensitiveASCIIStringViewTraits::hash(string1), CaseInsensitiveASCIIStringViewTraits::hash(string3));
    EXPECT_NE(CaseInsensitiveASCIIStringViewTraits::hash(string1), CaseInsensitiveASCIIStringViewTraits::hash(string4));
}
