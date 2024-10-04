/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/OwnPtr.h>
#include <AK/ReverseIterator.h>
#include <AK/String.h>
#include <AK/Vector.h>

TEST_CASE(construct)
{
    EXPECT(Vector<int>().is_empty());
    EXPECT(Vector<int>().size() == 0);
}

TEST_CASE(ints)
{
    Vector<int> ints;
    ints.append(1);
    ints.append(2);
    ints.append(3);
    EXPECT_EQ(ints.size(), 3u);
    EXPECT_EQ(ints.take_last(), 3);
    EXPECT_EQ(ints.size(), 2u);
    EXPECT_EQ(ints.take_last(), 2);
    EXPECT_EQ(ints.size(), 1u);
    EXPECT_EQ(ints.take_last(), 1);
    EXPECT_EQ(ints.size(), 0u);

    ints.clear();
    EXPECT_EQ(ints.size(), 0u);
}

TEST_CASE(strings)
{
    Vector<ByteString> strings;
    strings.append("ABC");
    strings.append("DEF");

    int loop_counter = 0;
    for (ByteString const& string : strings) {
        EXPECT(!string.is_empty());
        ++loop_counter;
    }

    loop_counter = 0;
    for (auto& string : (const_cast<Vector<ByteString> const&>(strings))) {
        EXPECT(!string.is_empty());
        ++loop_counter;
    }
    EXPECT_EQ(loop_counter, 2);
}

TEST_CASE(conforms_to_iterator_protocol)
{
    static_assert(std::random_access_iterator<Vector<int>::Iterator>);
    static_assert(std::random_access_iterator<Vector<int>::ConstIterator>);
    static_assert(std::random_access_iterator<Vector<int const>::Iterator>);
    static_assert(std::random_access_iterator<Vector<int const>::ConstIterator>);

    static_assert(std::random_access_iterator<Vector<String>::Iterator>);
    static_assert(std::random_access_iterator<Vector<String>::ConstIterator>);
    static_assert(std::random_access_iterator<Vector<String const>::Iterator>);
    static_assert(std::random_access_iterator<Vector<String const>::ConstIterator>);
}

TEST_CASE(strings_insert_ordered)
{
    Vector<ByteString> strings;
    strings.append("abc");
    strings.append("def");
    strings.append("ghi");

    strings.insert_before_matching("f-g"sv, [](auto& entry) {
        return "f-g"sv < entry;
    });

    EXPECT_EQ(strings[0], "abc");
    EXPECT_EQ(strings[1], "def");
    EXPECT_EQ(strings[2], "f-g");
    EXPECT_EQ(strings[3], "ghi");
}

TEST_CASE(prepend_vector)
{
    Vector<int> ints;
    ints.append(1);
    ints.append(2);
    ints.append(3);

    Vector<int> more_ints;
    more_ints.append(4);
    more_ints.append(5);
    more_ints.append(6);

    ints.prepend(move(more_ints));

    EXPECT_EQ(ints.size(), 6u);
    EXPECT_EQ(more_ints.size(), 0u);

    EXPECT_EQ(ints[0], 4);
    EXPECT_EQ(ints[1], 5);
    EXPECT_EQ(ints[2], 6);
    EXPECT_EQ(ints[3], 1);
    EXPECT_EQ(ints[4], 2);
    EXPECT_EQ(ints[5], 3);

    ints.prepend(move(more_ints));
    EXPECT_EQ(ints.size(), 6u);
    EXPECT_EQ(more_ints.size(), 0u);

    more_ints.prepend(move(ints));
    EXPECT_EQ(more_ints.size(), 6u);
    EXPECT_EQ(ints.size(), 0u);
}

TEST_CASE(prepend_vector_object)
{
    struct SubObject {
        SubObject(int v)
            : value(v)
        {
        }
        int value { 0 };
    };
    struct Object {
        Object(NonnullOwnPtr<SubObject>&& a_subobject)
            : subobject(move(a_subobject))
        {
        }
        OwnPtr<SubObject> subobject;
    };

    Vector<Object> objects;
    objects.empend(make<SubObject>(1));
    objects.empend(make<SubObject>(2));
    objects.empend(make<SubObject>(3));

    EXPECT_EQ(objects.size(), 3u);

    Vector<Object> more_objects;
    more_objects.empend(make<SubObject>(4));
    more_objects.empend(make<SubObject>(5));
    more_objects.empend(make<SubObject>(6));
    EXPECT_EQ(more_objects.size(), 3u);

    objects.prepend(move(more_objects));
    EXPECT_EQ(more_objects.size(), 0u);
    EXPECT_EQ(objects.size(), 6u);

    EXPECT_EQ(objects[0].subobject->value, 4);
    EXPECT_EQ(objects[1].subobject->value, 5);
    EXPECT_EQ(objects[2].subobject->value, 6);
    EXPECT_EQ(objects[3].subobject->value, 1);
    EXPECT_EQ(objects[4].subobject->value, 2);
    EXPECT_EQ(objects[5].subobject->value, 3);
}

