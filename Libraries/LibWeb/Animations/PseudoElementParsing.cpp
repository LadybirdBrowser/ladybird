/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PseudoElementParsing.h"
#include <LibWeb/CSS/Parser/Parser.h>

namespace Web::Animations {

static WebIDL::ExceptionOr<Optional<CSS::Selector::PseudoElementSelector>> pseudo_element_parsing_impl(Optional<Utf16String> const& value)
{
    Optional<CSS::Selector::PseudoElementSelector> pseudo_element;
    if (value.has_value()) {
        pseudo_element = parse_pseudo_element_selector(CSS::Parser::ParsingParams {}, *value);
        if (!pseudo_element.has_value())
            return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid pseudo-element selector: \"{}\"", value.value()));
    }
    return pseudo_element;
}

WebIDL::ExceptionOr<Optional<CSS::Selector::PseudoElementSelector>> pseudo_element_parsing(Optional<Utf16String> const& value)
{
    return pseudo_element_parsing_impl(value);
}

}
