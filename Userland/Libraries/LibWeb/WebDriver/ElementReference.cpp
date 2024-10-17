/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectList.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
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

// https://w3c.github.io/webdriver/#dfn-deserialize-a-web-element
ErrorOr<JS::NonnullGCPtr<Web::DOM::Element>, WebDriver::Error> deserialize_web_element(JsonObject const& object)
{
    // 1. If object has no own property web element identifier, return error with error code invalid argument.
    if (!object.has_string(web_element_identifier))
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Object is not a web element");

    // 2. Let reference be the result of getting the web element identifier property from object.
    auto reference = extract_web_element_reference(object);

    // 3. Let element be the result of trying to get a known element with session and reference.
    auto element = TRY(get_known_element(reference));

    // 4. Return success with data element.
    return *element;
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

// https://w3c.github.io/webdriver/#dfn-get-a-webelement-origin
ErrorOr<JS::NonnullGCPtr<Web::DOM::Element>, Web::WebDriver::Error> get_web_element_origin(StringView origin)
{
    // 1. Assert: browsing context is the current browsing context.

    // 2. Let element be equal to the result of trying to get a known element with session and origin.
    auto element = TRY(get_known_element(origin));

    // 3. Return success with data element.
    return element;
}

// https://w3c.github.io/webdriver/#dfn-get-a-known-element
ErrorOr<JS::NonnullGCPtr<Web::DOM::Element>, Web::WebDriver::Error> get_known_element(StringView element_id)
{
    // NOTE: The whole concept of "connected elements" is not implemented yet. See get_or_create_a_web_element_reference().
    //       For now the element is only represented by its ID.

    // 1. If not node reference is known with session, session's current browsing context, and reference return error
    //    with error code no such element.
    auto element = element_id.to_number<int>();
    if (!element.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "Element ID is not an integer");

    // 2. Let node be the result of get a node with session, session's current browsing context, and reference.
    auto* node = Web::DOM::Node::from_unique_id(*element);

    // 3. If node is not null and node does not implement Element return error with error code no such element.
    if (node && !node->is_element())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, ByteString::formatted("Could not find element with ID: {}", element_id));

    // 4. If node is null or node is stale return error with error code stale element reference.
    if (!node || is_element_stale(*node))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::StaleElementReference, ByteString::formatted("Element with ID: {} is stale", element_id));

    // 5. Return success with data node.
    return static_cast<Web::DOM::Element&>(*node);
}

// https://w3c.github.io/webdriver/#dfn-is-stale
bool is_element_stale(Web::DOM::Node const& element)
{
    // An element is stale if its node document is not the active document or if it is not connected.
    return !element.document().is_active() || !element.is_connected();
}

// https://w3c.github.io/webdriver/#dfn-interactable
bool is_element_interactable(Web::HTML::BrowsingContext const& browsing_context, Web::DOM::Element const& element)
{
    // An interactable element is an element which is either pointer-interactable or keyboard-interactable.
    return is_element_keyboard_interactable(element) || is_element_pointer_interactable(browsing_context, element);
}

// https://w3c.github.io/webdriver/#dfn-pointer-interactable
bool is_element_pointer_interactable(Web::HTML::BrowsingContext const& browsing_context, Web::DOM::Element const& element)
{
    // A pointer-interactable element is defined to be the first element, defined by the paint order found at the center
    // point of its rectangle that is inside the viewport, excluding the size of any rendered scrollbars.
    auto const* document = browsing_context.active_document();
    if (!document)
        return false;

    auto const* paint_root = document->paintable_box();
    if (!paint_root)
        return false;

    auto viewport = browsing_context.page().top_level_traversable()->viewport_rect();
    auto center_point = in_view_center_point(element, viewport);

    auto result = paint_root->hit_test(center_point, Painting::HitTestType::TextCursor);
    if (!result.has_value())
        return false;

    return result->dom_node() == &element;
}

// https://w3c.github.io/webdriver/#dfn-keyboard-interactable
bool is_element_keyboard_interactable(Web::DOM::Element const& element)
{
    // A keyboard-interactable element is any element that has a focusable area, is a body element, or is the document element.
    return element.is_focusable() || is<HTML::HTMLBodyElement>(element) || element.is_document_element();
}

// https://w3c.github.io/webdriver/#dfn-editable
bool is_element_editable(Web::DOM::Element const& element)
{
    // Editable elements are those that can be used for typing and clearing, and they fall into two subcategories:
    // "Mutable form control elements" and "Mutable elements".
    return is_element_mutable_form_control(element) || is_element_mutable(element);
}

// https://w3c.github.io/webdriver/#dfn-mutable-element
bool is_element_mutable(Web::DOM::Element const& element)
{
    // Denotes elements that are editing hosts or content editable.
    if (!is<HTML::HTMLElement>(element))
        return false;

    auto const& html_element = static_cast<HTML::HTMLElement const&>(element);
    return html_element.is_editable();
}

