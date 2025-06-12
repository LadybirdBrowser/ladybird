/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PseudoElementParsing.h"
#include <LibWeb/CSS/Parser/Parser.h>

namespace Web::Animations {

// https://drafts.csswg.org/web-animations-1/#dom-keyframeeffect-pseudo-element-parsing
WebIDL::ExceptionOr<Optional<CSS::Selector::PseudoElementSelector>> pseudo_element_parsing(JS::Realm& realm, Optional<String> const& value)
{
    // 1. Given the value value, perform the following steps:

    // 2. If value is not null and is an invalid <pseudo-element-selector>,
    Optional<CSS::Selector::PseudoElementSelector> pseudo_element;
    if (value.has_value()) {
        pseudo_element = parse_pseudo_element_selector(CSS::Parser::ParsingParams { realm }, *value);
        if (!pseudo_element.has_value()) {
            // 1. Throw a DOMException with error name "SyntaxError".
            // 2. Abort.
            return WebIDL::SyntaxError::create(realm, MUST(String::formatted("Invalid pseudo-element selector: \"{}\"", value.value())));
        }
    }

    // 3. If value is one of the legacy Selectors Level 2 single-colon selectors (':before', ':after', ':first-letter', or ':first-line'),
    // then return the equivalent two-colon selector (e.g. '::before').
    if (value.has_value() && value->is_one_of(":before", ":after", ":first-letter", ":first-line")) {
        return CSS::pseudo_element_from_string(MUST(value->substring_from_byte_offset(1)));
    }

    // 4. Otherwise, return value.
    return pseudo_element;
}

}