TEST_CASE(vector_compare)
{
    Vector<int> ints;
    Vector<int> same_ints;

    for (int i = 0; i < 1000; ++i) {
        ints.append(i);
        same_ints.append(i);
    }

    EXPECT_EQ(ints.size(), 1000u);
    EXPECT_EQ(ints, same_ints);

    Vector<ByteString> strings;
    Vector<ByteString> same_strings;

    for (int i = 0; i < 1000; ++i) {
        strings.append(ByteString::number(i));
        same_strings.append(ByteString::number(i));
    }

    EXPECT_EQ(strings.size(), 1000u);
    EXPECT_EQ(strings, same_strings);
}

TEST_CASE(grow_past_inline_capacity)
{
    auto make_vector = [] {
        Vector<ByteString, 16> strings;
        for (int i = 0; i < 32; ++i) {
            strings.append(ByteString::number(i));
        }
        return strings;
    };

    auto strings = make_vector();

    EXPECT_EQ(strings.size(), 32u);
    EXPECT_EQ(strings[31], "31");

    strings.clear();
    EXPECT_EQ(strings.size(), 0u);
    EXPECT_EQ(strings.capacity(), 16u);

    strings = make_vector();

    strings.clear_with_capacity();
    EXPECT_EQ(strings.size(), 0u);
    EXPECT(strings.capacity() >= 32u);
}

BENCHMARK_CASE(vector_append_trivial)
{
    // This should be super fast thanks to Vector using memmove.
    Vector<int> ints;
    for (int i = 0; i < 1000000; ++i) {
        ints.append(i);
    }
    for (int i = 0; i < 100; ++i) {
        Vector<int> tmp;
        tmp.extend(ints);
        EXPECT_EQ(tmp.size(), 1000000u);
    }
}

BENCHMARK_CASE(vector_remove_trivial)
{
    // This should be super fast thanks to Vector using memmove.
    Vector<int> ints;
    for (int i = 0; i < 10000; ++i) {
        ints.append(i);
    }
    while (!ints.is_empty()) {
        ints.remove(0);
    }
    EXPECT_EQ(ints.size(), 0u);
}

TEST_CASE(vector_remove)
{
    Vector<int> ints;
    ints.append(1);
    ints.append(2);
    ints.append(3);
    ints.append(4);
    ints.append(5);

    ints.remove(1);
    EXPECT_EQ(ints.size(), 4u);
    EXPECT_EQ(ints[0], 1);
    EXPECT_EQ(ints[1], 3);
    EXPECT_EQ(ints[2], 4);
    EXPECT_EQ(ints[3], 5);

    ints.remove(0);
    EXPECT_EQ(ints.size(), 3u);
    EXPECT_EQ(ints[0], 3);
    EXPECT_EQ(ints[1], 4);
    EXPECT_EQ(ints[2], 5);

    ints.take_last();
    EXPECT_EQ(ints.size(), 2u);
    EXPECT_EQ(ints[0], 3);
    EXPECT_EQ(ints[1], 4);

    ints.take_first();
    EXPECT_EQ(ints.size(), 1u);
    EXPECT_EQ(ints[0], 4);
}

TEST_CASE(remove_all_matching)
{
    Vector<int> ints;

    ints.append(1);
    ints.append(2);
    ints.append(3);
    ints.append(4);

    EXPECT_EQ(ints.size(), 4u);

    EXPECT_EQ(ints.remove_all_matching([&](int value) { return value > 2; }), true);
    EXPECT_EQ(ints.remove_all_matching([&](int) { return false; }), false);

    EXPECT_EQ(ints.size(), 2u);

    EXPECT_EQ(ints.remove_all_matching([&](int) { return true; }), true);

    EXPECT(ints.is_empty());

    EXPECT_EQ(ints.remove_all_matching([&](int) { return true; }), false);
}

TEST_CASE(nonnullownptrvector)
{
    struct Object {
        ByteString string;
    };
    Vector<NonnullOwnPtr<Object>> objects;

    objects.append(make<Object>());
    EXPECT_EQ(objects.size(), 1u);

    OwnPtr<Object> o = make<Object>();
    objects.append(o.release_nonnull());
    EXPECT(o == nullptr);
    EXPECT_EQ(objects.size(), 2u);
}

