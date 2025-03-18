/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectList.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/WebDriver/ElementReference.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-web-element-identifier
static String const web_element_identifier = "element-6066-11e4-a52e-4f735466cecf"_string;
static JS::PropertyKey web_element_identifier_key { web_element_identifier };

// https://w3c.github.io/webdriver/#dfn-shadow-root-identifier
static String const shadow_root_identifier = "shadow-6066-11e4-a52e-4f735466cecf"_string;
static JS::PropertyKey shadow_root_identifier_key { shadow_root_identifier };

// https://w3c.github.io/webdriver/#dfn-browsing-context-group-node-map
static HashMap<GC::RawPtr<HTML::BrowsingContextGroup const>, HashTable<String>> browsing_context_group_node_map;

// https://w3c.github.io/webdriver/#dfn-navigable-seen-nodes-map
static HashMap<GC::RawPtr<HTML::Navigable>, HashTable<String>> navigable_seen_nodes_map;

// https://w3c.github.io/webdriver/#dfn-get-a-node
GC::Ptr<Web::DOM::Node> get_node(HTML::BrowsingContext const& browsing_context, StringView reference)
{
    // 1. Let browsing context group node map be session's browsing context group node map.
    // 2. Let browsing context group be browsing context's browsing context group.
    auto const* browsing_context_group = browsing_context.group();

    // 3. If browsing context group node map does not contain browsing context group, return null.
    // 4. Let node id map be browsing context group node map[browsing context group].
    auto node_id_map = browsing_context_group_node_map.get(browsing_context_group);
    if (!node_id_map.has_value())
        return nullptr;

    // 5. Let node be the entry in node id map whose value is reference, if such an entry exists, or null otherwise.
    GC::Ptr<Web::DOM::Node> node;

    if (node_id_map->contains(reference)) {
        auto node_id = reference.to_number<i64>().value();
        node = Web::DOM::Node::from_unique_id(UniqueNodeID(node_id));
    }

    // 6. Return node.
    return node;
}

// https://w3c.github.io/webdriver/#dfn-get-or-create-a-node-reference
String get_or_create_a_node_reference(HTML::BrowsingContext const& browsing_context, Web::DOM::Node const& node)
{
    // 1. Let browsing context group node map be session's browsing context group node map.
    // 2. Let browsing context group be browsing context's browsing context group.
    auto const* browsing_context_group = browsing_context.group();

    // 3. If browsing context group node map does not contain browsing context group, set browsing context group node
    //    map[browsing context group] to a new weak map.
    // 4. Let node id map be browsing context group node map[browsing context group].
    auto& node_id_map = browsing_context_group_node_map.ensure(browsing_context_group);

    auto node_id = String::number(node.unique_id().value());

    // 5. If node id map does not contain node:
    if (!node_id_map.contains(node_id)) {
        // 1. Let node id be a new globally unique string.
        // 2. Set node id map[node] to node id.
        node_id_map.set(node_id);

        // 3. Let navigable be browsing context's active document's node navigable.
        auto navigable = browsing_context.active_document()->navigable();

        // 4. Let navigable seen nodes map be session's navigable seen nodes map.
        // 5. If navigable seen nodes map does not contain navigable, set navigable seen nodes map[navigable] to an empty set.
        // 6. Append node id to navigable seen nodes map[navigable].
        navigable_seen_nodes_map.ensure(navigable).set(node_id);
    }

    // 6. Return node id map[node].
    return node_id;
}

// https://w3c.github.io/webdriver/#dfn-node-reference-is-known
bool node_reference_is_known(HTML::BrowsingContext const& browsing_context, StringView reference)
{
    // 1. Let navigable be browsing context's active document's node navigable.
    auto navigable = browsing_context.active_document()->navigable();
    if (!navigable)
        return false;

    // 2. Let navigable seen nodes map be session's navigable seen nodes map.
    // 3. If navigable seen nodes map contains navigable and navigable seen nodes map[navigable] contains reference,
    //    return true, otherwise return false.
    if (auto map = navigable_seen_nodes_map.get(navigable); map.has_value())
        return map->contains(reference);
    return false;
}

// https://w3c.github.io/webdriver/#dfn-get-or-create-a-web-element-reference
String get_or_create_a_web_element_reference(HTML::BrowsingContext const& browsing_context, Web::DOM::Node const& element)
{
    // 1. Assert: element implements Element.
    VERIFY(element.is_element());

    // 2. Return the result of trying to get or create a node reference given session, session's current browsing
    //    context, and element.
    return get_or_create_a_node_reference(browsing_context, element);
}

