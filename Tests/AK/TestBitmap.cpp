/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Bitmap.h>

using namespace Test::Randomized;

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
