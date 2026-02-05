/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/AllOf.h>
#include <AK/Array.h>
#include <AK/Vector.h>

using namespace Test::Randomized;

TEST_CASE(vacuous_truth)
{
    constexpr Array<int, 0> a {};
    static_assert(all_of(a.begin(), a.end(), [](auto) { return false; }));
    EXPECT(all_of(a.begin(), a.end(), [](auto) { return false; }));
}

TEST_CASE(all_but_one_false)
{
    constexpr Array<int, 5> a { 0, 1, 2, 3, 4 };
    static_assert(!all_of(a.begin(), a.end(), [](auto n) { return n != 3; }));
    EXPECT(!all_of(a.begin(), a.end(), [](auto n) { return n != 3; }));
}

RANDOMIZED_TEST_CASE(trivial_all_true)
{
    GEN(vec, Gen::vector(0, 10, []() { return Gen::number_u64(); }));
    EXPECT(all_of(vec.begin(), vec.end(), [](auto) { return true; }));
}

RANDOMIZED_TEST_CASE(trivial_all_false)
{
    GEN(vec, Gen::vector(1, 10, []() { return Gen::number_u64(); }));
    EXPECT(!all_of(vec.begin(), vec.end(), [](auto) { return false; }));
}

TEST_CASE(should_determine_if_predicate_applies_to_all_elements_in_container)
{
    constexpr Array<int, 10> a {};

    static_assert(all_of(a.begin(), a.end(), [](auto elem) { return elem == 0; }));
    static_assert(!all_of(a.begin(), a.end(), [](auto elem) { return elem == 1; }));

    EXPECT(all_of(a.begin(), a.end(), [](auto elem) { return elem == 0; }));
    EXPECT(!all_of(a.begin(), a.end(), [](auto elem) { return elem == 1; }));
}

TEST_CASE(container_form)
{
    constexpr Array a { 10, 20, 30 };
    static_assert(all_of(a, [](auto elem) { return elem > 0; }));
    static_assert(!all_of(a, [](auto elem) { return elem > 10; }));
    EXPECT(all_of(a, [](auto elem) { return elem > 0; }));
    EXPECT(!all_of(a, [](auto elem) { return elem > 10; }));

    Vector b { 10, 20, 30 };
    EXPECT(all_of(b, [](auto elem) { return elem > 0; }));
    EXPECT(!all_of(b, [](auto elem) { return elem > 10; }));

    struct ArbitraryIterable {
        struct ArbitraryIterator {
            ArbitraryIterator(int v)
                : value(v)
            {
            }

            bool operator==(ArbitraryIterator const&) const = default;
            int operator*() const { return value; }
            ArbitraryIterator& operator++()
            {
                ++value;
                return *this;
            }

            int value;
        };
        ArbitraryIterator begin() const { return 0; }
        ArbitraryIterator end() const { return 20; }
    };

    ArbitraryIterable c;
    EXPECT(all_of(c, [](auto elem) { return elem < 20; }));
    EXPECT(!all_of(c, [](auto elem) { return elem > 10; }));
}
