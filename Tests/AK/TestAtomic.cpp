/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Atomic.h>

TEST_CASE(construct_empty)
{
    EXPECT(Atomic<bool>().load() == false);
    EXPECT(Atomic<u32>().load() == 0);
    EXPECT(Atomic<u16>().load() == 0);
    EXPECT(Atomic<u8>().load() == 0);

    EXPECT(Atomic<u16*>().load() == nullptr);
}

TEST_CASE(construct_with_value)
{
    EXPECT(Atomic<bool>(false).load() == false);
    EXPECT(Atomic<bool>(true).load() == true);
    EXPECT(Atomic<u32>(2).load() == 2);
    EXPECT(Atomic<u16>(3).load() == 3);
    EXPECT(Atomic<u8>(4).load() == 4);

    u16 v_u16 = 0;
    EXPECT(Atomic<u16*>(&v_u16).load() == &v_u16);
}

TEST_CASE(do_exchange)
{
    Atomic<bool> a_bool(false);
    EXPECT(a_bool.exchange(true) == false);
    EXPECT(a_bool.load() == true && static_cast<bool>(a_bool) == true);

    Atomic<u32> a_u32(2);
    EXPECT(a_u32.exchange(22) == 2);
    EXPECT(a_u32.load() == 22 && static_cast<u8>(a_u32) == 22);

    Atomic<u16> a_u16(3);
    EXPECT(a_u16.exchange(33) == 3);
    EXPECT(a_u16.load() == 33 && static_cast<u8>(a_u16) == 33);

    Atomic<u8> a_u8(4);
    EXPECT(a_u8.exchange(44) == 4);
    EXPECT(a_u8.load() == 44 && static_cast<u8>(a_u8) == 44);

    u16 v_u16[6];
    Atomic<u16*> a_pu16(&v_u16[2]);
    EXPECT(a_pu16.load() == &v_u16[2] && static_cast<u16*>(a_pu16) == &v_u16[2]);
}

TEST_CASE(do_compare_exchange)
{
    Atomic<bool> a_bool(false);
    bool e_bool = true;
    EXPECT(a_bool.compare_exchange_strong(e_bool, true) == false);
    EXPECT(e_bool == false);
    EXPECT(a_bool.load() == false && static_cast<bool>(a_bool) == false);
    e_bool = false;
    EXPECT(a_bool.compare_exchange_strong(e_bool, true) == true);
    EXPECT(a_bool.load() == true && static_cast<bool>(a_bool) == true);

    Atomic<u32> a_u32(2);
    u32 e_u32 = 99;
    EXPECT(a_u32.compare_exchange_strong(e_u32, 22) == false);
    EXPECT(e_u32 == 2);
    EXPECT(a_u32.load() == 2 && static_cast<u32>(a_u32) == 2);
    e_u32 = 2;
    EXPECT(a_u32.compare_exchange_strong(e_u32, 22) == true);
    EXPECT(a_u32.load() == 22 && static_cast<u32>(a_u32) == 22);

    Atomic<u16> a_u16(3);
    u16 e_u16 = 99;
    EXPECT(a_u16.compare_exchange_strong(e_u16, 33) == false);
    EXPECT(e_u16 == 3);
    EXPECT(a_u16.load() == 3 && static_cast<u16>(a_u16) == 3);
    e_u16 = 3;
    EXPECT(a_u16.compare_exchange_strong(e_u16, 33) == true);
    EXPECT(a_u16.load() == 33 && static_cast<u16>(a_u16) == 33);

    Atomic<u8> a_u8(4);
    u8 e_u8 = 99;
    EXPECT(a_u8.compare_exchange_strong(e_u8, 44) == false);
    EXPECT(e_u8 == 4);
    EXPECT(a_u8.load() == 4 && static_cast<u16>(a_u8) == 4);
    e_u8 = 4;
    EXPECT(a_u8.compare_exchange_strong(e_u8, 44) == true);
    EXPECT(a_u8.load() == 44 && static_cast<u16>(a_u8) == 44);
}

