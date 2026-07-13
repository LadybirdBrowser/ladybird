/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>

namespace Web {

inline Utf16String utf16_string_from_url_ascii(String const& string)
{
    return Utf16String::from_ascii_without_validation(string.bytes());
}

inline Utf16String utf16_string_from_url_ascii(StringView string)
{
    return Utf16String::from_ascii_without_validation(string.bytes());
}

inline Utf16String utf16_string_from_url_ascii_with_prefix(char prefix, StringView string)
{
    Utf16StringBuilder builder { string.length() + 1 };
    builder.append_ascii(prefix);
    builder.append_ascii(string);
    return builder.to_string();
}

inline Utf16String utf16_string_from_url_ascii_with_prefix(char prefix, String const& string)
{
    return utf16_string_from_url_ascii_with_prefix(prefix, string.bytes_as_string_view());
}

inline Utf16String utf16_string_from_url_ascii_host_and_port(StringView host, u16 port)
{
    Utf16StringBuilder builder;
    builder.append_ascii(host);
    builder.append_ascii(':');
    builder.appendff("{}", port);
    return builder.to_string();
}

inline Utf16String utf16_string_from_url_ascii_host_and_port(String const& host, u16 port)
{
    return utf16_string_from_url_ascii_host_and_port(host.bytes_as_string_view(), port);
}

}