// https://w3c.github.io/webdriver/#dfn-web-element-reference-object
JsonObject web_element_reference_object(HTML::BrowsingContext const& browsing_context, Web::DOM::Node const& element)
{
    // 1. Let identifier be the web element identifier.
    auto identifier = web_element_identifier;

    // 2. Let reference be the result of get or create a web element reference given element.
    auto reference = get_or_create_a_web_element_reference(browsing_context, element);

    // 3. Return a JSON Object initialized with a property with name identifier and value reference.
    JsonObject object;
    object.set(identifier, move(reference));
    return object;
}

// https://w3c.github.io/webdriver/#dfn-represents-a-web-element
bool represents_a_web_element(JsonValue const& value)
{
    // An ECMAScript Object represents a web element if it has a web element identifier own property.
    if (!value.is_object())
        return false;

    return value.as_object().has(web_element_identifier);
}

// https://w3c.github.io/webdriver/#dfn-represents-a-web-element
bool represents_a_web_element(JS::Value value)
{
    // An ECMAScript Object represents a web element if it has a web element identifier own property.
    if (!value.is_object())
        return false;

    auto result = value.as_object().has_own_property(web_element_identifier_key);
    return !result.is_error() && result.value();
}

// https://w3c.github.io/webdriver/#dfn-deserialize-a-web-element
ErrorOr<GC::Ref<Web::DOM::Element>, WebDriver::Error> deserialize_web_element(Web::HTML::BrowsingContext const& browsing_context, JsonObject const& object)
{
    // 1. If object has no own property web element identifier, return error with error code invalid argument.
    if (!object.has_string(web_element_identifier))
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Object is not a web element"sv);

    // 2. Let reference be the result of getting the web element identifier property from object.
    auto reference = extract_web_element_reference(object);

    // 3. Let element be the result of trying to get a known element with session and reference.
    auto element = TRY(get_known_element(browsing_context, reference));

    // 4. Return success with data element.
    return element;
}

// https://w3c.github.io/webdriver/#dfn-deserialize-a-web-element
ErrorOr<GC::Ref<Web::DOM::Element>, WebDriver::Error> deserialize_web_element(Web::HTML::BrowsingContext const& browsing_context, JS::Object const& object)
{
    // 1. If object has no own property web element identifier, return error with error code invalid argument.
    auto property = object.get(web_element_identifier_key);
    if (property.is_error() || !property.value().is_string())
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Object is not a web element"sv);

    // 2. Let reference be the result of getting the web element identifier property from object.
    auto reference = property.value().as_string().utf8_string();

    // 3. Let element be the result of trying to get a known element with session and reference.
    auto element = TRY(get_known_element(browsing_context, reference));

    // 4. Return success with data element.
    return element;
}

String extract_web_element_reference(JsonObject const& object)
{
    return object.get_string(web_element_identifier).release_value();
}

// https://w3c.github.io/webdriver/#dfn-get-a-webelement-origin
ErrorOr<GC::Ref<Web::DOM::Element>, Web::WebDriver::Error> get_web_element_origin(Web::HTML::BrowsingContext const& browsing_context, StringView origin)
{
    // 1. Assert: browsing context is the current browsing context.

    // 2. Let element be equal to the result of trying to get a known element with session and origin.
    auto element = TRY(get_known_element(browsing_context, origin));

    // 3. Return success with data element.
    return element;
}

// https://w3c.github.io/webdriver/#dfn-get-a-known-element
ErrorOr<GC::Ref<Web::DOM::Element>, Web::WebDriver::Error> get_known_element(Web::HTML::BrowsingContext const& browsing_context, StringView reference)
{
    // 1. If not node reference is known with session, session's current browsing context, and reference return error
    //    with error code no such element.
    if (!node_reference_is_known(browsing_context, reference))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, MUST(String::formatted("Element reference '{}' is not known", reference)));

    // 2. Let node be the result of get a node with session, session's current browsing context, and reference.
    auto node = get_node(browsing_context, reference);

    // 3. If node is not null and node does not implement Element return error with error code no such element.
    if (node && !node->is_element())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, MUST(String::formatted("Could not find element with reference '{}'", reference)));

    // 4. If node is null or node is stale return error with error code stale element reference.
    if (!node || is_element_stale(*node))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::StaleElementReference, MUST(String::formatted("Element reference '{}' is stale", reference)));

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
    return element.is_editable_or_editing_host();
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