// https://w3c.github.io/webdriver/#dfn-mutable-form-control-element
bool is_element_mutable_form_control(Web::DOM::Element const& element)
{
    // Denotes input elements that are mutable (e.g. that are not read only or disabled) and whose type attribute is
    // in one of the following states:
    if (is<HTML::HTMLInputElement>(element)) {
        auto const& input_element = static_cast<HTML::HTMLInputElement const&>(element);
        if (!input_element.is_mutable() || !input_element.enabled())
            return false;

        // Text and Search, URL, Telephone, Email, Password, Date, Month, Week, Time, Local Date and Time, Number,
        // Range, Color, File Upload
        switch (input_element.type_state()) {
        case HTML::HTMLInputElement::TypeAttributeState::Text:
        case HTML::HTMLInputElement::TypeAttributeState::Search:
        case HTML::HTMLInputElement::TypeAttributeState::URL:
        case HTML::HTMLInputElement::TypeAttributeState::Telephone:
        case HTML::HTMLInputElement::TypeAttributeState::Email:
        case HTML::HTMLInputElement::TypeAttributeState::Password:
        case HTML::HTMLInputElement::TypeAttributeState::Date:
        case HTML::HTMLInputElement::TypeAttributeState::Month:
        case HTML::HTMLInputElement::TypeAttributeState::Week:
        case HTML::HTMLInputElement::TypeAttributeState::Time:
        case HTML::HTMLInputElement::TypeAttributeState::LocalDateAndTime:
        case HTML::HTMLInputElement::TypeAttributeState::Number:
        case HTML::HTMLInputElement::TypeAttributeState::Range:
        case HTML::HTMLInputElement::TypeAttributeState::Color:
        case HTML::HTMLInputElement::TypeAttributeState::FileUpload:
            return true;
        default:
            return false;
        }
    }

    // And the textarea element.
    if (is<HTML::HTMLTextAreaElement>(element)) {
        auto const& text_area = static_cast<HTML::HTMLTextAreaElement const&>(element);
        return text_area.enabled();
    }

    return false;
}

// https://w3c.github.io/webdriver/#dfn-non-typeable-form-control
bool is_element_non_typeable_form_control(Web::DOM::Element const& element)
{
    // A non-typeable form control is an input element whose type attribute state causes the primary input mechanism not
    // to be through means of a keyboard, whether virtual or physical.
    if (!is<HTML::HTMLInputElement>(element))
        return false;

    auto const& input_element = static_cast<HTML::HTMLInputElement const&>(element);

    switch (input_element.type_state()) {
    case HTML::HTMLInputElement::TypeAttributeState::Hidden:
    case HTML::HTMLInputElement::TypeAttributeState::Range:
    case HTML::HTMLInputElement::TypeAttributeState::Color:
    case HTML::HTMLInputElement::TypeAttributeState::Checkbox:
    case HTML::HTMLInputElement::TypeAttributeState::RadioButton:
    case HTML::HTMLInputElement::TypeAttributeState::FileUpload:
    case HTML::HTMLInputElement::TypeAttributeState::SubmitButton:
    case HTML::HTMLInputElement::TypeAttributeState::ImageButton:
    case HTML::HTMLInputElement::TypeAttributeState::ResetButton:
    case HTML::HTMLInputElement::TypeAttributeState::Button:
        return true;
    default:
        return false;
    }
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
ErrorOr<JS::NonnullGCPtr<Web::DOM::ShadowRoot>, Web::WebDriver::Error> get_known_shadow_root(StringView shadow_id)
{
    // NOTE: The whole concept of "known shadow roots" is not implemented yet. See get_or_create_a_shadow_root_reference().
    //       For now the shadow root is only represented by its ID.
    auto shadow_root = shadow_id.to_number<int>();
    if (!shadow_root.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Shadow ID is not an integer");

    auto* node = Web::DOM::Node::from_unique_id(*shadow_root);

    if (!node || !node->is_shadow_root())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, ByteString::formatted("Could not find shadow root with ID: {}", shadow_id));

    return static_cast<Web::DOM::ShadowRoot&>(*node);
}

// https://w3c.github.io/webdriver/#dfn-center-point
CSSPixelPoint in_view_center_point(DOM::Element const& element, CSSPixelRect viewport)
{
    // 1. Let rectangle be the first element of the DOMRect sequence returned by calling getClientRects() on element.
    auto const* rectangle = element.get_client_rects()->item(0);
    VERIFY(rectangle);

    // 2. Let left be max(0, min(x coordinate, x coordinate + width dimension)).
    auto left = max(0.0, min(rectangle->x(), rectangle->x() + rectangle->width()));

    // 3. Let right be min(innerWidth, max(x coordinate, x coordinate + width dimension)).
    auto right = min(viewport.width().to_double(), max(rectangle->x(), rectangle->x() + rectangle->width()));

    // 4. Let top be max(0, min(y coordinate, y coordinate + height dimension)).
    auto top = max(0.0, min(rectangle->y(), rectangle->y() + rectangle->height()));

    // 5. Let bottom be min(innerHeight, max(y coordinate, y coordinate + height dimension)).
    auto bottom = min(viewport.height().to_double(), max(rectangle->y(), rectangle->y() + rectangle->height()));

    // 6. Let x be floor((left + right) ÷ 2.0).
    auto x = floor((left + right) / 2.0);

    // 7. Let y be floor((top + bottom) ÷ 2.0).
    auto y = floor((top + bottom) / 2.0);

    // 8. Return the pair of (x, y).
    return { x, y };
}

}
