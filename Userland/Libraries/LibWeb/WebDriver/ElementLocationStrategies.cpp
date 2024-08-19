/*
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/WebDriver/ElementLocationStrategies.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#css-selectors
static ErrorOr<GC::Ref<DOM::NodeList>, Error> locate_element_by_css_selector(DOM::ParentNode& start_node, StringView selector)
{
    // 1. Let elements be the result of calling querySelectorAll() with start node as this and selector as the argument.
    //    If this causes an exception to be thrown, return error with error code invalid selector.
    auto elements = start_node.query_selector_all(selector);
    if (elements.is_exception())
        return Error::from_code(ErrorCode::InvalidSelector, "querySelectorAll() failed"sv);

    // 2.Return success with data elements.
    return elements.release_value();
}

// https://w3c.github.io/webdriver/#link-text
static ErrorOr<GC::Ref<DOM::NodeList>, Error> locate_element_by_link_text(DOM::ParentNode&, StringView)
{
    return Error::from_code(ErrorCode::UnsupportedOperation, "Not implemented: locate element by link text"sv);
}

// https://w3c.github.io/webdriver/#partial-link-text
static ErrorOr<GC::Ref<DOM::NodeList>, Error> locate_element_by_partial_link_text(DOM::ParentNode&, StringView)
{
    return Error::from_code(ErrorCode::UnsupportedOperation, "Not implemented: locate element by partial link text"sv);
}

// https://w3c.github.io/webdriver/#tag-name
static ErrorOr<GC::Ref<DOM::NodeList>, Error> locate_element_by_tag_name(DOM::ParentNode&, StringView)
{
    return Error::from_code(ErrorCode::UnsupportedOperation, "Not implemented: locate element by tag name"sv);
}

// https://w3c.github.io/webdriver/#xpath
static ErrorOr<GC::Ref<DOM::NodeList>, Error> locate_element_by_x_path(DOM::ParentNode&, StringView)
{
    return Error::from_code(ErrorCode::UnsupportedOperation, "Not implemented: locate element by XPath"sv);
}

Optional<LocationStrategy> location_strategy_from_string(StringView type)
{
    if (type == "css selector"sv)
        return LocationStrategy::CssSelector;
    if (type == "link text"sv)
        return LocationStrategy::LinkText;
    if (type == "partial link text"sv)
        return LocationStrategy::PartialLinkText;
    if (type == "tag name"sv)
        return LocationStrategy::TagName;
    if (type == "xpath"sv)
        return LocationStrategy::XPath;
    return {};
}

ErrorOr<GC::Ref<DOM::NodeList>, Error> invoke_location_strategy(LocationStrategy type, DOM::ParentNode& start_node, StringView selector)
{
    switch (type) {
    case LocationStrategy::CssSelector:
        return locate_element_by_css_selector(start_node, selector);
    case LocationStrategy::LinkText:
        return locate_element_by_link_text(start_node, selector);
    case LocationStrategy::PartialLinkText:
        return locate_element_by_partial_link_text(start_node, selector);
    case LocationStrategy::TagName:
        return locate_element_by_tag_name(start_node, selector);
    case LocationStrategy::XPath:
        return locate_element_by_x_path(start_node, selector);
    }

    VERIFY_NOT_REACHED();
}

}