TEST_CASE(insert_trivial)
{
    Vector<int> ints;
    ints.append(0);
    ints.append(10);
    ints.append(20);
    ints.append(30);
    ints.append(40);
    ints.insert(2, 15);
    EXPECT_EQ(ints.size(), 6u);
    EXPECT_EQ(ints[0], 0);
    EXPECT_EQ(ints[1], 10);
    EXPECT_EQ(ints[2], 15);
    EXPECT_EQ(ints[3], 20);
    EXPECT_EQ(ints[4], 30);
    EXPECT_EQ(ints[5], 40);
}

TEST_CASE(resize_initializes)
{
    struct A {
        A() { initialized = true; }
        bool initialized { false };
    };

    Vector<A> ints;
    ints.resize(32);

    for (size_t idx = 0; idx < 32; ++idx)
        EXPECT(ints[idx].initialized);
}

TEST_CASE(should_compare_vectors_of_same_type)
{
    Vector<int> a {};
    Vector<int> b {};

    EXPECT(a == b);
    EXPECT(!(a != b));

    a.append(1);
    EXPECT(!(a == b));
    EXPECT(a != b);

    b.append(1);
    EXPECT(a == b);
    EXPECT(!(a != b));

    a.append(42);
    b.append(17);
    EXPECT(!(a == b));
    EXPECT(a != b);
}

TEST_CASE(should_compare_vectors_of_different_inline_capacity)
{
    Vector<int, 1> a {};
    Vector<int, 64> b {};

    EXPECT(a == b);
    EXPECT(!(a != b));

    a.append(1);
    EXPECT(!(a == b));
    EXPECT(a != b);

    b.append(1);
    EXPECT(a == b);
    EXPECT(!(a != b));

    a.append(42);
    b.append(17);
    EXPECT(!(a == b));
    EXPECT(a != b);
}

TEST_CASE(should_compare_vectors_of_different_sizes)
{
    Vector<int, 0> a {};
    Vector<int, 0> b {};

    EXPECT(a == b);
    EXPECT(!(a != b));

    // A is longer
    a.append(1);
    EXPECT(!(a == b));
    EXPECT(a != b);

    b.append(1);
    EXPECT(a == b);
    EXPECT(!(a != b));

    // B is longer
    b.append(42);
    EXPECT(!(a == b));
    EXPECT(a != b);
}

TEST_CASE(should_find_value)
{
    Vector<int> v { 1, 2, 3, 4, 0, 6, 7, 8, 0, 0 };

    auto const expected = v.begin() + 4;

    EXPECT_EQ(expected, v.find(0));
}

TEST_CASE(should_find_predicate)
{
    Vector<int> v { 1, 2, 3, 4, 0, 6, 7, 8, 0, 0 };

    auto const expected = v.begin() + 4;

    EXPECT_EQ(expected, v.find_if([](auto const v) { return v == 0; }));
}

TEST_CASE(should_find_index)
{
    Vector<int> v { 1, 2, 3, 4, 0, 6, 7, 8, 0, 0 };

    EXPECT_EQ(4u, v.find_first_index(0).value());
    EXPECT(!v.find_first_index(42).has_value());
}

TEST_CASE(should_find_predicate_index)
{
    Vector<int> v { 1, 2, 3, 4, 0, 6, 7, 8, 0, 0 };

    EXPECT_EQ(4u, v.find_first_index_if([](auto const v) { return v == 0; }).value());
    EXPECT(!v.find_first_index_if([](auto const v) { return v == 123; }).has_value());
}

TEST_CASE(should_find_using_a_hashcompatible_value)
{
    // Tests whether a hash-compatible value can be used to compare (Strings cannot be impliticly constructed from a StringView.)
    Vector v { "hello!"_string };
    EXPECT(v.contains_slow("hello!"sv));
}

TEST_CASE(should_contain_start)
{
    // Tests whether value is found if at the start of the range.
    Vector<int> v { 1, 2, 3, 4, 5 };

    EXPECT(v.contains_in_range(1, 0, 4));
}

TEST_CASE(should_contain_end)
{
    // Tests whether value is found if at the end of the range.
    Vector<int> v { 1, 2, 3, 4, 5 };

    EXPECT(v.contains_in_range(5, 0, 4));
}

TEST_CASE(should_contain_range)
{
    // Tests whether value is found within a range.
    Vector<int> v { 1, 2, 3, 4, 5 };

    EXPECT(v.contains_in_range(3, 0, 4));
}