TEST_CASE(fetch_add)
{
    Atomic<u32> a_u32(5);
    EXPECT(a_u32.fetch_add(2) == 5);
    EXPECT(a_u32.load() == 7 && static_cast<u32>(a_u32) == 7);

    Atomic<u16> a_u16(5);
    EXPECT(a_u16.fetch_add(2) == 5);
    EXPECT(a_u16.load() == 7 && static_cast<u16>(a_u16) == 7);

    Atomic<u8> a_u8(5);
    EXPECT(a_u8.fetch_add(2) == 5);
    EXPECT(a_u8.load() == 7 && static_cast<u8>(a_u8) == 7);

    u32 v_u32[6];
    Atomic<u32*> a_pu32(&v_u32[2]);
    EXPECT(a_pu32.load() == &v_u32[2] && static_cast<u32*>(a_pu32) == &v_u32[2]);
    EXPECT(a_pu32.fetch_add(2) == &v_u32[2]);
    EXPECT(a_pu32.load() == &v_u32[4] && static_cast<u32*>(a_pu32) == &v_u32[4]);
    EXPECT(a_pu32.fetch_add(-3) == &v_u32[4]);
    EXPECT(a_pu32.load() == &v_u32[1] && static_cast<u32*>(a_pu32) == &v_u32[1]);

    u16 v_u16[6];
    Atomic<u16*> a_pu16(&v_u16[2]);
    EXPECT(a_pu16.load() == &v_u16[2] && static_cast<u16*>(a_pu16) == &v_u16[2]);
    EXPECT(a_pu16.fetch_add(2) == &v_u16[2]);
    EXPECT(a_pu16.load() == &v_u16[4] && static_cast<u16*>(a_pu16) == &v_u16[4]);
    EXPECT(a_pu16.fetch_add(-3) == &v_u16[4]);
    EXPECT(a_pu16.load() == &v_u16[1] && static_cast<u16*>(a_pu16) == &v_u16[1]);

    u8 v_u8[6];
    Atomic<u8*> a_pu8(&v_u8[2]);
    EXPECT(a_pu8.load() == &v_u8[2] && static_cast<u8*>(a_pu8) == &v_u8[2]);
    EXPECT(a_pu8.fetch_add(2) == &v_u8[2]);
    EXPECT(a_pu8.load() == &v_u8[4] && static_cast<u8*>(a_pu8) == &v_u8[4]);
    EXPECT(a_pu8.fetch_add(-3) == &v_u8[4]);
    EXPECT(a_pu8.load() == &v_u8[1] && static_cast<u8*>(a_pu8) == &v_u8[1]);
}

TEST_CASE(fetch_sub)
{
    Atomic<u32> a_u32(5);
    EXPECT(a_u32.fetch_sub(2) == 5);
    EXPECT(a_u32.load() == 3 && static_cast<u32>(a_u32) == 3);

    Atomic<u16> a_u16(5);
    EXPECT(a_u16.fetch_sub(2) == 5);
    EXPECT(a_u16.load() == 3 && static_cast<u16>(a_u16) == 3);

    Atomic<u8> a_u8(5);
    EXPECT(a_u8.fetch_sub(2) == 5);
    EXPECT(a_u8.load() == 3 && static_cast<u8>(a_u8) == 3);

    u32 v_u32[6];
    Atomic<u32*> a_pu32(&v_u32[2]);
    EXPECT(a_pu32.load() == &v_u32[2] && static_cast<u32*>(a_pu32) == &v_u32[2]);
    EXPECT(a_pu32.fetch_sub(2) == &v_u32[2]);
    EXPECT(a_pu32.load() == &v_u32[0] && static_cast<u32*>(a_pu32) == &v_u32[0]);
    EXPECT(a_pu32.fetch_sub(-3) == &v_u32[0]);
    EXPECT(a_pu32.load() == &v_u32[3] && static_cast<u32*>(a_pu32) == &v_u32[3]);

    u16 v_u16[6];
    Atomic<u16*> a_pu16(&v_u16[2]);
    EXPECT(a_pu16.load() == &v_u16[2] && static_cast<u16*>(a_pu16) == &v_u16[2]);
    EXPECT(a_pu16.fetch_sub(2) == &v_u16[2]);
    EXPECT(a_pu16.load() == &v_u16[0] && static_cast<u16*>(a_pu16) == &v_u16[0]);
    EXPECT(a_pu16.fetch_sub(-3) == &v_u16[0]);
    EXPECT(a_pu16.load() == &v_u16[3] && static_cast<u16*>(a_pu16) == &v_u16[3]);

    u8 v_u8[6];
    Atomic<u8*> a_pu8(&v_u8[2]);
    EXPECT(a_pu8.load() == &v_u8[2] && static_cast<u8*>(a_pu8) == &v_u8[2]);
    EXPECT(a_pu8.fetch_sub(2) == &v_u8[2]);
    EXPECT(a_pu8.load() == &v_u8[0] && static_cast<u8*>(a_pu8) == &v_u8[0]);
    EXPECT(a_pu8.fetch_sub(-3) == &v_u8[0]);
    EXPECT(a_pu8.load() == &v_u8[3] && static_cast<u8*>(a_pu8) == &v_u8[3]);
}

