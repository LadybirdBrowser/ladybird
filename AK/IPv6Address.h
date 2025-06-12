/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Endian.h>
#include <AK/Format.h>
#include <AK/IPv4Address.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace AK {

class [[gnu::packed]] IPv6Address {
public:
    using in6_addr_t = u8[16];

    constexpr IPv6Address() = default;

    constexpr IPv6Address(in6_addr_t const& data)
    {
        for (size_t i = 0; i < 16; i++)
            m_data[i] = data[i];
    }

    constexpr IPv6Address(Array<u8, 16> const& data)
    {
        for (size_t i = 0; i < 16; i++)
            m_data[i] = data[i];
    }

    template<SameAs<char const*> T>
    constexpr IPv6Address(T const&) = delete; // Disable implicit conversion of char const* -> ipv4 -> ipv6

    constexpr IPv6Address(IPv4Address const& ipv4_address)
    {
        // IPv4 mapped IPv6 address
        m_data[10] = 0xff;
        m_data[11] = 0xff;
        m_data[12] = ipv4_address[0];
        m_data[13] = ipv4_address[1];
        m_data[14] = ipv4_address[2];
        m_data[15] = ipv4_address[3];
    }

    constexpr u16 operator[](int i) const { return group(i); }

    ErrorOr<String> to_string() const
    {
        if (is_zero()) {
            return "::"_string;
        }

        StringBuilder builder;

        if (is_ipv4_mapped()) {
            return String::formatted("::ffff:{}.{}.{}.{}", m_data[12], m_data[13], m_data[14], m_data[15]);
        }

        // Find the start of the longest span of 0 values
        Optional<int> longest_zero_span_start;
        int zero_span_length = 0;
        for (int i = 0; i < 8;) {
            if (group(i) != 0) {
                i++;
                continue;
            }
            int contiguous_zeros = 1;
            for (int j = i + 1; j < 8; j++) {
                if (group(j) != 0)
                    break;
                contiguous_zeros++;
            }

            if (!longest_zero_span_start.has_value() || longest_zero_span_start.value() < contiguous_zeros) {
                longest_zero_span_start = i;
                zero_span_length = contiguous_zeros;
            }

            i += contiguous_zeros;
        }

        for (int i = 0; i < 8;) {
            if (longest_zero_span_start.has_value() && longest_zero_span_start.value() == i) {
                if (longest_zero_span_start.value() + zero_span_length >= 8)
                    TRY(builder.try_append("::"sv));
                else
                    TRY(builder.try_append(':'));
                i += zero_span_length;
                continue;
            }

            if (i == 0)
                TRY(builder.try_appendff("{:x}", group(i)));
            else
                TRY(builder.try_appendff(":{:x}", group(i)));

            i++;
        }
        return builder.to_string();
    }

