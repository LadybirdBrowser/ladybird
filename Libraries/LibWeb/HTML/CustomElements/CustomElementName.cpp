/*
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Utf8View.h>
#include <LibWeb/HTML/CustomElements/CustomElementName.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#custom-elements-core-concepts:prod-pcenchar
static bool is_pcen_char(u32 code_point)
{
    return code_point == '-'
        || code_point == '.'
        || (code_point >= '0' && code_point <= '9')
        || code_point == '_'
        || (code_point >= 'a' && code_point <= 'z')
        || code_point == 0xb7
        || (code_point >= 0xc0 && code_point <= 0xd6)
        || (code_point >= 0xd8 && code_point <= 0xf6)
        || (code_point >= 0xf8 && code_point <= 0x37d)
        || (code_point >= 0x37f && code_point <= 0x1fff)
        || (code_point >= 0x200c && code_point <= 0x200d)
        || (code_point >= 0x203f && code_point <= 0x2040)
        || (code_point >= 0x2070 && code_point <= 0x218f)
        || (code_point >= 0x2c00 && code_point <= 0x2fef)
        || (code_point >= 0x3001 && code_point <= 0xD7ff)
        || (code_point >= 0xf900 && code_point <= 0xfdcf)
        || (code_point >= 0xfdf0 && code_point <= 0xfffd)
        || (code_point >= 0x10000 && code_point <= 0xeffff);
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#valid-custom-element-name
bool is_valid_custom_element_name(StringView name)
{
    // name must not be any of the following:
    if (name.is_one_of(
            "annotation-xml"sv,
            "color-profile"sv,
            "font-face"sv,
            "font-face-src"sv,
            "font-face-uri"sv,
            "font-face-format"sv,
            "font-face-name"sv,
            "missing-glyph"sv)) {
        return false;
    }

    // name must match the PotentialCustomElementName production:
    // PotentialCustomElementName ::=
    //      [a-z] (PCENChar)* '-' (PCENChar)*

    auto code_points = Utf8View { name };
    auto it = code_points.begin();

    if (code_points.is_empty() || *it < 'a' || *it > 'z')
        return false;

    ++it;

    bool found_hyphen = false;

    for (; it != code_points.end(); ++it) {
        if (!is_pcen_char(*it))
            return false;
        if (*it == '-')
            found_hyphen = true;
    }

    return found_hyphen;
}

}