TEST_CASE(fetch_inc)
{
    Atomic<u32> a_u32(5);
    EXPECT(a_u32++ == 5);
    EXPECT(a_u32.load() == 6 && a_u32 == 6);
    EXPECT(++a_u32 == 7);
    EXPECT(a_u32.load() == 7 && a_u32 == 7);
    EXPECT((a_u32 += 2) == 9);
    EXPECT(a_u32.load() == 9 && a_u32 == 9);

    Atomic<u16> a_u16(5);
    EXPECT(a_u16++ == 5);
    EXPECT(a_u16.load() == 6 && a_u16 == 6);
    EXPECT(++a_u16 == 7);
    EXPECT(a_u16.load() == 7 && a_u16 == 7);
    EXPECT((a_u16 += 2) == 9);
    EXPECT(a_u16.load() == 9 && a_u16 == 9);

    Atomic<u8> a_u8(5);
    EXPECT(a_u8++ == 5);
    EXPECT(a_u8.load() == 6 && a_u8 == 6);
    EXPECT(++a_u8 == 7);
    EXPECT(a_u8.load() == 7 && a_u8 == 7);
    EXPECT((a_u8 += 2) == 9);
    EXPECT(a_u8.load() == 9 && a_u8 == 9);

    u32 v_u32[8];
    Atomic<u32*> a_pu32(&v_u32[2]);
    EXPECT(a_pu32++ == &v_u32[2]);
    EXPECT(a_pu32.load() == &v_u32[3] && a_pu32 == &v_u32[3]);
    EXPECT(++a_pu32 == &v_u32[4]);
    EXPECT(a_pu32.load() == &v_u32[4] && a_pu32 == &v_u32[4]);
    EXPECT((a_pu32 += 2) == &v_u32[6]);
    EXPECT(a_pu32.load() == &v_u32[6] && a_pu32 == &v_u32[6]);

    u16 v_u16[8];
    Atomic<u16*> a_pu16(&v_u16[2]);
    EXPECT(a_pu16++ == &v_u16[2]);
    EXPECT(a_pu16.load() == &v_u16[3] && a_pu16 == &v_u16[3]);
    EXPECT(++a_pu16 == &v_u16[4]);
    EXPECT(a_pu16.load() == &v_u16[4] && a_pu16 == &v_u16[4]);
    EXPECT((a_pu16 += 2) == &v_u16[6]);
    EXPECT(a_pu16.load() == &v_u16[6] && a_pu16 == &v_u16[6]);

    u8 v_u8[8];
    Atomic<u8*> a_pu8(&v_u8[2]);
    EXPECT(a_pu8++ == &v_u8[2]);
    EXPECT(a_pu8.load() == &v_u8[3] && a_pu8 == &v_u8[3]);
    EXPECT(++a_pu8 == &v_u8[4]);
    EXPECT(a_pu8.load() == &v_u8[4] && a_pu8 == &v_u8[4]);
    EXPECT((a_pu8 += 2) == &v_u8[6]);
    EXPECT(a_pu8.load() == &v_u8[6] && a_pu8 == &v_u8[6]);
}

TEST_CASE(fetch_dec)
{
    Atomic<u32> a_u32(5);
    EXPECT(a_u32-- == 5);
    EXPECT(a_u32.load() == 4 && a_u32 == 4);
    EXPECT(--a_u32 == 3);
    EXPECT(a_u32.load() == 3 && a_u32 == 3);
    EXPECT((a_u32 -= 2) == 1);
    EXPECT(a_u32.load() == 1 && a_u32 == 1);

    Atomic<u16> a_u16(5);
    EXPECT(a_u16-- == 5);
    EXPECT(a_u16.load() == 4 && a_u16 == 4);
    EXPECT(--a_u16 == 3);
    EXPECT(a_u16.load() == 3 && a_u16 == 3);
    EXPECT((a_u16 -= 2) == 1);
    EXPECT(a_u16.load() == 1 && a_u16 == 1);

    Atomic<u8> a_u8(5);
    EXPECT(a_u8-- == 5);
    EXPECT(a_u8.load() == 4 && a_u8 == 4);
    EXPECT(--a_u8 == 3);
    EXPECT(a_u8.load() == 3 && a_u8 == 3);
    EXPECT((a_u8 -= 2) == 1);
    EXPECT(a_u8.load() == 1 && a_u8 == 1);

    u32 v_u32[8];
    Atomic<u32*> a_pu32(&v_u32[7]);
    EXPECT(a_pu32-- == &v_u32[7]);
    EXPECT(a_pu32.load() == &v_u32[6] && a_pu32 == &v_u32[6]);
    EXPECT(--a_pu32 == &v_u32[5]);
    EXPECT(a_pu32.load() == &v_u32[5] && a_pu32 == &v_u32[5]);
    EXPECT((a_pu32 -= 2) == &v_u32[3]);
    EXPECT(a_pu32.load() == &v_u32[3] && a_pu32 == &v_u32[3]);

    u16 v_u16[8];
    Atomic<u16*> a_pu16(&v_u16[7]);
    EXPECT(a_pu16-- == &v_u16[7]);
    EXPECT(a_pu16.load() == &v_u16[6] && a_pu16 == &v_u16[6]);
    EXPECT(--a_pu16 == &v_u16[5]);
    EXPECT(a_pu16.load() == &v_u16[5] && a_pu16 == &v_u16[5]);
    EXPECT((a_pu16 -= 2) == &v_u16[3]);
    EXPECT(a_pu16.load() == &v_u16[3] && a_pu16 == &v_u16[3]);

    u8 v_u8[8];
    Atomic<u8*> a_pu8(&v_u8[7]);
    EXPECT(a_pu8-- == &v_u8[7]);
    EXPECT(a_pu8.load() == &v_u8[6] && a_pu8 == &v_u8[6]);
    EXPECT(--a_pu8 == &v_u8[5]);
    EXPECT(a_pu8.load() == &v_u8[5] && a_pu8 == &v_u8[5]);
    EXPECT((a_pu8 -= 2) == &v_u8[3]);
    EXPECT(a_pu8.load() == &v_u8[3] && a_pu8 == &v_u8[3]);
}