TEST_CASE(should_not_contain_not_present)
{
    // Tests whether a value that is not present is not found, as expected.
    Vector<int> v { 1, 2, 3, 4, 5 };

    EXPECT(!v.contains_in_range(6, 0, 4));
}

TEST_CASE(should_not_contain_present_not_in_range)
{
    // Tests whether a value that is present, but not in range, is not found.
    Vector<int> v { 1, 2, 3, 4, 5 };

    EXPECT(!v.contains_in_range(2, 2, 4));
}

TEST_CASE(can_store_references)
{
    int my_integer = 42;
    Vector<int&> references;
    references.append(my_integer);
    references.prepend(my_integer);
    EXPECT_EQ(&references.first(), &references.last());

    {
        Vector<int&> other_references;
        other_references.extend(references);
        EXPECT_EQ(&other_references.first(), &my_integer);
    }

    {
        Vector<int&> other_references;
        other_references = references;
        EXPECT_EQ(&other_references.first(), &my_integer);
    }

    {
        auto it = references.find(my_integer);
        EXPECT(!it.is_end());
        EXPECT_EQ(*it, my_integer);
    }

    {
        int other_integer = 42;
        auto index = references.find_first_index(other_integer);
        EXPECT(index.has_value());
        EXPECT_EQ(index.value_or(99999u), 0u);
    }

    {
        auto integer = 42;
        EXPECT(references.contains_slow(integer));
    }

    {
        references.remove(0);
        references.ensure_capacity(10);
        EXPECT_EQ(&references.take_first(), &my_integer);
    }
}

TEST_CASE(reference_deletion_should_not_affect_object)
{
    size_t times_deleted = 0;
    struct DeleteCounter {
        explicit DeleteCounter(size_t& deleted)
            : deleted(deleted)
        {
        }

        ~DeleteCounter()
        {
            ++deleted;
        }

        size_t& deleted;
    };

    {
        DeleteCounter counter { times_deleted };
        Vector<DeleteCounter&> references;
        for (size_t i = 0; i < 16; ++i)
            references.append(counter);
    }
    EXPECT_EQ(times_deleted, 1u);
}

TEST_CASE(rbegin)
{
    Vector<int> v { 1, 2, 3, 4, 5, 6, 7, 8, 0 };

    auto const expected = v.begin() + 4;
    auto const expected_in_reverse = v.rbegin() + 4;
    EXPECT_EQ(*expected, *expected_in_reverse);
}

TEST_CASE(rend)
{
    Vector<int> v { 1, 2, 3, 4, 5, 6, 7, 8, 0 };

    auto const expected = v.end() - 5;
    auto const expected_in_reverse = v.rend() - 5;
    EXPECT_EQ(*expected, *expected_in_reverse);
}

TEST_CASE(reverse_iterator_for_loop)
{
    Vector<int> v { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    int index = 9;
    for (auto rev = v.rbegin(); rev != v.rend(); ++rev)
        EXPECT_EQ(*rev, index--);
}

TEST_CASE(reverse_range_for_loop)
{
    Vector<int> v { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    int index = 9;
    for (auto item : AK::ReverseWrapper::in_reverse(v))
        EXPECT_EQ(item, index--);

    index = 9;
    for (auto item : v.in_reverse())
        EXPECT_EQ(item, index--);
}

static bool is_inline_element(auto& el, auto& vector)
{
    uintptr_t vector_ptr = (uintptr_t)&vector;
    uintptr_t element_ptr = (uintptr_t)&el;
    return (element_ptr >= vector_ptr && element_ptr < (vector_ptr + sizeof(vector)));
}

TEST_CASE(uses_inline_capacity_when_appended_to)
{
    Vector<int, 10> v;
    v.unchecked_append(1);
    v.unchecked_append(123);
    v.unchecked_append(50);
    v.unchecked_append(43);

    for (auto& el : v)
        EXPECT(is_inline_element(el, v));
}

TEST_CASE(uses_inline_capacity_when_constructed_from_initializer_list)
{
    Vector<int, 10> v { 10, 9, 3, 1, 3 };

    for (auto& el : v)
        EXPECT(is_inline_element(el, v));
}

TEST_CASE(uses_inline_capacity_when_constructed_from_other_vector)
{
    Vector other { 4, 3, 2, 1 };
    Vector<int, 10> v(other);

    for (auto& el : v)
        EXPECT(is_inline_element(el, v));
}

TEST_CASE(uses_inline_capacity_when_constructed_from_span)
{
    Array array { "f00", "bar", "baz" };
    Vector<char const*, 10> v(array.span());

    for (auto& el : v)
        EXPECT(is_inline_element(el, v));
}
