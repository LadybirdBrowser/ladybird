/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/WebDriver/ElementReference.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-web-element-identifier
static ByteString const web_element_identifier = "element-6066-11e4-a52e-4f735466cecf"sv;

// https://w3c.github.io/webdriver/#dfn-shadow-root-identifier
static ByteString const shadow_root_identifier = "shadow-6066-11e4-a52e-4f735466cecf"sv;

// https://w3c.github.io/webdriver/#dfn-get-or-create-a-web-element-reference
ByteString get_or_create_a_web_element_reference(Web::DOM::Node const& element)
{
    // FIXME: 1. For each known element of the current browsing context’s list of known elements:
    // FIXME:     1. If known element equals element, return success with known element’s web element reference.
    // FIXME: 2. Add element to the list of known elements of the current browsing context.
    // FIXME: 3. Return success with the element’s web element reference.

    return ByteString::number(element.unique_id());
}

// https://w3c.github.io/webdriver/#dfn-web-element-reference-object
JsonObject web_element_reference_object(Web::DOM::Node const& element)
{
    // 1. Let identifier be the web element identifier.
    auto identifier = web_element_identifier;

    // 2. Let reference be the result of get or create a web element reference given element.
    auto reference = get_or_create_a_web_element_reference(element);

    // 3. Return a JSON Object initialized with a property with name identifier and value reference.
    JsonObject object;
    object.set(identifier, reference);
    return object;
}

ByteString extract_web_element_reference(JsonObject const& object)
{
    return object.get_byte_string(web_element_identifier).release_value();
}

// https://w3c.github.io/webdriver/#dfn-represents-a-web-element
bool represents_a_web_element(JsonValue const& value)
{
    // An ECMAScript Object represents a web element if it has a web element identifier own property.
    if (!value.is_object())
        return false;
    return value.as_object().has_string(web_element_identifier);
}

// https://w3c.github.io/webdriver/#dfn-get-a-known-connected-element
ErrorOr<Web::DOM::Element*, Web::WebDriver::Error> get_known_connected_element(StringView element_id)
{
    // NOTE: The whole concept of "connected elements" is not implemented yet. See get_or_create_a_web_element_reference().
    //       For now the element is only represented by its ID.
    auto element = element_id.to_number<int>();
    if (!element.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Element ID is not an integer");

    auto* node = Web::DOM::Node::from_unique_id(*element);

    if (!node || !node->is_element())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, ByteString::formatted("Could not find element with ID: {}", element_id));

    return static_cast<Web::DOM::Element*>(node);
}

// https://w3c.github.io/webdriver/#dfn-is-stale
bool is_element_stale(Web::DOM::Node const& element)
{
    // An element is stale if its node document is not the active document or if it is not connected.
    return !element.document().is_active() || !element.is_connected();
}

// https://w3c.github.io/webdriver/#dfn-get-or-create-a-shadow-root-reference
ByteString get_or_create_a_shadow_root_reference(Web::DOM::ShadowRoot const& shadow_root)
{
    // FIXME: 1. For each known shadow root of the current browsing context’s list of known shadow roots:
    // FIXME:     1. If known shadow root equals shadow root, return success with known shadow root’s shadow root reference.
    // FIXME: 2. Add shadow to the list of known shadow roots of the current browsing context.
    // FIXME: 3. Return success with the shadow’s shadow root reference.

    return ByteString::number(shadow_root.unique_id());
}

// https://w3c.github.io/webdriver/#dfn-shadow-root-reference-object
JsonObject shadow_root_reference_object(Web::DOM::ShadowRoot const& shadow_root)
{
    // 1. Let identifier be the shadow root identifier.
    auto identifier = shadow_root_identifier;

    // 2. Let reference be the result of get or create a shadow root reference given shadow root.
    auto reference = get_or_create_a_shadow_root_reference(shadow_root);

    // 3. Return a JSON Object initialized with a property with name identifier and value reference.
    JsonObject object;
    object.set(identifier, reference);
    return object;
}

// https://w3c.github.io/webdriver/#dfn-get-a-known-shadow-root
ErrorOr<Web::DOM::ShadowRoot*, Web::WebDriver::Error> get_known_shadow_root(StringView shadow_id)
{
    // NOTE: The whole concept of "known shadow roots" is not implemented yet. See get_or_create_a_shadow_root_reference().
    //       For now the shadow root is only represented by its ID.
    auto shadow_root = shadow_id.to_number<int>();
    if (!shadow_root.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Shadow ID is not an integer");

    auto* node = Web::DOM::Node::from_unique_id(*shadow_root);

    if (!node || !node->is_shadow_root())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, ByteString::formatted("Could not find shadow root with ID: {}", shadow_id));

    return static_cast<Web::DOM::ShadowRoot*>(node);
}

}
