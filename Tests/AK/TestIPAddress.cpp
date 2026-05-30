/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Endian.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>

TEST_CASE(ipv4_should_default_constructor_with_0s)
{
    constexpr IPv4Address addr {};

    static_assert(addr.is_zero());

    EXPECT(addr.is_zero());
}

TEST_CASE(ipv4_should_construct_from_c_array)
{
    constexpr auto addr = [] {
        u8 const a[4] = { 1, 2, 3, 4 };
        return IPv4Address(a);
    }();

    static_assert(!addr.is_zero());

    EXPECT(!addr.is_zero());
}

TEST_CASE(ipv4_should_construct_from_u32)
{
    constexpr auto addr = [] {
        NetworkOrdered<u32> const a = 0x11'22'33'44;
        return IPv4Address(a);
    }();

    static_assert(!addr.is_zero());

    EXPECT(!addr.is_zero());
}

TEST_CASE(ipv4_should_get_octets_by_byte_offset)
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

TEST_CASE(ipv4_should_convert_to_string)
{
    constexpr IPv4Address addr(1, 25, 39, 42);

    EXPECT_EQ("1.25.39.42", addr.to_byte_string());
}

TEST_CASE(ipv4_should_make_ipv4_address_from_string)
{
    auto const addr = IPv4Address::from_string("192.168.0.1"sv);

    EXPECT(addr.has_value());
    EXPECT_EQ(192, addr.value()[0]);
    EXPECT_EQ(168, addr.value()[1]);
    EXPECT_EQ(0, addr.value()[2]);
    EXPECT_EQ(1, addr.value()[3]);
}

TEST_CASE(ipv4_should_make_empty_optional_from_bad_string)
{
    auto const addr = IPv4Address::from_string("bad string"sv);

    EXPECT(!addr.has_value());
}

TEST_CASE(ipv4_should_make_empty_optional_from_out_of_range_values)
{
    auto const addr = IPv4Address::from_string("192.168.0.500"sv);

    EXPECT(!addr.has_value());
}

TEST_CASE(ipv4_should_fill_d_octet_from_1_part)
{
    auto const addr = IPv4Address::from_string("1"sv);

    EXPECT(addr.has_value());
    EXPECT_EQ(0, addr.value()[0]);
    EXPECT_EQ(0, addr.value()[1]);
    EXPECT_EQ(0, addr.value()[2]);
    EXPECT_EQ(1, addr.value()[3]);
}

TEST_CASE(ipv4_should_fill_a_and_d_octets_from_2_parts)
{
    auto const addr = IPv4Address::from_string("192.1"sv);

    EXPECT(addr.has_value());
    EXPECT_EQ(192, addr.value()[0]);
    EXPECT_EQ(0, addr.value()[1]);
    EXPECT_EQ(0, addr.value()[2]);
    EXPECT_EQ(1, addr.value()[3]);
}

TEST_CASE(ipv4_should_fill_a_b_d_octets_from_3_parts)
{
    auto const addr = IPv4Address::from_string("192.168.1"sv);

    EXPECT(addr.has_value());
    EXPECT_EQ(192, addr.value()[0]);
    EXPECT_EQ(168, addr.value()[1]);
    EXPECT_EQ(0, addr.value()[2]);
    EXPECT_EQ(1, addr.value()[3]);
}

