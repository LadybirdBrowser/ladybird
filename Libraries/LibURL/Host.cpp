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

// https://url.spec.whatwg.org/#concept-ipv6-serializer
static void serialize_ipv6_address(IPv6Address const& address, StringBuilder& output)
{
    // 1. Let output be the empty string.

    // 2. Let compress be an index to the first IPv6 piece in the first longest sequences of address’s IPv6 pieces that are 0.
    Optional<size_t> compress;
    size_t longest_sequence_length = 0;
    size_t current_sequence_length = 0;
    size_t current_sequence_start = 0;
    for (size_t i = 0; i < 8; ++i) {
        if (address[i] == 0) {
            if (current_sequence_length == 0)
                current_sequence_start = i;
            ++current_sequence_length;
        } else {
            if (current_sequence_length > longest_sequence_length) {
                longest_sequence_length = current_sequence_length;
                compress = current_sequence_start;
            }
            current_sequence_length = 0;
        }
    }

    if (current_sequence_length > longest_sequence_length) {
        longest_sequence_length = current_sequence_length;
        compress = current_sequence_start;
    }

    // 3. If there is no sequence of address’s IPv6 pieces that are 0 that is longer than 1, then set compress to null.
    if (longest_sequence_length <= 1)
        compress = {};

    // 4. Let ignore0 be false.
    auto ignore0 = false;

    // 5. For each pieceIndex in the range 0 to 7, inclusive:
    for (size_t piece_index = 0; piece_index <= 7; ++piece_index) {
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

    // 6. Return output.
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
    auto public_suffix = get_public_suffix(host_string.bytes_as_string_view());

    // NOTE: get_public_suffix() returns Optional, but this algorithm assumes a value. Is that OK?
    VERIFY(public_suffix.has_value());

    // 4. Assert: publicSuffix is an ASCII string that does not end with ".".
    VERIFY(all_of(public_suffix->code_points(), is_ascii));
    VERIFY(!public_suffix->ends_with('.'));

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
    auto registrable_domain = get_public_suffix(host_string);

    // NOTE: get_public_suffix() returns Optional, but this algorithm assumes a value. Is that OK?
    VERIFY(registrable_domain.has_value());

    // 4. Assert: registrableDomain is an ASCII string that does not end with ".".
    VERIFY(all_of(registrable_domain->code_points(), is_ascii));
    VERIFY(!registrable_domain->ends_with('.'));

    // 5. Return registrableDomain and trailingDot concatenated.
    return MUST(String::formatted("{}{}", registrable_domain, trailing_dot));
}

}
