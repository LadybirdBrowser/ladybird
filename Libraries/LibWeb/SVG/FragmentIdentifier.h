/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibURL/URL.h>

namespace Web::SVG {

inline Utf16String decode_fragment_identifier(StringView fragment)
{
    return Utf16String::from_utf8_with_replacement_character(URL::percent_decode(fragment), Utf16String::WithBOMHandling::No);
}

inline Utf16String decode_fragment_identifier(Utf16View fragment)
{
    if (!fragment.contains('%'))
        return Utf16String::from_utf16(fragment);
    if (fragment.has_ascii_storage())
        return decode_fragment_identifier(fragment.bytes());
    return decode_fragment_identifier(URL::percent_encode(fragment, URL::PercentEncodeSet::C0Control));
}

}
