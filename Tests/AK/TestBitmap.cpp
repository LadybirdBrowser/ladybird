/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Bitmap.h>
#include <AK/FixedBitmap.h>

using namespace Test::Randomized;

TEST_CASE(fixed_bitmap_copies_packed_bytes)
{
    Array<u8, 2> bytes { 0b10000001, 0b00000010 };
    AK::FixedBitmap<10> bitmap { false };
    bitmap.copy_from(bytes.span());

    EXPECT(bitmap.get(0));
    EXPECT(bitmap.get(7));
    EXPECT(bitmap.get(9));
    EXPECT_EQ(bitmap.bytes(), bytes.span());
}

TEST_CASE(construct_empty)
{
    Bitmap bitmap;
    EXPECT_EQ(bitmap.size(), 0u);
}

TEST_CASE(find_first_set)
{
    auto bitmap = MUST(Bitmap::create(128, false));
    bitmap.set(69, true);
    EXPECT_EQ(bitmap.find_first_set().value(), 69u);
}

TEST_CASE(find_first_unset)
{
    auto bitmap = MUST(Bitmap::create(128, true));
    bitmap.set(51, false);
    EXPECT_EQ(bitmap.find_first_unset().value(), 51u);
}

TEST_CASE(find_functions_match_only_logical_bits)
{
    Array<u8, sizeof(size_t) + 1> storage {};

    for (size_t offset = 0; offset < sizeof(size_t); ++offset) {
        for (size_t size = 1; size <= 8; ++size) {
            for (size_t pattern = 0; pattern <= 0xff; ++pattern) {
                storage[offset] = static_cast<u8>(pattern);
                BitmapView bitmap { &storage[offset], size };

                for (bool value : { false, true }) {
                    Optional<size_t> expected;
                    for (size_t i = 0; i < size; ++i) {
                        if (bitmap.get(i) == value) {
                            expected = i;
                            break;
                        }
                    }

                    auto first = value ? bitmap.find_first_set() : bitmap.find_first_unset();
                    EXPECT_EQ(first, expected);

                    for (size_t hint = 0; hint < size; ++hint) {
                        auto found = value ? bitmap.find_one_anywhere_set(hint) : bitmap.find_one_anywhere_unset(hint);
                        EXPECT_EQ(found.has_value(), expected.has_value());
                        if (found.has_value()) {
                            EXPECT(*found < size);
                            EXPECT_EQ(bitmap.get(*found), value);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE(find_one_anywhere_is_independent_of_storage_alignment)
{
    constexpr auto bits_per_word = sizeof(size_t) * 8;
    constexpr auto size = bits_per_word * 2 + 5;
    Array<u8, sizeof(size_t) * 3> storage {};

    for (size_t offset = 0; offset < sizeof(size_t); ++offset) {
        storage.fill(offset % 2 == 0 ? 0x00 : 0xff);
        BitmapView bitmap { &storage[offset], size };
        for (size_t i = 0; i < size; ++i) {
            auto value = i % 17 == 3 || i % 19 == 7;
            if (value)
                storage[offset + i / 8] |= static_cast<u8>(1u << (i % 8));
            else
                storage[offset + i / 8] &= static_cast<u8>(~(1u << (i % 8)));
        }

        for (bool value : { false, true }) {
            for (size_t hint = 0; hint < size; ++hint) {
                auto search_start = hint / bits_per_word * bits_per_word;
                Optional<size_t> expected;
                for (size_t i = search_start; i < size; ++i) {
                    if (bitmap.get(i) == value) {
                        expected = i;
                        break;
                    }
                }
                if (!expected.has_value()) {
                    for (size_t i = 0; i < search_start; ++i) {
                        if (bitmap.get(i) == value) {
                            expected = i;
                            break;
                        }
                    }
                }

                auto found = value ? bitmap.find_one_anywhere_set(hint) : bitmap.find_one_anywhere_unset(hint);
                EXPECT_EQ(found, expected);
            }
        }
    }
}

TEST_CASE(find_functions_handle_partial_words)
{
    auto maximum_size = sizeof(size_t) * 8 * 2 + 1;
    for (size_t size = 1; size <= maximum_size; ++size) {
        auto set_bitmap = MUST(Bitmap::create(size, false));
        for (size_t i = 0; i < size; ++i)
            set_bitmap.set(i, true);
        EXPECT(!set_bitmap.find_first_unset().has_value());
        for (size_t hint = 0; hint < size; ++hint)
            EXPECT(!set_bitmap.find_one_anywhere_unset(hint).has_value());

        set_bitmap.set(size - 1, false);
        EXPECT_EQ(set_bitmap.find_first_unset(), size - 1);
        for (size_t hint = 0; hint < size; ++hint)
            EXPECT_EQ(set_bitmap.find_one_anywhere_unset(hint), size - 1);

        auto unset_bitmap = MUST(Bitmap::create(size, true));
        for (size_t i = 0; i < size; ++i)
            unset_bitmap.set(i, false);
        EXPECT(!unset_bitmap.find_first_set().has_value());
        for (size_t hint = 0; hint < size; ++hint)
            EXPECT(!unset_bitmap.find_one_anywhere_set(hint).has_value());

        unset_bitmap.set(size - 1, true);
        EXPECT_EQ(unset_bitmap.find_first_set(), size - 1);
        for (size_t hint = 0; hint < size; ++hint)
            EXPECT_EQ(unset_bitmap.find_one_anywhere_set(hint), size - 1);
    }
}

TEST_CASE(find_one_anywhere_set)
{
    {
        auto bitmap = MUST(Bitmap::create(168, false));
        bitmap.set(34, true);
        bitmap.set(97, true);
        EXPECT_EQ(bitmap.find_one_anywhere_set(0).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(31).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(32).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(34).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(36).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(63).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(64).value(), 97u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(96).value(), 97u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(97).value(), 97u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(127).value(), 97u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(128).value(), 34u);
    }
    {
        auto bitmap = MUST(Bitmap::create(128 + 24, false));
        bitmap.set(34, true);
        bitmap.set(126, true);
        EXPECT_EQ(bitmap.find_one_anywhere_set(0).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(63).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_set(64).value(), 126u);
    }
    {
        auto bitmap = MUST(Bitmap::create(32, false));
        bitmap.set(12, true);
        bitmap.set(24, true);
        auto got = bitmap.find_one_anywhere_set(0).value();
        EXPECT(got == 12 || got == 24);
    }
}

TEST_CASE(find_one_anywhere_unset)
{
    {
        auto bitmap = MUST(Bitmap::create(168, true));
        bitmap.set(34, false);
        bitmap.set(97, false);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(0).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(31).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(32).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(34).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(36).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(63).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(64).value(), 97u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(96).value(), 97u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(97).value(), 97u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(127).value(), 97u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(128).value(), 34u);
    }
    {
        auto bitmap = MUST(Bitmap::create(128 + 24, true));
        bitmap.set(34, false);
        bitmap.set(126, false);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(0).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(63).value(), 34u);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(64).value(), 126u);
    }
    {
        auto bitmap = MUST(Bitmap::create(32, true));
        bitmap.set(12, false);
        bitmap.set(24, false);
        auto got = bitmap.find_one_anywhere_unset(0).value();
        EXPECT(got == 12 || got == 24);
    }
}

TEST_CASE(find_first_range)
{
    auto bitmap = MUST(Bitmap::create(128, true));
    bitmap.set(47, false);
    bitmap.set(48, false);
    bitmap.set(49, false);
    bitmap.set(50, false);
    bitmap.set(51, false);
    size_t found_range_size = 0;
    auto result = bitmap.find_longest_range_of_unset_bits(5, found_range_size);
    EXPECT_EQ(result.has_value(), true);
    EXPECT_EQ(found_range_size, 5u);
    EXPECT_EQ(result.value(), 47u);
}

TEST_CASE(set_range)
{
    {
        auto bitmap = MUST(Bitmap::create(128, false));
        bitmap.set_range(41, 10, true);
        EXPECT_EQ(bitmap.get(40), false);
        EXPECT_EQ(bitmap.get(41), true);
        EXPECT_EQ(bitmap.get(42), true);
        EXPECT_EQ(bitmap.get(43), true);
        EXPECT_EQ(bitmap.get(44), true);
        EXPECT_EQ(bitmap.get(45), true);
        EXPECT_EQ(bitmap.get(46), true);
        EXPECT_EQ(bitmap.get(47), true);
        EXPECT_EQ(bitmap.get(48), true);
        EXPECT_EQ(bitmap.get(49), true);
        EXPECT_EQ(bitmap.get(50), true);
        EXPECT_EQ(bitmap.get(51), false);
    }
    {
        auto bitmap = MUST(Bitmap::create(288, false));
        bitmap.set_range(48, 32, true);
        bitmap.set_range(94, 39, true);
        bitmap.set_range(190, 71, true);
        bitmap.set_range(190 + 71 - 7, 21, false); // slightly overlapping clear
        for (size_t i = 0; i < bitmap.size(); i++) {
            bool should_be_set = (i >= 48 && i < 48 + 32)
                || (i >= 94 && i < 94 + 39)
                || ((i >= 190 && i < 190 + 71) && !(i >= 190 + 71 - 7 && i < 190 + 71 - 7 + 21));
            EXPECT_EQ(bitmap.get(i), should_be_set);
        }
        EXPECT_EQ(bitmap.count_slow(true), 32u + 39u + 71u - 7u);
    }
}

TEST_CASE(find_first_fit)
{
    {
        auto bitmap = MUST(Bitmap::create(32, true));
        auto fit = bitmap.find_first_fit(1);
        EXPECT_EQ(fit.has_value(), false);
    }
    {
        auto bitmap = MUST(Bitmap::create(32, true));
        bitmap.set(31, false);
        auto fit = bitmap.find_first_fit(1);
        EXPECT_EQ(fit.has_value(), true);
        EXPECT_EQ(fit.value(), 31u);
    }

    for (size_t i = 0; i < 128; ++i) {
        auto bitmap = MUST(Bitmap::create(128, true));
        bitmap.set(i, false);
        auto fit = bitmap.find_first_fit(1);
        EXPECT_EQ(fit.has_value(), true);
        EXPECT_EQ(fit.value(), i);
    }

    for (size_t i = 0; i < 127; ++i) {
        auto bitmap = MUST(Bitmap::create(128, true));
        bitmap.set(i, false);
        bitmap.set(i + 1, false);
        auto fit = bitmap.find_first_fit(2);
        EXPECT_EQ(fit.has_value(), true);
        EXPECT_EQ(fit.value(), i);
    }

    size_t bitmap_size = 1024;
    for (size_t chunk_size = 1; chunk_size < 64; ++chunk_size) {
        for (size_t i = 0; i < bitmap_size - chunk_size; ++i) {
            auto bitmap = MUST(Bitmap::create(bitmap_size, true));
            for (size_t c = 0; c < chunk_size; ++c)
                bitmap.set(i + c, false);
            auto fit = bitmap.find_first_fit(chunk_size);
            EXPECT_EQ(fit.has_value(), true);
            EXPECT_EQ(fit.value(), i);
        }
    }
}

TEST_CASE(find_longest_range_of_unset_bits_edge)
{
    auto bitmap = MUST(Bitmap::create(36, true));
    bitmap.set_range(32, 4, false);
    size_t found_range_size = 0;
    auto result = bitmap.find_longest_range_of_unset_bits(1, found_range_size);
    EXPECT_EQ(result.has_value(), true);
    EXPECT_EQ(result.value(), 32u);
}

TEST_CASE(count_in_range)
{
    auto bitmap = MUST(Bitmap::create(256, false));
    bitmap.set(14, true);
    bitmap.set(17, true);
    bitmap.set(19, true);
    bitmap.set(20, true);
    for (size_t i = 34; i < 250; i++) {
        if (i < 130 || i > 183)
            bitmap.set(i, true);
    }

    auto count_bits_slow = [](Bitmap const& b, size_t start, size_t len, bool value) -> size_t {
        size_t count = 0;
        for (size_t i = start; i < start + len; i++) {
            if (b.get(i) == value)
                count++;
        }
        return count;
    };
    auto test_with_value = [&](bool value) {
        auto do_test = [&](size_t start, size_t len) {
            EXPECT_EQ(bitmap.count_in_range(start, len, value), count_bits_slow(bitmap, start, len, value));
        };
        do_test(16, 2);
        do_test(16, 3);
        do_test(16, 4);

        for (size_t start = 8; start < 24; start++) {
            for (size_t end = 9; end < 25; end++) {
                if (start >= end)
                    continue;
                do_test(start, end - start);
            }
        }

        for (size_t start = 1; start <= 9; start++) {
            for (size_t i = start + 1; i < bitmap.size() - start + 1; i++)
                do_test(start, i - start);
        }
    };
    test_with_value(true);
    test_with_value(false);
}

TEST_CASE(byte_aligned_access)
{
    {
        auto bitmap = MUST(Bitmap::create(16, true));
        EXPECT_EQ(bitmap.count_in_range(0, 16, true), 16u);
        EXPECT_EQ(bitmap.count_in_range(8, 8, true), 8u);
        EXPECT_EQ(bitmap.count_in_range(0, 8, true), 8u);
        EXPECT_EQ(bitmap.count_in_range(4, 8, true), 8u);
    }
    {
        auto bitmap = MUST(Bitmap::create(16, false));
        bitmap.set_range(4, 8, true);
        EXPECT_EQ(bitmap.count_in_range(0, 16, true), 8u);
        EXPECT_EQ(bitmap.count_in_range(8, 8, true), 4u);
        EXPECT_EQ(bitmap.count_in_range(0, 8, true), 4u);
        EXPECT_EQ(bitmap.count_in_range(4, 8, true), 8u);
    }
    {
        auto bitmap = MUST(Bitmap::create(8, false));
        bitmap.set(2, true);
        bitmap.set(4, true);
        EXPECT_EQ(bitmap.count_in_range(0, 2, true), 0u);
        EXPECT_EQ(bitmap.count_in_range(0, 4, true), 1u);
        EXPECT_EQ(bitmap.count_in_range(0, 8, true), 2u);
        EXPECT_EQ(bitmap.count_in_range(4, 4, true), 1u);
    }
}

RANDOMIZED_TEST_CASE(set_get)
{
    GEN(init, Gen::boolean());
    GEN(new_value, Gen::boolean());
    GEN(size, Gen::number_u64(1, 64));
    GEN(i, Gen::number_u64(size - 1));

    auto bitmap = MUST(Bitmap::create(size, init));
    bitmap.set(i, new_value);

    EXPECT_EQ(bitmap.get(i), new_value);
}

RANDOMIZED_TEST_CASE(set_range)
{
    GEN(init, Gen::boolean());
    GEN(size, Gen::number_u64(1, 64));
    GEN(new_value, Gen::boolean());

    GEN(start, Gen::number_u64(size - 1));
    GEN(len, Gen::number_u64(size - start - 1));

    auto bitmap = MUST(Bitmap::create(size, init));
    bitmap.set_range(start, len, new_value);

    for (size_t i = start; i < start + len; ++i)
        EXPECT_EQ(bitmap.get(i), new_value);

    EXPECT_EQ(bitmap.count_in_range(start, len, new_value), len);
}

RANDOMIZED_TEST_CASE(fill)
{
    GEN(init, Gen::boolean());
    GEN(size, Gen::number_u64(1, 64));
    GEN(new_value, Gen::boolean());

    auto bitmap = MUST(Bitmap::create(size, init));
    bitmap.fill(new_value);

    EXPECT_EQ(bitmap.count_slow(new_value), size);
}

TEST_CASE(find_one_anywhere_edge_case)
{
    {
        auto bitmap = MUST(Bitmap::create(1, false));
        bitmap.set(0, false);
        EXPECT_EQ(bitmap.find_one_anywhere_unset(0).value(), 0UL);
    }
}

RANDOMIZED_TEST_CASE(find_one_anywhere)
{
    GEN(init, Gen::boolean());
    GEN(size, Gen::number_u64(1, 64));
    GEN(hint, Gen::number_u64(size - 1));

    GEN(new_value, Gen::boolean());
    GEN(i, Gen::number_u64(size - 1));

    auto bitmap = MUST(Bitmap::create(size, init));
    bitmap.set(i, new_value);

    Optional<size_t> result = new_value
        ? bitmap.find_one_anywhere_set(hint)
        : bitmap.find_one_anywhere_unset(hint);

    auto expected_found_index = init == new_value ? 0 : i;
    EXPECT_EQ(result.value(), expected_found_index);
}

TEST_CASE(find_first_edge_case)
{
    {
        auto bitmap = MUST(Bitmap::create(1, false));
        bitmap.set(0, false);
        EXPECT_EQ(bitmap.find_first_unset().value(), 0UL);
    }
}

RANDOMIZED_TEST_CASE(find_first)
{
    GEN(init, Gen::boolean());
    GEN(size, Gen::number_u64(1, 64));

    GEN(new_value, Gen::boolean());
    GEN(i, Gen::number_u64(size - 1));

    auto bitmap = MUST(Bitmap::create(size, init));
    bitmap.set(i, new_value);

    Optional<size_t> result = new_value
        ? bitmap.find_first_set()
        : bitmap.find_first_unset();

    auto expected_found_index = init == new_value ? 0 : i;
    EXPECT_EQ(result.value(), expected_found_index);
}