// https://w3c.github.io/webdriver/#dfn-in-view
bool is_element_in_view(ReadonlySpan<GC::Ref<Web::DOM::Element>> paint_tree, Web::DOM::Element& element)
{
    // An element is in view if it is a member of its own pointer-interactable paint tree, given the pretense that its
    // pointer events are not disabled.
    if (!element.paintable() || !element.paintable()->is_visible() || !element.paintable()->visible_for_hit_testing())
        return false;

    return paint_tree.contains_slow(GC::Ref { element });
}

// https://w3c.github.io/webdriver/#dfn-in-view
bool is_element_obscured(ReadonlySpan<GC::Ref<Web::DOM::Element>> paint_tree, Web::DOM::Element& element)
{
    // An element is obscured if the pointer-interactable paint tree at its center point is empty, or the first element
    // in this tree is not an inclusive descendant of itself.
    return paint_tree.is_empty() || !paint_tree.first()->is_shadow_including_inclusive_descendant_of(element);
}

// https://w3c.github.io/webdriver/#dfn-pointer-interactable-paint-tree
GC::RootVector<GC::Ref<Web::DOM::Element>> pointer_interactable_tree(Web::HTML::BrowsingContext& browsing_context, Web::DOM::Element& element)
{
    // 1. If element is not in the same tree as session's current browsing context's active document, return an empty sequence.
    if (!browsing_context.active_document()->contains(element))
        return GC::RootVector<GC::Ref<Web::DOM::Element>>(browsing_context.heap());

    // 2. Let rectangles be the DOMRect sequence returned by calling getClientRects().
    auto rectangles = element.get_client_rects();

    // 3. If rectangles has the length of 0, return an empty sequence.
    if (rectangles.is_empty())
        return GC::RootVector<GC::Ref<Web::DOM::Element>>(browsing_context.heap());

    // 4. Let center point be the in-view center point of the first indexed element in rectangles.
    auto viewport = browsing_context.page().top_level_traversable()->viewport_rect();
    auto center_point = Web::WebDriver::in_view_center_point(element, viewport);

    // 5. Return the elements from point given the coordinates center point.
    return browsing_context.active_document()->elements_from_point(center_point.x().to_double(), center_point.y().to_double());
}

// https://w3c.github.io/webdriver/#dfn-get-or-create-a-shadow-root-reference
String get_or_create_a_shadow_root_reference(HTML::BrowsingContext const& browsing_context, Web::DOM::ShadowRoot const& shadow_root)
{
    // 1. Assert: element implements ShadowRoot.
    // 2. Return the result of trying to get or create a node reference with session, session's current browsing context,
    //    and element.
    return get_or_create_a_node_reference(browsing_context, shadow_root);
}

// https://w3c.github.io/webdriver/#dfn-shadow-root-reference-object
JsonObject shadow_root_reference_object(HTML::BrowsingContext const& browsing_context, Web::DOM::ShadowRoot const& shadow_root)
{
    // 1. Let identifier be the shadow root identifier.
    auto identifier = shadow_root_identifier;

    // 2. Let reference be the result of get or create a shadow root reference with session and shadow root.
    auto reference = get_or_create_a_shadow_root_reference(browsing_context, shadow_root);

    // 3. Return a JSON Object initialized with a property with name identifier and value reference.
    JsonObject object;
    object.set(identifier, move(reference));
    return object;
}

// https://w3c.github.io/webdriver/#dfn-represents-a-shadow-root
bool represents_a_shadow_root(JsonValue const& value)
{
    // An ECMAScript Object represents a shadow root if it has a shadow root identifier own property.
    if (!value.is_object())
        return false;

    return value.as_object().has(shadow_root_identifier);
}

// https://w3c.github.io/webdriver/#dfn-represents-a-shadow-root
bool represents_a_shadow_root(JS::Value value)
{
    // An ECMAScript Object represents a shadow root if it has a shadow root identifier own property.
    if (!value.is_object())
        return false;

    auto result = value.as_object().has_own_property(shadow_root_identifier_key);
    return !result.is_error() && result.value();
}

// https://w3c.github.io/webdriver/#dfn-deserialize-a-shadow-root
ErrorOr<GC::Ref<Web::DOM::ShadowRoot>, WebDriver::Error> deserialize_shadow_root(Web::HTML::BrowsingContext const& browsing_context, JsonObject const& object)
{
    // 1. If object has no own property shadow root identifier, return error with error code invalid argument.
    if (!object.has_string(shadow_root_identifier))
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Object is not a Shadow Root"sv);

    // 2. Let reference be the result of getting the shadow root identifier property from object.
    auto const& reference = object.get_string(shadow_root_identifier).release_value();

    // 3. Let shadow be the result of trying to get a known shadow root with session and reference.
    auto shadow = TRY(get_known_shadow_root(browsing_context, reference));

    // 4. Return success with data shadow.
    return shadow;
}

