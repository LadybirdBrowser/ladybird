/*
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/CustomElements/CustomElementName.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#valid-custom-element-name
bool is_valid_custom_element_name(String const& name)
{
    // A string name is a valid custom element name if all of the following are true:
    // - name is a valid element local name;
    if (!DOM::is_valid_element_local_name(name))
        return false;

    // - name's 0th code point is an ASCII lower alpha;
    auto code_points = Utf8View { name };
    if (auto first = code_points.begin(); first.done() || !is_ascii_lower_alpha(*first))
        return false;

    // - name does not contain any ASCII upper alphas;
    // - name contains a U+002D (-); and
    bool contains_ascii_upper_alpha = false;
    bool contains_hyphen = false;
    for (auto code_point : code_points) {
        if (is_ascii_upper_alpha(code_point)) {
            contains_ascii_upper_alpha = true;
            break;
        }
        if (code_point == '-')
            contains_hyphen = true;
    }
    if (contains_ascii_upper_alpha || !contains_hyphen)
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