    static Optional<IPv6Address> from_string(StringView string)
    {
        if (string.is_null())
            return {};

        // NOTE: This supports URI syntax (square brackets) for IPv6 addresses.
        //       See: https://datatracker.ietf.org/doc/html/rfc3986#section-3.2.2
        auto const starts_with_bracket = string.starts_with('[');
        auto const ends_with_bracket = string.ends_with(']');

        if (starts_with_bracket || ends_with_bracket) {
            if (!starts_with_bracket || !ends_with_bracket)
                return {};

            string = string.substring_view(1, string.length() - 2);
        }

        auto const parts = string.split_view(':', SplitBehavior::KeepEmpty);
        if (parts.is_empty())
            return {};
        if (parts.size() > 9) {
            // We may have 9 parts if the address is compressed
            // at the beginning or end, e.g. by substituting the
            // leading or trailing 0 with a : character. Otherwise,
            // the maximum number of parts is 8, which we validate
            // when expanding the compression.
            return {};
        }
        if (parts.size() >= 4 && parts[parts.size() - 1].contains('.')) {
            // Check if this may be an ipv4 mapped address
            auto is_ipv4_prefix = [&]() {
                auto separator_part = parts[parts.size() - 2].trim_whitespace();
                if (separator_part.is_empty())
                    return false;
                auto separator_value = StringUtils::convert_to_uint_from_hex(separator_part);
                if (!separator_value.has_value() || separator_value.value() != 0xffff)
                    return false;
                // TODO: this allows multiple compression tags "::" in the prefix, which is technically not legal
                for (size_t i = 0; i < parts.size() - 2; i++) {
                    auto part = parts[i].trim_whitespace();
                    if (part.is_empty())
                        continue;
                    auto value = StringUtils::convert_to_uint_from_hex(part);
                    if (!value.has_value() || value.value() != 0)
                        return false;
                }
                return true;
            };

            if (is_ipv4_prefix()) {
                auto ipv4_address = IPv4Address::from_string(parts[parts.size() - 1]);
                if (ipv4_address.has_value())
                    return IPv6Address(ipv4_address.value());
                return {};
            }
        }

        in6_addr_t addr {};
        int group = 0;
        int have_groups = 0;
        bool found_compressed = false;
        for (size_t i = 0; i < parts.size();) {
            auto trimmed_part = parts[i].trim_whitespace();
            if (trimmed_part.is_empty()) {
                if (found_compressed)
                    return {};
                int empty_parts = 1;
                bool is_leading = (i == 0);
                bool is_trailing = false;
                for (size_t j = i + 1; j < parts.size(); j++) {
                    if (!parts[j].trim_whitespace().is_empty())
                        break;
                    empty_parts++;
                    if (j == parts.size() - 1)
                        is_trailing = true;
                }
                if (is_leading && is_trailing) {
                    if (empty_parts > 3)
                        return {};
                    return IPv6Address();
                }
                if (is_leading || is_trailing) {
                    if (empty_parts > 2)
                        return {};
                } else if (empty_parts > 1) {
                    return {};
                }

                int remaining_parts = parts.size() - empty_parts - have_groups;
                found_compressed = true;
                group = 8 - remaining_parts;
                VERIFY(group >= 0);
                i += empty_parts;
                continue;
            } else {
                i++;
            }
            auto part = StringUtils::convert_to_uint_from_hex(trimmed_part);
            if (!part.has_value() || part.value() > 0xffff)
                return {};

            if (++have_groups > 8)
                return {};

            VERIFY(group < 8);
            addr[group * sizeof(u16)] = (u8)(part.value() >> 8);
            addr[group * sizeof(u16) + 1] = (u8)part.value();
            group++;
        }

        return IPv6Address(addr);
    }

    constexpr in6_addr_t const& to_in6_addr_t() const { return m_data; }

    constexpr bool operator==(IPv6Address const& other) const = default;
    constexpr bool operator!=(IPv6Address const& other) const = default;

    constexpr bool is_zero() const
    {
        for (auto& d : m_data) {
            if (d != 0)
                return false;
        }
        return true;
    }

    constexpr bool is_ipv4_mapped() const
    {
        if (m_data[0] || m_data[1] || m_data[2] || m_data[3] || m_data[4] || m_data[5] || m_data[6] || m_data[7] || m_data[8] || m_data[9])
            return false;
        if (m_data[10] != 0xff || m_data[11] != 0xff)
            return false;
        return true;
    }

    Optional<IPv4Address> ipv4_mapped_address() const
    {
        if (is_ipv4_mapped())
            return IPv4Address(m_data[12], m_data[13], m_data[14], m_data[15]);
        return {};
    }

    // https://datatracker.ietf.org/doc/html/rfc4291#section-2.5.3
    [[nodiscard]] static IPv6Address loopback()
    {
        return IPv6Address({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 });
    }

private:
    constexpr u16 group(unsigned i) const
    {
        VERIFY(i < 8);
        return ((u16)m_data[i * sizeof(u16)] << 8) | m_data[i * sizeof(u16) + 1];
    }

    in6_addr_t m_data {};
};

static_assert(sizeof(IPv6Address) == 16);

template<>
struct Formatter<IPv6Address> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, IPv6Address const& value)
    {
        return Formatter<StringView>::format(builder, TRY(value.to_string()));
    }
};

}

#if USING_AK_GLOBALLY
using AK::IPv6Address;
#endif
