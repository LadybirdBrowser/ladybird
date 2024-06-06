/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <LibTest/TestCase.h>
#include <AK/Atomic.h>

template <typename T, typename U>
void test_construct_empty()
{
    EXPECT(Atomic<T>().load() == U());
}

template <typename T>
void test_construct_with_value(T value)
{
    EXPECT(Atomic<T>(value).load() == value);
}

template <typename T>
void test_exchange(T initial, T new_value)
{
    Atomic<T> atomic(initial);
    EXPECT(atomic.exchange(new_value) == initial);
    EXPECT(atomic.load() == new_value);
}

template <typename T>
void test_compare_exchange(T initial, T compare_value, T new_value)
{
    Atomic<T> atomic(initial);
    T expected = compare_value;
    EXPECT(atomic.compare_exchange_strong(expected, new_value) == (initial == compare_value));
    EXPECT(atomic.load() == (initial == compare_value ? new_value : initial));
}

template <typename T>
void test_fetch_add(T initial, T add_value)
{
    Atomic<T> atomic(initial);
    EXPECT(atomic.fetch_add(add_value) == initial);
    EXPECT(atomic.load() == static_cast<T>(initial + add_value));
}

template <typename T>
void test_fetch_sub(T initial, T sub_value)
{
    Atomic<T> atomic(initial);
    EXPECT(atomic.fetch_sub(sub_value) == initial);
    EXPECT(atomic.load() == static_cast<T>(initial - sub_value));
}

template <typename T>
void test_fetch_and(T initial, T and_value)
{
    Atomic<T> atomic(initial);
    EXPECT(atomic.fetch_and(and_value) == initial);
    EXPECT(atomic.load() == static_cast<T>(initial & and_value));
}

template <typename T>
void test_fetch_or(T initial, T or_value)
{
    Atomic<T> atomic(initial);
    EXPECT(atomic.fetch_or(or_value) == initial);
    EXPECT(atomic.load() == static_cast<T>(initial | or_value));
}

template <typename T>
void test_fetch_xor(T initial, T xor_value)
{
    Atomic<T> atomic(initial);
    EXPECT(atomic.fetch_xor(xor_value) == initial);
    EXPECT(atomic.load() == static_cast<T>(initial ^ xor_value));
}

TEST_CASE(construct_empty)
{
    test_construct_empty<bool, bool>();
    test_construct_empty<u32, u32>();
    test_construct_empty<u16, u16>();
    test_construct_empty<u8, u8>();
    test_construct_empty<u16*, std::nullptr_t>();
}

TEST_CASE(construct_with_value)
{
    test_construct_with_value(false);
    test_construct_with_value(true);
    test_construct_with_value<u32>(2);
    test_construct_with_value<u16>(3);
    test_construct_with_value<u8>(4);

    u16 v_u16 = 0;
    test_construct_with_value(&v_u16);
}

TEST_CASE(do_exchange)
{
    test_exchange(false, true);
    test_exchange<u32>(2, 22);
    test_exchange<u16>(3, 33);
    test_exchange<u8>(4, 44);

    u16 v_u16[6];
    test_exchange(&v_u16[2], &v_u16[3]);
}

TEST_CASE(do_compare_exchange)
{
    test_compare_exchange(false, true, true);
    test_compare_exchange<u32>(2, 99, 22);
    test_compare_exchange<u32>(2, 2, 22);
    test_compare_exchange<u16>(3, 99, 33);
    test_compare_exchange<u16>(3, 3, 33);
    test_compare_exchange<u8>(4, 99, 44);
    test_compare_exchange<u8>(4, 4, 44);
}

TEST_CASE(fetch_add)
{
    test_fetch_add<u32>(5, 2);
    test_fetch_add<u16>(5, 2);
    test_fetch_add<u8>(5, 2);

    u32 v_u32[6];
    Atomic<u32*> a_pu32(&v_u32[2]);
    EXPECT(a_pu32.fetch_add(2) == &v_u32[2]);
    EXPECT(a_pu32.load() == &v_u32[4]);

    u16 v_u16[6];
    Atomic<u16*> a_pu16(&v_u16[2]);
    EXPECT(a_pu16.fetch_add(2) == &v_u16[2]);
    EXPECT(a_pu16.load() == &v_u16[4]);

    u8 v_u8[6];
    Atomic<u8*> a_pu8(&v_u8[2]);
    EXPECT(a_pu8.fetch_add(2) == &v_u8[2]);
    EXPECT(a_pu8.load() == &v_u8[4]);
}

TEST_CASE(fetch_sub)
{
    test_fetch_sub<u32>(5, 2);
    test_fetch_sub<u16>(5, 2);
    test_fetch_sub<u8>(5, 2);

    u32 v_u32[6];
    Atomic<u32*> a_pu32(&v_u32[2]);
    EXPECT(a_pu32.fetch_sub(2) == &v_u32[2]);
    EXPECT(a_pu32.load() == &v_u32[0]);

    u16 v_u16[6];
    Atomic<u16*> a_pu16(&v_u16[2]);
    EXPECT(a_pu16.fetch_sub(2) == &v_u16[2]);
    EXPECT(a_pu16.load() == &v_u16[0]);

    u8 v_u8[6];
    Atomic<u8*> a_pu8(&v_u8[2]);
    EXPECT(a_pu8.fetch_sub(2) == &v_u8[2]);
    EXPECT(a_pu8.load() == &v_u8[0]);
}

TEST_CASE(fetch_and)
{
    test_fetch_and<u32>(0xdeadbeef, 0x8badf00d);
    test_fetch_and<u16>(0xbeef, 0xf00d);
    test_fetch_and<u8>(0xef, 0x0d);
}

TEST_CASE(fetch_or)
{
    test_fetch_or<u32>(0xaadb00d, 0xdeadbeef);
    test_fetch_or<u16>(0xb00d, 0xbeef);
    test_fetch_or<u8>(0x0d, 0xef);
}

TEST_CASE(fetch_xor)
{
    test_fetch_xor<u32>(0x55004ee2, 0xdeadbeef);
    test_fetch_xor<u16>(0x4ee2, 0xbeef);
    test_fetch_xor<u8>(0xe2, 0xef);
}