TEST_CASE(fetch_and)
{
    Atomic<u32> a_u32(0xdeadbeef);
    EXPECT(a_u32.fetch_and(0x8badf00d) == 0xdeadbeef);
    EXPECT(a_u32.load() == 0x8aadb00d && static_cast<u32>(a_u32) == 0x8aadb00d);
    a_u32 = 0xdeadbeef;
    EXPECT((a_u32 &= 0x8badf00d) == 0x8aadb00d);

    Atomic<u16> a_u16(0xbeef);
    EXPECT(a_u16.fetch_and(0xf00d) == 0xbeef);
    EXPECT(a_u16.load() == 0xb00d && static_cast<u16>(a_u16) == 0xb00d);
    a_u16 = 0xbeef;
    EXPECT((a_u16 &= 0xf00d) == 0xb00d);

    Atomic<u8> a_u8(0xef);
    EXPECT(a_u8.fetch_and(0x0d) == 0xef);
    EXPECT(a_u8.load() == 0x0d && static_cast<u8>(a_u8) == 0x0d);
    a_u8 = 0xef;
    EXPECT((a_u8 &= 0x0d) == 0x0d);
}

TEST_CASE(fetch_or)
{
    Atomic<u32> a_u32(0xaadb00d);
    EXPECT(a_u32.fetch_or(0xdeadbeef) == 0xaadb00d);
    EXPECT(a_u32.load() == 0xdeadbeef && static_cast<u32>(a_u32) == 0xdeadbeef);
    a_u32 = 0xaadb00d;
    EXPECT((a_u32 |= 0xdeadbeef) == 0xdeadbeef);

    Atomic<u16> a_u16(0xb00d);
    EXPECT(a_u16.fetch_or(0xbeef) == 0xb00d);
    EXPECT(a_u16.load() == 0xbeef && static_cast<u16>(a_u16) == 0xbeef);
    a_u16 = 0xb00d;
    EXPECT((a_u16 |= 0xbeef) == 0xbeef);

    Atomic<u8> a_u8(0x0d);
    EXPECT(a_u8.fetch_or(0xef) == 0x0d);
    EXPECT(a_u8.load() == 0xef && static_cast<u8>(a_u8) == 0xef);
    a_u8 = 0x0d;
    EXPECT((a_u8 |= 0xef) == 0xef);
}

TEST_CASE(fetch_xor)
{
    Atomic<u32> a_u32(0x55004ee2);
    EXPECT(a_u32.fetch_xor(0xdeadbeef) == 0x55004ee2);
    EXPECT(a_u32.load() == 0x8badf00d && static_cast<u32>(a_u32) == 0x8badf00d);
    a_u32 = 0x55004ee2;
    EXPECT((a_u32 ^= 0xdeadbeef) == 0x8badf00d);

    Atomic<u16> a_u16(0x4ee2);
    EXPECT(a_u16.fetch_xor(0xbeef) == 0x4ee2);
    EXPECT(a_u16.load() == 0xf00d && static_cast<u16>(a_u16) == 0xf00d);
    a_u16 = 0x4ee2;
    EXPECT((a_u16 ^= 0xbeef) == 0xf00d);

    Atomic<u8> a_u8(0xe2);
    EXPECT(a_u8.fetch_xor(0xef) == 0xe2);
    EXPECT(a_u8.load() == 0x0d && static_cast<u8>(a_u8) == 0x0d);
    a_u8 = 0xe2;
    EXPECT((a_u8 ^= 0xef) == 0x0d);
}
