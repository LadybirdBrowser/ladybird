/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Parser/SpeculativeMockElement.h>

namespace Web::HTML {

Optional<String> SpeculativeMockElement::attribute(FlyString const& name) const
{
    for (auto const& attribute : attribute_list) {
        if (attribute.local_name == name)
            return attribute.value;
    }
    return {};
}

// https://html.spec.whatwg.org/multipage/parsing.html#create-a-speculative-mock-element
SpeculativeMockElement create_a_speculative_mock_element(FlyString tag_name, Vector<HTMLToken::Attribute> attributes)
{
    // 1. Let element be a new speculative mock element.
    // 2. FIXME: Set element's namespace to namespace.
    // 3. Set element's local name to tagName.
    // 4. Set element's attribute list to attributes.
    // 5. FIXME: Set element's children to a new empty list.
    // 6. Optionally, perform a speculative fetch for element.
    // The speculative fetch is performed by the caller (see SpeculativeHTMLParser::process_start_tag).

    // 7. Return element.
    return SpeculativeMockElement {
        .local_name = move(tag_name),
        .attribute_list = move(attributes),
    };
}

}
