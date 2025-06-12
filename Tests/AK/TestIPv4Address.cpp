/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Endian.h>
#include <AK/IPv4Address.h>

TEST_CASE(should_default_contructor_with_0s)
{
    constexpr IPv4Address addr {};

    static_assert(addr.is_zero());

    EXPECT(addr.is_zero());
}

TEST_CASE(should_construct_from_c_array)
{
    constexpr auto addr = [] {
        u8 const a[4] = { 1, 2, 3, 4 };
        return IPv4Address(a);
    }();

    static_assert(!addr.is_zero());

    EXPECT(!addr.is_zero());
}

TEST_CASE(should_construct_from_u32)
{
    constexpr auto addr = [] {
        NetworkOrdered<u32> const a = 0x11'22'33'44;
        return IPv4Address(a);
    }();

    static_assert(!addr.is_zero());

    EXPECT(!addr.is_zero());
}

TEST_CASE(should_get_octets_by_byte_offset)
{
    constexpr IPv4Address addr(1, 25, 39, 42);

    static_assert(1 == addr[0]);
    static_assert(25 == addr[1]);
    static_assert(39 == addr[2]);
    static_assert(42 == addr[3]);

    EXPECT_EQ(1, addr[0]);
    EXPECT_EQ(25, addr[1]);
    EXPECT_EQ(39, addr[2]);
    EXPECT_EQ(42, addr[3]);
}

TEST_CASE(should_convert_to_string)
{
    constexpr IPv4Address addr(1, 25, 39, 42);

    EXPECT_EQ("1.25.39.42", addr.to_byte_string());
}

TEST_CASE(should_make_ipv4_address_from_string)
{
    auto const addr = IPv4Address::from_string("192.168.0.1"sv);

    EXPECT(addr.has_value());
    EXPECT_EQ(192, addr.value()[0]);
    EXPECT_EQ(168, addr.value()[1]);
    EXPECT_EQ(0, addr.value()[2]);
    EXPECT_EQ(1, addr.value()[3]);
}

TEST_CASE(should_make_empty_optional_from_bad_string)
{
    auto const addr = IPv4Address::from_string("bad string"sv);

    EXPECT(!addr.has_value());
}

TEST_CASE(should_make_empty_optional_from_out_of_range_values)
{
    auto const addr = IPv4Address::from_string("192.168.0.500"sv);

    EXPECT(!addr.has_value());
}

TEST_CASE(should_fill_d_octet_from_1_part)
{
    auto const addr = IPv4Address::from_string("1"sv);

    EXPECT(addr.has_value());
    EXPECT_EQ(0, addr.value()[0]);
    EXPECT_EQ(0, addr.value()[1]);
    EXPECT_EQ(0, addr.value()[2]);
    EXPECT_EQ(1, addr.value()[3]);
}

TEST_CASE(should_fill_a_and_d_octets_from_2_parts)
{
    auto const addr = IPv4Address::from_string("192.1"sv);

    EXPECT(addr.has_value());
    EXPECT_EQ(192, addr.value()[0]);
    EXPECT_EQ(0, addr.value()[1]);
    EXPECT_EQ(0, addr.value()[2]);
    EXPECT_EQ(1, addr.value()[3]);
}

TEST_CASE(should_fill_a_b_d_octets_from_3_parts)
{
    auto const addr = IPv4Address::from_string("192.168.1"sv);

    EXPECT(addr.has_value());
    EXPECT_EQ(192, addr.value()[0]);
    EXPECT_EQ(168, addr.value()[1]);
    EXPECT_EQ(0, addr.value()[2]);
    EXPECT_EQ(1, addr.value()[3]);
}

TEST_CASE(should_convert_to_in_addr_t)
{
    constexpr IPv4Address addr(1, 2, 3, 4);

    static_assert(0x04'03'02'01u == addr.to_in_addr_t());

    EXPECT_EQ(0x04'03'02'01u, addr.to_in_addr_t());
}

TEST_CASE(should_convert_to_u32)
{
    constexpr IPv4Address addr(1, 2, 3, 4);

    static_assert(0x04'03'02'01u == addr.to_in_addr_t());

    EXPECT_EQ(0x04'03'02'01u, addr.to_u32());
}

TEST_CASE(should_compare)
{
    constexpr IPv4Address addr_a(1, 2, 3, 4);
    constexpr IPv4Address addr_b(1, 2, 3, 5);

    static_assert(addr_a != addr_b);
    static_assert(addr_a == addr_a);

    EXPECT(addr_a != addr_b);
    EXPECT(addr_a == addr_a);
}

TEST_CASE(netmask_from_cidr)
{
    EXPECT(IPv4Address::netmask_from_cidr(24) == IPv4Address(255, 255, 255, 0));

    EXPECT(IPv4Address::netmask_from_cidr(0) == IPv4Address(0, 0, 0, 0));
    EXPECT(IPv4Address::netmask_from_cidr(32) == IPv4Address(255, 255, 255, 255));

    EXPECT(IPv4Address::netmask_from_cidr(28) == IPv4Address(255, 255, 255, 240));
    EXPECT(IPv4Address::netmask_from_cidr(22) == IPv4Address(255, 255, 252, 0));
    EXPECT(IPv4Address::netmask_from_cidr(14) == IPv4Address(255, 252, 0, 0));
    EXPECT(IPv4Address::netmask_from_cidr(6) == IPv4Address(252, 0, 0, 0));
}
