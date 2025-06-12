/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/StdLibExtras.h>
#include <AK/TypeList.h>
#include <AK/Vector.h>

template<typename F, typename... Args>
F for_each_argument(F f, Args&&... args)
{
    (f(forward<Args>(args)), ...);
    return f;
}

template<typename T, T... ints>
void verify_sequence(IntegerSequence<T, ints...> seq, std::initializer_list<T> expected)
{
    EXPECT_EQ(seq.size(), expected.size());
    for_each_argument([idx = expected.begin()](T t) mutable { EXPECT_EQ(t, *(idx++)); }, ints...);
}

TEST_CASE(TestIndexSequence)
{
    constexpr auto integer_seq1 = IntegerSequence<int, 0, 1, 2, 3, 4> {};
    constexpr auto integer_seq2 = MakeIntegerSequence<int, 5> {};
    static_assert(IsSame<decltype(integer_seq1), decltype(integer_seq2)>, "");

    static_assert(integer_seq1.size() == 5, "");
    static_assert(integer_seq2.size() == 5, "");

    constexpr auto index_seq1 = IndexSequence<0, 1, 2> {};
    constexpr auto index_seq2 = MakeIndexSequence<3> {};
    static_assert(IsSame<decltype(index_seq1), decltype(index_seq2)>, "");

    verify_sequence(MakeIndexSequence<10> {}, std::initializer_list<size_t> { 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U });
    verify_sequence(MakeIntegerSequence<long, 16> {}, std::initializer_list<long> { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 });
}

TEST_CASE(TypeList)
{
    using MyTypes = TypeList<int, bool, char>;
    static_assert(IsSame<MyTypes::Type<0>, int>, "");
    static_assert(IsSame<MyTypes::Type<1>, bool>, "");
    static_assert(IsSame<MyTypes::Type<2>, char>, "");
}
