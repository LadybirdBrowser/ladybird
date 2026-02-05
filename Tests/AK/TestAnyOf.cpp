/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/Vector.h>

using namespace Test::Randomized;

TEST_CASE(vacuous_truth)
{
    constexpr Array<int, 0> a {};
    static_assert(!any_of(a.begin(), a.end(), [](auto) { return true; }));
    EXPECT(!any_of(a.begin(), a.end(), [](auto) { return true; }));
}

TEST_CASE(all_false)
{
    constexpr Array<int, 5> a { 0, 1, 2, 3, 4 };
    static_assert(!any_of(a.begin(), a.end(), [](auto n) { return n > 10; }));
    EXPECT(!any_of(a.begin(), a.end(), [](auto n) { return n > 10; }));
}

RANDOMIZED_TEST_CASE(trivial_all_true)
{
    GEN(vec, Gen::vector(1, 10, []() { return Gen::number_u64(); }));
    EXPECT(any_of(vec.begin(), vec.end(), [](auto) { return true; }));
}

RANDOMIZED_TEST_CASE(trivial_all_false)
{
    GEN(vec, Gen::vector(0, 10, []() { return Gen::number_u64(); }));
    EXPECT(!any_of(vec.begin(), vec.end(), [](auto) { return false; }));
}

TEST_CASE(should_determine_if_predicate_applies_to_any_element_in_container)
{
    constexpr Array<int, 10> a { 1 };

    static_assert(any_of(a.begin(), a.end(), [](auto elem) { return elem == 0; }));
    static_assert(any_of(a.begin(), a.end(), [](auto elem) { return elem == 1; }));
    static_assert(!any_of(a.begin(), a.end(), [](auto elem) { return elem == 2; }));

    EXPECT(any_of(a.begin(), a.end(), [](auto elem) { return elem == 0; }));
    EXPECT(any_of(a.begin(), a.end(), [](auto elem) { return elem == 1; }));
    EXPECT(!any_of(a.begin(), a.end(), [](auto elem) { return elem == 2; }));
}

TEST_CASE(container_form)
{
    constexpr Array a { 10, 20, 30 };
    static_assert(any_of(a, [](auto elem) { return elem == 10; }));
    static_assert(any_of(a, [](auto elem) { return elem == 20; }));
    static_assert(!any_of(a, [](auto elem) { return elem == 40; }));

    EXPECT(any_of(a, [](auto elem) { return elem == 10; }));
    EXPECT(any_of(a, [](auto elem) { return elem == 20; }));
    EXPECT(!any_of(a, [](auto elem) { return elem == 40; }));

    Vector b { 10, 20, 30 };
    EXPECT(any_of(b, [](auto elem) { return elem > 10; }));
    EXPECT(!any_of(b, [](auto elem) { return elem > 40; }));

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
    EXPECT(any_of(c, [](auto elem) { return elem < 20; }));
    EXPECT(!any_of(c, [](auto elem) { return elem > 31; }));
}