TEST_CASE(ipv4_should_convert_to_in_addr_t)
{
    constexpr IPv4Address addr(1, 2, 3, 4);

    static_assert(0x04'03'02'01u == addr.to_in_addr_t());

    EXPECT_EQ(0x04'03'02'01u, addr.to_in_addr_t());
}

TEST_CASE(ipv4_should_convert_to_u32)
{
    constexpr IPv4Address addr(1, 2, 3, 4);

    static_assert(0x04'03'02'01u == addr.to_in_addr_t());

    EXPECT_EQ(0x04'03'02'01u, addr.to_u32());
}

TEST_CASE(ipv4_should_compare)
{
    constexpr IPv4Address addr_a(1, 2, 3, 4);
    constexpr IPv4Address addr_b(1, 2, 3, 5);

    static_assert(addr_a != addr_b);
    static_assert(addr_a == addr_a);

    EXPECT(addr_a != addr_b);
    EXPECT(addr_a == addr_a);
}

TEST_CASE(ipv4_netmask_from_cidr)
{
    EXPECT(IPv4Address::netmask_from_cidr(24) == IPv4Address(255, 255, 255, 0));

    EXPECT(IPv4Address::netmask_from_cidr(0) == IPv4Address(0, 0, 0, 0));
    EXPECT(IPv4Address::netmask_from_cidr(32) == IPv4Address(255, 255, 255, 255));

    EXPECT(IPv4Address::netmask_from_cidr(28) == IPv4Address(255, 255, 255, 240));
    EXPECT(IPv4Address::netmask_from_cidr(22) == IPv4Address(255, 255, 252, 0));
    EXPECT(IPv4Address::netmask_from_cidr(14) == IPv4Address(255, 252, 0, 0));
    EXPECT(IPv4Address::netmask_from_cidr(6) == IPv4Address(252, 0, 0, 0));
}

TEST_CASE(ipv6_should_default_constructor_with_0s)
{
    constexpr IPv6Address addr {};

    static_assert(addr.is_zero());

    EXPECT(addr.is_zero());
}

TEST_CASE(ipv6_should_construct_from_c_array)
{
    constexpr auto addr = [] {
        u8 const a[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
        return IPv6Address(a);
    }();

    static_assert(!addr.is_zero());

    EXPECT(!addr.is_zero());
}

TEST_CASE(ipv6_should_get_groups_by_index)
{
    constexpr IPv6Address addr({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 });

    static_assert(0x102 == addr[0]);
    static_assert(0x304 == addr[1]);
    static_assert(0x506 == addr[2]);
    static_assert(0x708 == addr[3]);
    static_assert(0x90a == addr[4]);
    static_assert(0xb0c == addr[5]);
    static_assert(0xd0e == addr[6]);
    static_assert(0xf10 == addr[7]);

    EXPECT_EQ(0x102, addr[0]);
    EXPECT_EQ(0x304, addr[1]);
    EXPECT_EQ(0x506, addr[2]);
    EXPECT_EQ(0x708, addr[3]);
    EXPECT_EQ(0x90a, addr[4]);
    EXPECT_EQ(0xb0c, addr[5]);
    EXPECT_EQ(0xd0e, addr[6]);
    EXPECT_EQ(0xf10, addr[7]);
}

TEST_CASE(ipv6_should_convert_to_string)
{
    auto to_string = [&](IPv6Address::in6_addr_t const& data) -> String {
        return MUST(IPv6Address(data).to_string());
    };

    EXPECT_EQ("102:304:506:708:90a:b0c:d0e:f10"sv, to_string({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }));
    EXPECT_EQ("::"sv, MUST(IPv6Address().to_string()));
    EXPECT_EQ("::1"sv, to_string({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }));
    EXPECT_EQ("1::"sv, to_string({ 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }));
    EXPECT_EQ("102:0:506:708:900::10"sv, to_string({ 1, 2, 0, 0, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 16 }));
    EXPECT_EQ("102:0:506:708:900::"sv, to_string({ 1, 2, 0, 0, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0 }));
    EXPECT_EQ("::304:506:708:90a:b0c:d0e:f10"sv, to_string({ 0, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }));
    EXPECT_EQ("102:304::708:90a:b0c:d0e:f10"sv, to_string({ 1, 2, 3, 4, 0, 0, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }));
}

TEST_CASE(ipv6_should_make_ipv6_address_from_string)
{
    EXPECT(!IPv6Address::from_string(":::"sv).has_value());
    EXPECT(!IPv6Address::from_string(":::1"sv).has_value());
    EXPECT(!IPv6Address::from_string("1:::"sv).has_value());
    EXPECT_EQ(IPv6Address::from_string("102:304:506:708:90a:b0c:d0e:f10"sv).value(), IPv6Address({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }));
    EXPECT_EQ(IPv6Address::from_string("::"sv).value(), IPv6Address());
    EXPECT_EQ(IPv6Address::from_string("::1"sv).value(), IPv6Address({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }));
    EXPECT_EQ(IPv6Address::from_string("1::"sv).value(), IPv6Address({ 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }));
    EXPECT_EQ(IPv6Address::from_string("102:0:506:708:900::10"sv).value(), IPv6Address({ 1, 2, 0, 0, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 16 }));
    EXPECT_EQ(IPv6Address::from_string("102:0:506:708:900::"sv).value(), IPv6Address({ 1, 2, 0, 0, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0 }));
    EXPECT_EQ(IPv6Address::from_string("::304:506:708:90a:b0c:d0e:f10"sv).value(), IPv6Address({ 0, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }));
    EXPECT_EQ(IPv6Address::from_string("102:304::708:90a:b0c:d0e:f10"sv).value(), IPv6Address({ 1, 2, 3, 4, 0, 0, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }));
    EXPECT_EQ(IPv6Address::from_string("[102:304::708:90a:b0c:d0e:f10]"sv).value(), IPv6Address({ 1, 2, 3, 4, 0, 0, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }));
    EXPECT_EQ(IPv6Address::from_string("[::304:506:708:90a:b0c:d0e:f10]"sv).value(), IPv6Address({ 0, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }));
}

TEST_CASE(ipv6_should_not_make_ipv6_address_from_string)
{
    EXPECT_EQ(IPv6Address::from_string("[102:0:506:708:900::10"sv), OptionalNone {});
    EXPECT_EQ(IPv6Address::from_string("102:0:506:708:900::10]"sv), OptionalNone {});
    EXPECT_EQ(IPv6Address::from_string("[::304:506:708:90a:b0c:d0e:f10]]"sv), OptionalNone {});
    EXPECT_EQ(IPv6Address::from_string("[[::304:506:708:90a:b0c:d0e:f10]"sv), OptionalNone {});
    EXPECT_EQ(IPv6Address::from_string("[[::304:506:708:90a:b0c:d0e:f10]]"sv), OptionalNone {});
}

TEST_CASE(ipv6_ipv4_mapped_ipv6)
{
    auto ipv4_address_to_map = IPv4Address::from_string("192.168.0.1"sv).release_value();
    IPv6Address mapped_address(ipv4_address_to_map);
    EXPECT(mapped_address.is_ipv4_mapped());
    EXPECT_EQ(ipv4_address_to_map, mapped_address.ipv4_mapped_address().value());
    EXPECT_EQ("::ffff:192.168.0.1"sv, MUST(mapped_address.to_string()));
    EXPECT_EQ(IPv4Address(192, 168, 1, 9), IPv6Address::from_string("::FFFF:192.168.1.9"sv).value().ipv4_mapped_address().value());
    EXPECT_EQ(IPv4Address(192, 168, 1, 7), IPv6Address::from_string("::0:0:ffff:192.168.1.7"sv).value().ipv4_mapped_address().value());
    EXPECT_EQ(IPv4Address(192, 168, 2, 5), IPv6Address::from_string("0:0:0::ffff:192.168.2.5"sv).value().ipv4_mapped_address().value());
    EXPECT(!IPv6Address::from_string("::abcd:192.168.1.9"sv).has_value());
    EXPECT(!IPv6Address::from_string("::::ffff:192.168.1.9"sv).has_value());
    EXPECT(!IPv6Address::from_string("ffff::ffff:192.168.1.9"sv).has_value());
    EXPECT(!IPv6Address::from_string("0::0::ffff:192.168.1.9"sv).has_value());
    EXPECT(!IPv6Address::from_string("::0::ffff:192.168.1.9"sv).has_value());
    EXPECT(!IPv6Address::from_string(":0:ffff:192.168.1.9"sv).has_value());
}

TEST_CASE(ipv6_should_make_empty_optional_from_bad_string)
{
    auto const addr = IPv6Address::from_string("bad string"sv);

    EXPECT(!addr.has_value());
}

TEST_CASE(ipv6_should_make_empty_optional_from_out_of_range_values)
{
    auto const addr = IPv6Address::from_string("::10000"sv);

    EXPECT(!addr.has_value());
}

TEST_CASE(ipv6_should_only_compare_bytes_from_address)
{
    constexpr IPv6Address addr_a({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 });
    constexpr IPv6Address addr_b({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17 });
    constexpr IPv6Address addr_c({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17 });

    static_assert(addr_a != addr_b);
    static_assert(addr_a == addr_a);
    static_assert(addr_b == addr_c);

    EXPECT(addr_a != addr_b);
    EXPECT(addr_a == addr_a);
    EXPECT(addr_b == addr_c);
}
