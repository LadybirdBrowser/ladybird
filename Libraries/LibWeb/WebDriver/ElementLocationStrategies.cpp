/*
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/StaticNodeList.h>
#include <LibWeb/WebDriver/ElementLocationStrategies.h>
#include <LibWeb/WebDriver/ElementReference.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#css-selectors
static ErrorOr<JS::NonnullGCPtr<DOM::NodeList>, Error> locate_element_by_css_selector(DOM::ParentNode& start_node, StringView selector)
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
static ErrorOr<JS::NonnullGCPtr<DOM::NodeList>, Error> locate_element_by_link_text(DOM::ParentNode& start_node, StringView selector)
{
    auto& realm = start_node.realm();

    // 1. Let elements be the result of calling querySelectorAll() with start node as this and "a" as the argument. If
    //    this throws an exception, return error with error code unknown error.
    auto elements = start_node.query_selector_all("a"sv);
    if (elements.is_exception())
        return Error::from_code(ErrorCode::UnknownError, "querySelectorAll() failed"sv);

    // 2. Let result be an empty NodeList.
    Vector<JS::Handle<DOM::Node>> result;

    // 3. For each element in elements:
    for (size_t i = 0; i < elements.value()->length(); ++i) {
        auto& element = const_cast<DOM::Node&>(*elements.value()->item(i));

        // 1. Let rendered text be the value that would be returned via a call to Get Element Text for element.
        auto rendered_text = element_rendered_text(element);

        // 2. Let trimmed text be the result of removing all whitespace from the start and end of the string rendered text.
        auto trimmed_text = MUST(rendered_text.trim_whitespace());

        // 3. If trimmed text equals selector, append element to result.
        if (trimmed_text == selector)
            result.append(element);
    }

    // 4. Return success with data result.
    return DOM::StaticNodeList::create(realm, move(result));
}

// https://w3c.github.io/webdriver/#partial-link-text
static ErrorOr<JS::NonnullGCPtr<DOM::NodeList>, Error> locate_element_by_partial_link_text(DOM::ParentNode& start_node, StringView selector)
{
    auto& realm = start_node.realm();

    // 1. Let elements be the result of calling querySelectorAll() with start node as this and "a" as the argument. If
    //    this throws an exception, return error with error code unknown error.
    auto elements = start_node.query_selector_all("a"sv);
    if (elements.is_exception())
        return Error::from_code(ErrorCode::UnknownError, "querySelectorAll() failed"sv);

    // 2. Let result be an empty NodeList.
    Vector<JS::Handle<DOM::Node>> result;

    // 3. For each element in elements:
    for (size_t i = 0; i < elements.value()->length(); ++i) {
        auto& element = const_cast<DOM::Node&>(*elements.value()->item(i));

        // 1. Let rendered text be the value that would be returned via a call to Get Element Text for element.
        auto rendered_text = element_rendered_text(element);

        // 2. If rendered text contains selector, append element to result.
        if (rendered_text.contains(selector))
            result.append(element);
    }

    // 4. Return success with data result.
    return DOM::StaticNodeList::create(realm, move(result));
}

// https://w3c.github.io/webdriver/#tag-name
static JS::NonnullGCPtr<DOM::NodeList> locate_element_by_tag_name(DOM::ParentNode& start_node, StringView selector)
{
    auto& realm = start_node.realm();

    // To find a web element with the Tag Name strategy return success with data set to the result of calling
    // getElementsByTagName() with start node as this and selector as the argument.
    auto elements = start_node.get_elements_by_tag_name(MUST(FlyString::from_utf8(selector)));

    // FIXME: Having to convert this to a NodeList is a bit awkward.
    Vector<JS::Handle<DOM::Node>> result;

    for (size_t i = 0; i < elements->length(); ++i) {
        auto* element = elements->item(i);
        result.append(*element);
    }

    return DOM::StaticNodeList::create(realm, move(result));
}

// https://w3c.github.io/webdriver/#xpath
static ErrorOr<JS::NonnullGCPtr<DOM::NodeList>, Error> locate_element_by_x_path(DOM::ParentNode&, StringView)
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

ErrorOr<JS::NonnullGCPtr<DOM::NodeList>, Error> invoke_location_strategy(LocationStrategy type, DOM::ParentNode& start_node, StringView selector)
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
