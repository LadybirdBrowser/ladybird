/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Host.h>
#include <LibURL/URL.h>

namespace URL {

Host::Host(VariantType&& value)
    : m_value(move(value))
{
}

Host::Host(String&& string)
    : m_value(move(string))
{
}

// https://url.spec.whatwg.org/#concept-ipv4-serializer
static String serialize_ipv4_address(IPv4Address address)
{
    // 1. Let output be the empty string.
    // NOTE: Array to avoid prepend.
    Array<u8, 4> output;

    // 2. Let n be the value of address.
    u32 n = address;

    // 3. For each i in the range 1 to 4, inclusive:
    for (size_t i = 0; i <= 3; ++i) {
        // 1. Prepend n % 256, serialized, to output.
        output[3 - i] = n % 256;

        // 2. If i is not 4, then prepend U+002E (.) to output.
        // NOTE: done at end

        // 3. Set n to floor(n / 256).
        n /= 256;
    }

    // 4. Return output.
    return MUST(String::formatted("{}.{}.{}.{}", output[0], output[1], output[2], output[3]));
}

// https://url.spec.whatwg.org/#find-the-ipv6-address-compressed-piece-index
static Optional<size_t> find_the_ipv6_address_compressed_piece_index(IPv6Address const& address)
{
    // 1. Let longestIndex be null.
    Optional<size_t> longest_index;

    // 2. Let longestSize be 1.
    size_t longest_size = 1;

    // 3. Let foundIndex be null.
    Optional<size_t> found_index;

    // 4. Let foundSize be 0.
    size_t found_size = 0;

    // 5. For each pieceIndex of address’s pieces’s indices:
    for (size_t piece_index = 0; piece_index < address.size(); ++piece_index) {
        // 1. If address’s pieces[pieceIndex] is not 0:
        if (address[piece_index] != 0) {
            // 1. If foundSize is greater than longestSize, then set longestIndex to foundIndex and longestSize to foundSize.
            if (found_size > longest_size) {
                longest_index = found_index;
                longest_size = found_size;
            }

            // 2. Set foundIndex to null.
            found_index = {};

            // 3. Set foundSize to 0.
            found_size = 0;
        }
        // 2. Otherwise:
        else {
            // 1. If foundIndex is null, then set foundIndex to pieceIndex.
            if (!found_index.has_value())
                found_index = piece_index;

            // 2. Increment foundSize by 1.
            ++found_size;
        }
    }

    // 6. If foundSize is greater than longestSize, then return foundIndex.
    if (found_size > longest_size)
        return found_index;

    // 7. Return longestIndex.
    return longest_index;
}

// https://url.spec.whatwg.org/#concept-ipv6-serializer
static void serialize_ipv6_address(IPv6Address const& address, StringBuilder& output)
{
    // 1. Let output be the empty string.

    // 2. Let compress be the result of finding the IPv6 address compressed piece index given address.
    auto compress = find_the_ipv6_address_compressed_piece_index(address);

    // 3. Let ignore0 be false.
    auto ignore0 = false;

    // 4. For each pieceIndex of address’s pieces’s indices:
    for (size_t piece_index = 0; piece_index < address.size(); ++piece_index) {
        // 1. If ignore0 is true and address[pieceIndex] is 0, then continue.
        if (ignore0 && address[piece_index] == 0)
            continue;

        // 2. Otherwise, if ignore0 is true, set ignore0 to false.
        if (ignore0)
            ignore0 = false;

        // 3. If compress is pieceIndex, then:
        if (compress == piece_index) {
            // 1. Let separator be "::" if pieceIndex is 0, and U+003A (:) otherwise.
            auto separator = piece_index == 0 ? "::"sv : ":"sv;

            // 2. Append separator to output.
            output.append(separator);

            // 3. Set ignore0 to true and continue.
            ignore0 = true;
            continue;
        }

        // 4. Append address[pieceIndex], represented as the shortest possible lowercase hexadecimal number, to output.
        output.appendff("{:x}", address[piece_index]);

        // 5. If pieceIndex is not 7, then append U+003A (:) to output.
        if (piece_index != 7)
            output.append(':');
    }

    // 5. Return output.
}

// https://url.spec.whatwg.org/#concept-domain
bool Host::is_domain() const
{
    // A domain is a non-empty ASCII string that identifies a realm within a network.
    return m_value.has<String>() && !m_value.get<String>().is_empty();
}

// https://url.spec.whatwg.org/#empty-host
bool Host::is_empty_host() const
{
    // An empty host is the empty string.
    return m_value.has<String>() && m_value.get<String>().is_empty();
}

// https://url.spec.whatwg.org/#concept-host-serializer
String Host::serialize() const
{
    return m_value.visit(
        // 1. If host is an IPv4 address, return the result of running the IPv4 serializer on host.
        [](IPv4Address const& address) {
            return serialize_ipv4_address(address);
        },
        // 2. Otherwise, if host is an IPv6 address, return U+005B ([), followed by the result of running the IPv6 serializer on host, followed by U+005D (]).
        [](IPv6Address const& address) {
            StringBuilder output;
            output.append('[');
            serialize_ipv6_address(address, output);
            output.append(']');
            return output.to_string_without_validation();
        },
        // 3. Otherwise, host is a domain, opaque host, or empty host, return host.
        [](String const& string) {
            return string;
        });
}

// https://url.spec.whatwg.org/#host-public-suffix
Optional<String> Host::public_suffix() const
{
    // 1. If host is not a domain, then return null.
    if (!is_domain())
        return OptionalNone {};

    auto const& host_string = m_value.get<String>();

    // 2. Let trailingDot be "." if host ends with "."; otherwise the empty string.
    auto trailing_dot = host_string.ends_with('.') ? "."sv : ""sv;

    // 3. Let publicSuffix be the public suffix determined by running the Public Suffix List algorithm with host as domain. [PSL]
    // NOTE: The spec algorithm for the public suffix returns "*" by default, but get_public_suffix() returns an empty Optional.
    //       Remove the `value_or()` if and when we update it.
    auto public_suffix = get_public_suffix(host_string.bytes_as_string_view()).value_or("*"_string);

    // 4. Assert: publicSuffix is an ASCII string that does not end with ".".
    VERIFY(all_of(public_suffix.code_points(), is_ascii));
    VERIFY(!public_suffix.ends_with('.'));

    // 5. Return publicSuffix and trailingDot concatenated.
    return MUST(String::formatted("{}{}", public_suffix, trailing_dot));
}

// https://url.spec.whatwg.org/#host-registrable-domain
Optional<String> Host::registrable_domain() const
{
    // 1. If host’s public suffix is null or host’s public suffix equals host, then return null.
    auto public_suffix = this->public_suffix();
    if (!public_suffix.has_value() || public_suffix == m_value.get<String>())
        return OptionalNone {};

    // NOTE: If we got here, we know this Host is a String.
    auto const& host_string = m_value.get<String>();

    // 2. Let trailingDot be "." if host ends with "."; otherwise the empty string.
    auto trailing_dot = host_string.ends_with('.') ? "."sv : ""sv;

    // 3. Let registrableDomain be the registrable domain determined by running the Public Suffix List algorithm with host as domain. [PSL]
    // NOTE: The spec algorithm for the public suffix returns "*" by default, but get_public_suffix() returns an empty Optional.
    //       Remove the `value_or()` if and when we update it.
    auto registrable_domain = get_registrable_domain(host_string).value_or("*"_string);

    // 4. Assert: registrableDomain is an ASCII string that does not end with ".".
    VERIFY(all_of(registrable_domain.code_points(), is_ascii));
    VERIFY(!registrable_domain.ends_with('.'));

    // 5. Return registrableDomain and trailingDot concatenated.
    return MUST(String::formatted("{}{}", registrable_domain, trailing_dot));
}

}
