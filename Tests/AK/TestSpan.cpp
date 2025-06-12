/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Checked.h>
#include <AK/Span.h>
#include <string.h>

TEST_CASE(constexpr_default_constructor_is_empty)
{
    constexpr Span<int> span;
    static_assert(span.is_empty(), "Default constructed span should be empty.");
}

TEST_CASE(conforms_to_iterator_protocol)
{
    static_assert(std::random_access_iterator<Span<int>::Iterator>);
    static_assert(std::random_access_iterator<Span<int>::ConstIterator>);
    static_assert(std::random_access_iterator<Span<int const>::Iterator>);
    static_assert(std::random_access_iterator<Span<int const>::ConstIterator>);

    static_assert(std::random_access_iterator<Bytes::Iterator>);
    static_assert(std::random_access_iterator<Bytes::ConstIterator>);
    static_assert(std::random_access_iterator<ReadonlyBytes::Iterator>);
    static_assert(std::random_access_iterator<ReadonlyBytes::ConstIterator>);
}

TEST_CASE(implicit_conversion_to_const)
{
    constexpr Bytes bytes0;
    [[maybe_unused]] constexpr ReadonlyBytes bytes2 = bytes0;
    [[maybe_unused]] constexpr ReadonlyBytes bytes3 = static_cast<ReadonlyBytes>(bytes2);
}

TEST_CASE(span_works_with_constant_types)
{
    static constexpr u8 buffer[4] { 1, 2, 3, 4 };
    constexpr ReadonlyBytes bytes { buffer, 4 };

    static_assert(IsConst<RemoveReference<decltype(bytes[1])>>);
    static_assert(bytes[2] == 3);
}

TEST_CASE(span_works_with_mutable_types)
{
    u8 buffer[4] { 1, 2, 3, 4 };
    Bytes bytes { buffer, 4 };

    EXPECT_EQ(bytes[2], 3);
    ++bytes[2];
    EXPECT_EQ(bytes[2], 4);
}

TEST_CASE(iterator_behaves_like_loop)
{
    u8 buffer[256];
    for (int idx = 0; idx < 256; ++idx) {
        buffer[idx] = static_cast<u8>(idx);
    }

    Bytes bytes { buffer, 256 };
    size_t idx = 0;
    for (auto iter = bytes.begin(); iter < bytes.end(); ++iter) {
        EXPECT_EQ(*iter, buffer[idx]);

        ++idx;
    }
}

TEST_CASE(modifying_is_possible)
{
    int values_before[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int values_after[8] = { 7, 6, 5, 4, 3, 2, 1, 0 };

    Span<int> span { values_before, 8 };
    for (auto& value : span) {
        value = 8 - value;
    }

    for (int idx = 0; idx < 8; ++idx) {
        EXPECT_EQ(values_before[idx], values_after[idx]);
    }
}

TEST_CASE(at_and_index_operator_return_same_value)
{
    u8 buffer[256];
    for (int idx = 0; idx < 256; ++idx) {
        buffer[idx] = static_cast<u8>(idx);
    }

    Bytes bytes { buffer, 256 };
    for (int idx = 0; idx < 256; ++idx) {
        EXPECT_EQ(buffer[idx], bytes[idx]);
        EXPECT_EQ(bytes[idx], bytes.at(idx));
    }
}

TEST_CASE(can_subspan_whole_span)
{
    static constexpr u8 buffer[16] {};
    constexpr ReadonlyBytes bytes { buffer, 16 };

    constexpr auto slice = bytes.slice(0, 16);

    static_assert(slice.data() == buffer);
    static_assert(slice.size() == 16u);
}

TEST_CASE(can_subspan_as_intended)
{
    static constexpr u16 buffer[8] { 1, 2, 3, 4, 5, 6, 7, 8 };

    constexpr ReadonlySpan<u16> span { buffer, 8 };
    constexpr auto slice = span.slice(3, 2);

    static_assert(slice.size() == 2u);
    static_assert(slice[0] == 4);
    static_assert(slice[1] == 5);
}

TEST_CASE(span_from_void_pointer)
{
    int value = 0;
    [[maybe_unused]] Bytes bytes0 { reinterpret_cast<void*>(value), 4 };
    [[maybe_unused]] ReadonlyBytes bytes1 { reinterpret_cast<void const*>(value), 4 };
}

TEST_CASE(span_from_c_string)
{
    char const* str = "Serenity";
    [[maybe_unused]] ReadonlyBytes bytes { str, strlen(str) };
}

TEST_CASE(starts_with)
{
    char const* str = "HeyFriends!";
    ReadonlyBytes bytes { str, strlen(str) };
    char const* str_hey = "Hey";
    ReadonlyBytes hey_bytes { str_hey, strlen(str_hey) };
    EXPECT(bytes.starts_with(hey_bytes));
    char const* str_nah = "Nah";
    ReadonlyBytes nah_bytes { str_nah, strlen(str_nah) };
    EXPECT(!bytes.starts_with(nah_bytes));

    u8 const hey_array[3] = { 'H', 'e', 'y' };
    ReadonlyBytes hey_bytes_u8 { hey_array, 3 };
    EXPECT(bytes.starts_with(hey_bytes_u8));
}

TEST_CASE(contains_slow)
{
    Vector<String> list { "abc"_string, "def"_string, "ghi"_string };
    auto span = list.span();

    EXPECT(span.contains_slow("abc"_string));
    EXPECT(span.contains_slow("abc"sv));

    EXPECT(span.contains_slow("def"_string));
    EXPECT(span.contains_slow("def"sv));

    EXPECT(span.contains_slow("ghi"_string));
    EXPECT(span.contains_slow("ghi"sv));

    EXPECT(!span.contains_slow("whf"_string));
    EXPECT(!span.contains_slow("whf"sv));

    EXPECT(!span.contains_slow(String {}));
    EXPECT(!span.contains_slow(StringView {}));
}

TEST_CASE(compare_different_constness)
{
    constexpr Array<int, 3> array { 4, 5, 6 };
    Vector<int> vector { 4, 5, 6 };

    EXPECT_EQ(array, vector.span());
}