// https://w3c.github.io/webdriver/#dfn-deserialize-a-shadow-root
ErrorOr<GC::Ref<Web::DOM::ShadowRoot>, WebDriver::Error> deserialize_shadow_root(Web::HTML::BrowsingContext const& browsing_context, JS::Object const& object)
{
    // 1. If object has no own property shadow root identifier, return error with error code invalid argument.
    auto property = object.get(shadow_root_identifier_key);
    if (property.is_error() || !property.value().is_string())
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Object is not a Shadow Root"sv);

    // 2. Let reference be the result of getting the shadow root identifier property from object.
    auto reference = property.value().as_string().utf8_string();

    // 3. Let shadow be the result of trying to get a known shadow root with session and reference.
    auto shadow = TRY(get_known_shadow_root(browsing_context, reference));

    // 4. Return success with data shadow.
    return shadow;
}

// https://w3c.github.io/webdriver/#dfn-get-a-known-shadow-root
ErrorOr<GC::Ref<Web::DOM::ShadowRoot>, Web::WebDriver::Error> get_known_shadow_root(HTML::BrowsingContext const& browsing_context, StringView reference)
{
    // 1. If not node reference is known with session, session's current browsing context, and reference return error with error code no such shadow root.
    if (!node_reference_is_known(browsing_context, reference))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchShadowRoot, MUST(String::formatted("Shadow root reference '{}' is not known", reference)));

    // 2. Let node be the result of get a node with session, session's current browsing context, and reference.
    auto node = get_node(browsing_context, reference);

    // 3. If node is not null and node does not implement ShadowRoot return error with error code no such shadow root.
    if (node && !node->is_shadow_root())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchShadowRoot, MUST(String::formatted("Could not find shadow root with reference '{}'", reference)));

    // 4. If node is null or node is detached return error with error code detached shadow root.
    if (!node || is_shadow_root_detached(static_cast<Web::DOM::ShadowRoot&>(*node)))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::DetachedShadowRoot, MUST(String::formatted("Element reference '{}' is stale", reference)));

    // 5. Return success with data node.
    return static_cast<Web::DOM::ShadowRoot&>(*node);
}

// https://w3c.github.io/webdriver/#dfn-is-detached
bool is_shadow_root_detached(Web::DOM::ShadowRoot const& shadow_root)
{
    // A shadow root is detached if its node document is not the active document or if the element node referred to as
    // its host is stale.
    return !shadow_root.document().is_active() || !shadow_root.host() || is_element_stale(*shadow_root.host());
}

// https://w3c.github.io/webdriver/#dfn-bot-dom-getvisibletext
String element_rendered_text(DOM::Node& node)
{
    // FIXME: The spec does not define how to get the element's rendered text, other than to do exactly as Selenium does.
    //        This implementation is not sufficient, as we must also at least consider the shadow DOM.
    if (!is<HTML::HTMLElement>(node))
        return node.text_content().value_or(String {});

    auto& element = static_cast<HTML::HTMLElement&>(node);
    return element.inner_text();
}

// https://w3c.github.io/webdriver/#dfn-center-point
CSSPixelPoint in_view_center_point(DOM::Element const& element, CSSPixelRect viewport)
{
    // 1. Let rectangle be the first element of the DOMRect sequence returned by calling getClientRects() on element.
    auto rectangle = element.get_client_rects().first();

    // 2. Let left be max(0, min(x coordinate, x coordinate + width dimension)).
    auto left = max(CSSPixels(0), min(rectangle.x(), rectangle.x() + rectangle.width()));

    // 3. Let right be min(innerWidth, max(x coordinate, x coordinate + width dimension)).
    auto right = min(viewport.width(), max(rectangle.x(), rectangle.x() + rectangle.width()));

    // 4. Let top be max(0, min(y coordinate, y coordinate + height dimension)).
    auto top = max(CSSPixels(0), min(rectangle.y(), rectangle.y() + rectangle.height()));

    // 5. Let bottom be min(innerHeight, max(y coordinate, y coordinate + height dimension)).
    auto bottom = min(viewport.height(), max(rectangle.y(), rectangle.y() + rectangle.height()));

    // 6. Let x be floor((left + right) ÷ 2.0).
    auto x = floor((left + right) / 2.0);

    // 7. Let y be floor((top + bottom) ÷ 2.0).
    auto y = floor((top + bottom) / 2.0);

    // 8. Return the pair of (x, y).
    return { x, y };
}

}
