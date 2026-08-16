/*
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/CustomElements/CustomElementName.h>

namespace Web::HTML {

bool is_valid_custom_element_name(Utf16View const& name)
{
    // OPTIMIZATION: A valid custom element name must contain a hyphen. Check this first so ordinary built-in element
    //               names do not need the more expensive element local name validation below.
    if (!name.contains('-'))
        return false;

    // A string name is a valid custom element name if all of the following are true:
    // - name is a valid element local name;
    if (!DOM::is_valid_element_local_name(name))
        return false;

    // - name's 0th code point is an ASCII lower alpha;
    if (auto first = name.begin(); !is_ascii_lower_alpha(*first))
        return false;

    // - name does not contain any ASCII upper alphas;
    // - name contains a U+002D (-); and
    bool contains_ascii_upper_alpha = false;
    for (auto code_point : name) {
        if (is_ascii_upper_alpha(code_point)) {
            contains_ascii_upper_alpha = true;
            break;
        }
    }
    if (contains_ascii_upper_alpha)
        return false;

    // - name is not one of the following:
    //   - "annotation-xml"
    //   - "color-profile"
    //   - "font-face"
    //   - "font-face-src"
    //   - "font-face-uri"
    //   - "font-face-format"
    //   - "font-face-name"
    //   - "missing-glyph"
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

    return true;
}

}
