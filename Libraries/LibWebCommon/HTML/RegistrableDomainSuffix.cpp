/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/RegistrableDomainSuffix.h>

namespace Web::DOM {

// https://html.spec.whatwg.org/multipage/browsers.html#is-a-registrable-domain-suffix-of-or-is-equal-to
bool is_a_registrable_domain_suffix_of_or_is_equal_to(Utf16View host_suffix_string, URL::Host const& original_host)
{
    // 1. If hostSuffixString is the empty string, then return false.
    if (host_suffix_string.is_empty())
        return false;

    // 2. Let hostSuffix be the result of parsing hostSuffixString.
    auto host_suffix = URL::Parser::parse_host(host_suffix_string);

    // 3. If hostSuffix is failure, then return false.
    if (!host_suffix.has_value())
        return false;

    return is_a_registrable_domain_suffix_of_or_is_equal_to(host_suffix.value(), original_host);
}

static bool ends_with_dot_prefixed_suffix(Utf16View string, Utf16View suffix)
{
    if (string.length_in_code_units() <= suffix.length_in_code_units())
        return false;
    if (!string.ends_with(suffix))
        return false;
    return string.code_unit_at(string.length_in_code_units() - suffix.length_in_code_units() - 1) == '.';
}

bool is_a_registrable_domain_suffix_of_or_is_equal_to(URL::Host const& host_suffix, URL::Host const& original_host)
{
    // 4. If hostSuffix does not equal originalHost, then:
    if (host_suffix != original_host) {
        // 1. If hostSuffix or originalHost is not a domain, then return false.
        // NOTE: This excludes hosts that are IP addresses.
        if (!host_suffix.has<String>() || !original_host.has<String>())
            return false;
        auto const& host_suffix_string = host_suffix.get<String>();
        auto const& original_host_string = original_host.get<String>();

        // 2. If hostSuffix, prefixed by U+002E (.), does not match the end of originalHost, then return false.
        if (!ends_with_dot_prefixed_suffix(Utf16View::from_ascii(original_host_string.bytes_as_string_view()), Utf16View::from_ascii(host_suffix_string.bytes_as_string_view())))
            return false;

        // 3. If any of the following are true:
        //     * hostSuffix equals hostSuffix's public suffix; or
        //     * hostSuffix, prefixed by U+002E (.), matches the end of originalHost's public suffix,
        //    then return false. [URL]
        if (host_suffix_string == host_suffix.public_suffix())
            return false;

        auto original_host_public_suffix = original_host.public_suffix();
        VERIFY(original_host_public_suffix.has_value());

        if (ends_with_dot_prefixed_suffix(Utf16View::from_ascii(original_host_public_suffix->bytes_as_string_view()), Utf16View::from_ascii(host_suffix_string.bytes_as_string_view())))
            return false;

        // 4. Assert: originalHost's public suffix, prefixed by U+002E (.), matches the end of hostSuffix.
        VERIFY(ends_with_dot_prefixed_suffix(Utf16View::from_ascii(host_suffix_string.bytes_as_string_view()), Utf16View::from_ascii(original_host_public_suffix->bytes_as_string_view())));
    }

    // 5. Return true.
    return true;
}

}
