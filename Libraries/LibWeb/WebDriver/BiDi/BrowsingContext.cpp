/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/MemoryStream.h>
#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/JPEGWriter.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/HTML/AnimationFrameCallbackDriver.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebDriver/BiDi/BindingsGlue.h>
#include <LibWeb/WebDriver/BiDi/BrowsingContext.h>
#include <LibWeb/WebDriver/BiDi/RemoteValue.h>
#include <LibWeb/XPath/XPath.h>
#include <LibWeb/XPath/XPathResult.h>

namespace Web::WebDriver::BiDi {

static Error invalid_argument(StringView message)
{
    return Error::from_code(ErrorCode::InvalidArgument, message);
}

static Error invalid_selector(StringView message)
{
    return Error::from_code(ErrorCode::InvalidSelector, message);
}

using NodeList = GC::RootVector<GC::Ref<DOM::Node>>;

// https://w3c.github.io/webdriver-bidi/#locate-nodes-using-css
static ErrorOr<void, Error> locate_nodes_using_css(NodeList const& context_nodes, String const& selector, Optional<u64> maximum_returned_node_count, NodeList& returned_nodes)
{
    // 1. Let returned nodes be an empty list.
    // 2. Let parse result be the result of parse a selector given selector.
    // 3. If parse result is failure, return error with error code invalid selector.
    // 4. For each context node of context nodes:
    for (auto const& context_node : context_nodes) {
        // 1. Let elements be the result of match a selector against a tree with parse result and navigable’s active
        //    document root using scoping root context node.
        // NB: This is what querySelectorAll() does with context node as this.
        auto* parent_node = as_if<DOM::ParentNode>(*context_node);
        if (!parent_node)
            continue;

        auto elements = parent_node->query_selector_all(Utf16String::from_utf8(selector));
        if (elements.is_exception())
            return invalid_selector("The selector is not a valid CSS selector"sv);

        // 2. For each element in elements:
        for (size_t i = 0; i < elements.value()->length(); ++i) {
            // 1. Append element to returned nodes.
            returned_nodes.append(const_cast<DOM::Node&>(*elements.value()->item(i)));

            // 2. If maximum returned node count is not null and size of returned nodes is equal to maximum returned node
            //    count, return success with data returned nodes.
            if (maximum_returned_node_count.has_value() && returned_nodes.size() == *maximum_returned_node_count)
                return {};
        }
    }

    // 5. Return success with data returned nodes.
    return {};
}

// https://w3c.github.io/webdriver-bidi/#locate-nodes-using-xpath
static ErrorOr<void, Error> locate_nodes_using_xpath(NodeList const& context_nodes, String const& selector, Optional<u64> maximum_returned_node_count, NodeList& returned_nodes)
{
    // 1. Let returned nodes be an empty list.
    // 2. For each context node of context nodes:
    for (auto const& context_node : context_nodes) {
        // 1. Let evaluate result be the result of calling evaluate on navigable's active document, with arguments
        //    selector, context node, null, ORDERED_NODE_SNAPSHOT_TYPE, and null. If this throws a "SyntaxError"
        //    DOMException, return error with error code invalid selector; otherwise, if this throws any other exception
        //    return error with error code unknown error.
        auto evaluate_result = XPath::evaluate(Utf16String::from_utf8(selector), context_node, nullptr, XPath::XPathResult::ORDERED_NODE_SNAPSHOT_TYPE, nullptr);
        if (evaluate_result.has<XPath::EvaluationError>()) {
            if (evaluate_result.get<XPath::EvaluationError>() == XPath::EvaluationError::InvalidExpression)
                return invalid_selector("The selector is not a valid XPath expression"sv);
            return Error::from_code(ErrorCode::UnknownError, "Unable to evaluate the XPath expression"sv);
        }
        auto result = evaluate_result.get<GC::Ref<XPath::XPathResult>>();

        // 2. Let index be 0.
        // 3. Let length be the result of getting the snapshotLength property from evaluate result.
        auto length = result->snapshot_length();

        // 4. Repeat, while index is less than length:
        for (u32 index = 0; index < length; ++index) {
            // 1. Let node be the result of calling snapshotItem with evaluate result as this and index as the argument.
            auto node = result->snapshot_item(static_cast<int>(index));
            if (!node)
                continue;

            // 2. Append node to returned nodes.
            returned_nodes.append(*node);

            // 3. If maximum returned node count not null and size of returned nodes is equal to maximum returned node
            //    count, return success with data returned nodes.
            if (maximum_returned_node_count.has_value() && returned_nodes.size() == *maximum_returned_node_count)
                return {};

            // 4. Set index to index + 1.
        }
    }

    // 3. Return success with data returned nodes.
    return {};
}

enum class MatchType : u8 {
    Full,
    Partial,
};

// https://w3c.github.io/webdriver-bidi/#locate-nodes-using-inner-text
static ErrorOr<NodeList, Error> locate_nodes_using_inner_text(NodeList const& context_nodes, String const& selector, Optional<u64> max_depth, MatchType match_type, bool ignore_case, Optional<u64> maximum_returned_node_count)
{
    // 1. If selector is the empty string, return error with error code invalid selector.
    if (selector.is_empty())
        return invalid_selector("The inner text selector is empty"sv);

    // 2. Let returned nodes be an empty list.
    NodeList returned_nodes;

    // 3. If ignore case is false, let search text be selector. Otherwise, let search text be the result of toUppercase
    //    with selector according to the Unicode Default Case Conversion algorithm.
    auto search_text = ignore_case ? MUST(selector.to_uppercase()) : selector;

    // NB: The children of a node are its element children, as ParentNode's children attribute gives them.
    auto children_of = [](DOM::Node& node) {
        NodeList child_nodes;
        for (auto* child = node.first_child(); child; child = child->next_sibling()) {
            if (is<DOM::Element>(*child))
                child_nodes.append(*child);
        }
        return child_nodes;
    };

    // 4. For each context node in context nodes:
    for (auto const& context_node : context_nodes) {
        // 1. If context node implements Document or DocumentFragment:
        if (is<DOM::Document>(*context_node) || is<DOM::DocumentFragment>(*context_node)) {
            // 1. Let child nodes be an empty list.
            // 2. For each node child in the children of context node.
            //     1. Append child to child nodes.
            auto child_nodes = children_of(context_node);

            // 3. Extend returned nodes with the result of trying to locate nodes using inner text with child nodes,
            //    selector, max depth, match type, ignore case, and maximum returned node count.
            returned_nodes.extend(TRY(locate_nodes_using_inner_text(child_nodes, selector, max_depth, match_type, ignore_case, maximum_returned_node_count)));
        }

        // 2. If context node does not implement HTMLElement then continue.
        auto* html_element = as_if<HTML::HTMLElement>(*context_node);
        if (!html_element)
            continue;

        // 3. Let node inner text be the result of calling the innerText getter steps with context node as the this value.
        auto node_inner_text = html_element->inner_text().to_utf8();

        // 4. If ignore case is false, let node text be node inner text. Otherwise, let node text be the result of
        //    toUppercase with node inner text according to the Unicode Default Case Conversion algorithm.
        auto node_text = ignore_case ? MUST(node_inner_text.to_uppercase()) : node_inner_text;

        // 5. If search text is a code point substring of node text, perform the following steps:
        if (!node_text.contains(search_text))
            continue;

        // 1. Let child nodes be an empty list and, for each node child in the children of context node:
        //     1. Append child to child nodes.
        auto child_nodes = children_of(context_node);

        // 2. If size of child nodes is equal to 0 or max depth is equal to 0, perform the following steps:
        if (child_nodes.is_empty() || max_depth == 0u) {
            // 1. If match type is "full" and node text is search text, append context node to returned nodes.
            if (match_type == MatchType::Full && node_text == search_text)
                returned_nodes.append(context_node);
            // 2. Otherwise, if match type is "partial", append context node to returned nodes.
            else if (match_type == MatchType::Partial)
                returned_nodes.append(context_node);
        }
        // 3. Otherwise, perform the following steps:
        else {
            // 1. Let child max depth be null if max depth is null, or max depth - 1 otherwise.
            Optional<u64> child_max_depth;
            if (max_depth.has_value())
                child_max_depth = *max_depth - 1;

            // 2. Let child node matches be the result of locate nodes using inner text with child nodes, selector, child
            //    max depth, match type, ignore case, and maximum returned node count.
            auto child_node_matches = TRY(locate_nodes_using_inner_text(child_nodes, selector, child_max_depth, match_type, ignore_case, maximum_returned_node_count));

            // 3. If size of child node matches is equal to 0 and match type is "partial", append context node to returned
            //    nodes. Otherwise, extend returned nodes with child node matches.
            if (child_node_matches.is_empty() && match_type == MatchType::Partial)
                returned_nodes.append(context_node);
            else
                returned_nodes.extend(move(child_node_matches));
        }
    }

    // 5. If maximum returned node count is not null, remove all entries in returned nodes with an index greater than or
    //    equal to maximum returned node count.
    if (maximum_returned_node_count.has_value() && returned_nodes.size() > *maximum_returned_node_count)
        returned_nodes.shrink(*maximum_returned_node_count);

    // 6. Return success with data returned nodes.
    return returned_nodes;
}

// https://w3c.github.io/webdriver-bidi/#collect-nodes-using-accessibility-attributes
static void collect_nodes_using_accessibility_attributes(NodeList const& context_nodes, Optional<String> const& role_selector, Optional<String> const& name_selector, Optional<u64> maximum_returned_node_count, NodeList& returned_nodes)
{
    // 1. If returned nodes is null:
    //     1. Set returned nodes to an empty list.

    // 2. For each context node in context nodes:
    for (auto const& context_node : context_nodes) {
        // 1. Let match be true.
        bool match = true;

        // 2. If context node implements Element:
        if (auto* element = as_if<DOM::Element>(*context_node)) {
            // 1. If selector contains "role":
            if (role_selector.has_value()) {
                // 1. Let role be the computed role of context node.
                auto role = element->role_or_default();

                // 2. If selector["role"] is not role:
                //     1. Set match to false.
                if (!role.has_value() || ARIA::role_name(*role).to_utf8() != *role_selector)
                    match = false;
            }

            // 2. If selector contains "name":
            if (name_selector.has_value()) {
                // 1. Let name be the accessible name of context node.
                auto name = element->accessible_name(element->document());

                // 2. If selector["name"] is not name:
                //     1. Set match to false.
                if (name.is_error() || name.value().to_utf8() != *name_selector)
                    match = false;
            }
        }
        // 3. Otherwise, set match to false.
        else {
            match = false;
        }

        // 4. If match is true:
        if (match) {
            // 1. If maximum returned node count is not null and size of returned nodes is equal to maximum returned node
            //    count, break.
            if (maximum_returned_node_count.has_value() && returned_nodes.size() == *maximum_returned_node_count)
                break;

            // 2. Append context node to returned nodes.
            returned_nodes.append(context_node);
        }

        // 5. Let child nodes be an empty list and, for each node child in the children of context node:
        //     1. If child implements Element, append child to child nodes.
        NodeList child_nodes;
        for (auto* child = context_node->first_child(); child; child = child->next_sibling()) {
            if (is<DOM::Element>(*child))
                child_nodes.append(*child);
        }

        // 6. Try to collect nodes using accessibility attributes with child nodes, selector, maximum returned node
        //    count, and returned nodes.
        collect_nodes_using_accessibility_attributes(child_nodes, role_selector, name_selector, maximum_returned_node_count, returned_nodes);
    }

    // 3. Return returned nodes.
}

static ErrorOr<String, Error> get_locator_string_value(JsonObject const& locator)
{
    auto value = locator.get("value"sv);
    if (!value.has_value() || !value->is_string())
        return invalid_argument("The value of the locator must be a string"sv);
    return value->as_string();
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-locateNodes
Response locate_nodes(HTML::Navigable& navigable, JsonObject const& params)
{
    // 1. Let navigable id be command parameters["context"].
    // 2. Let navigable be the result of trying to get a navigable with navigable id.
    // 3. Assert: navigable is not null.
    auto* local_navigable = as_if<HTML::LocalNavigable>(navigable);
    if (!local_navigable || !local_navigable->active_document())
        return Error::from_code(ErrorCode::NoSuchFrame, "Browsing context not found"sv);
    auto& document = *local_navigable->active_document();

    // 4. Let realm be the result of trying to get a realm from a navigable with navigable id of navigable and null.
    auto& settings = document.relevant_settings_object();
    HTML::TemporaryExecutionContext execution_context { settings };

    // 5. Let locator be command parameters["locator"].
    auto locator_value = params.get("locator"sv);
    if (!locator_value.has_value() || !locator_value->is_object())
        return invalid_argument("Parameter 'locator' must be an object"sv);
    auto const& locator = locator_value->as_object();

    // 6. If command parameters contains "startNodes", let start nodes parameter be command parameters["startNodes"].
    //    Otherwise let start nodes parameter be null.
    Optional<JsonArray const&> start_nodes_parameter;
    if (auto start_nodes = params.get("startNodes"sv); start_nodes.has_value()) {
        if (!start_nodes->is_array() || start_nodes->as_array().is_empty())
            return invalid_argument("Parameter 'startNodes' must be a non-empty list of shared references"sv);
        start_nodes_parameter = start_nodes->as_array();
    }

    // 7. If command parameters contains "maxNodeCount", let maximum returned node count be
    //    command parameters["maxNodeCount"]. Otherwise, let maximum returned node count be null.
    Optional<u64> maximum_returned_node_count;
    if (auto max_node_count = params.get("maxNodeCount"sv); max_node_count.has_value()) {
        maximum_returned_node_count = as_js_uint(*max_node_count);
        if (!maximum_returned_node_count.has_value() || *maximum_returned_node_count < 1)
            return invalid_argument("Parameter 'maxNodeCount' must be an integer greater than 0"sv);
    }

    // NB: The serialization options are checked before any node is located.
    // 14. If command parameters contains "serializationOptions", let serialization options be
    //     command parameters["serializationOptions"]. Otherwise, let serialization options be a map matching the
    //     script.SerializationOptions production with the fields set to their default values.
    SerializationOptions serialization_options;
    if (auto options = params.get("serializationOptions"sv); options.has_value())
        serialization_options = TRY(parse_serialization_options(*options));

    // 8. Let context nodes be an empty list.
    NodeList context_nodes;

    // 9. If start nodes parameter is null, append the navigable's active document to context nodes. Otherwise, for each
    //    serialized start node in start nodes parameter:
    if (!start_nodes_parameter.has_value()) {
        context_nodes.append(document);
    } else {
        for (auto const& serialized_start_node : start_nodes_parameter->values()) {
            // script.SharedReference = { sharedId: script.SharedId, ? handle: script.Handle, Extensible }
            if (!serialized_start_node.is_object() || !serialized_start_node.as_object().get_string("sharedId"sv).has_value())
                return invalid_argument("Parameter 'startNodes' must be a non-empty list of shared references"sv);
            if (auto handle = serialized_start_node.as_object().get("handle"sv); handle.has_value() && !handle->is_string())
                return invalid_argument("Parameter 'startNodes' must be a non-empty list of shared references"sv);

            // 1. Let start node be the result of trying to deserialize shared reference given serialized start node,
            //    realm and session.
            auto start_node = TRY(deserialize_shared_reference(serialized_start_node.as_object(), settings));

            // 2. Append start node to context nodes.
            auto* node = start_node.is_object() ? Bindings::impl_from<DOM::Node>(&start_node.as_object()) : nullptr;
            if (!node)
                return Error::from_code(ErrorCode::NoSuchNode, "Node not found"sv);

            // INTEROP: A start node is a node that can have element children: an element, a document, or a document
            //          fragment.
            if (!is<DOM::ParentNode>(*node))
                return invalid_argument("A start node must be an element, a document or a document fragment"sv);
            context_nodes.append(*node);
        }
    }

    // 10. Assert size of context nodes is greater than 0.
    VERIFY(!context_nodes.is_empty());

    // 11. Let type be locator["type"].
    auto type = locator.get_string("type"sv);
    if (!type.has_value())
        return invalid_argument("The type of the locator must be a string"sv);

    NodeList result_nodes;

    // 12. In the following list of conditions and associated steps, run the first set of steps for which the associated
    //     condition is true:
    // -> type is the string "css"
    if (*type == "css"sv) {
        // 1. Let selector be locator["value"].
        auto selector = TRY(get_locator_string_value(locator));

        // 2. Let result nodes be a result of trying to locate nodes using css given navigable, context nodes, selector
        //    and maximum returned nodes.
        TRY(locate_nodes_using_css(context_nodes, selector, maximum_returned_node_count, result_nodes));
    }
    // -> type is the string "xpath"
    else if (*type == "xpath"sv) {
        // 1. Let selector be locator["value"].
        auto selector = TRY(get_locator_string_value(locator));

        // 2. Let result nodes be a result of trying to locate nodes using xpath given navigable, context nodes, selector
        //    and maximum returned nodes.
        TRY(locate_nodes_using_xpath(context_nodes, selector, maximum_returned_node_count, result_nodes));
    }
    // -> type is the string "innerText"
    else if (*type == "innerText"sv) {
        // 1. Let selector be locator["value"].
        auto selector = TRY(get_locator_string_value(locator));

        // 2. If locator contains maxDepth, let max depth be locator["maxDepth"]. Otherwise, let max depth be null.
        Optional<u64> max_depth;
        if (auto value = locator.get("maxDepth"sv); value.has_value()) {
            max_depth = as_js_uint(*value);
            if (!max_depth.has_value())
                return invalid_argument("The maxDepth of the locator must be an unsigned integer"sv);
        }

        // 3. If locator contains ignoreCase, let ignore case be locator["ignoreCase"]. Otherwise, let ignore case be
        //    false.
        bool ignore_case = false;
        if (auto value = locator.get("ignoreCase"sv); value.has_value()) {
            if (!value->is_bool())
                return invalid_argument("The ignoreCase of the locator must be a boolean"sv);
            ignore_case = value->as_bool();
        }

        // 4. If locator contains matchType, let match type be locator["matchType"]. Otherwise, let match type be "full".
        auto match_type = MatchType::Full;
        if (auto value = locator.get("matchType"sv); value.has_value()) {
            if (value->is_string() && value->as_string() == "full"sv)
                match_type = MatchType::Full;
            else if (value->is_string() && value->as_string() == "partial"sv)
                match_type = MatchType::Partial;
            else
                return invalid_argument("The matchType of the locator must be \"full\" or \"partial\""sv);
        }

        // 5. Let result nodes be a result of trying to locate nodes using inner text given context nodes, selector, max
        //    depth, match type, ignore case and maximum returned node count.
        result_nodes = TRY(locate_nodes_using_inner_text(context_nodes, selector, max_depth, match_type, ignore_case, maximum_returned_node_count));
    }
    // -> type is the string "accessibility"
    else if (*type == "accessibility"sv) {
        // 1. Let selector be locator["value"].
        auto selector = locator.get_object("value"sv);
        if (!selector.has_value())
            return invalid_argument("The value of the locator must be an object"sv);

        Optional<String> role_selector;
        if (auto role = selector->get("role"sv); role.has_value()) {
            if (!role->is_string())
                return invalid_argument("The role of the locator must be a string"sv);
            role_selector = role->as_string();
        }
        Optional<String> name_selector;
        if (auto name = selector->get("name"sv); name.has_value()) {
            if (!name->is_string())
                return invalid_argument("The name of the locator must be a string"sv);
            name_selector = name->as_string();
        }

        // 2. Let result nodes be locate nodes using accessibility attributes given context nodes, selector, and maximum
        //    returned node count.
        // https://w3c.github.io/webdriver-bidi/#locate-nodes-using-accessibility-attributes
        // 1. If selector does not contain "role" and selector does not contain "name", return error with error code
        //    invalid selector.
        if (!role_selector.has_value() && !name_selector.has_value())
            return invalid_selector("The accessibility locator has neither a role nor a name"sv);

        // 2. Return the result of collect nodes using accessibility attributes with context nodes, selector, maximum
        //    returned node count, and null.
        collect_nodes_using_accessibility_attributes(context_nodes, role_selector, name_selector, maximum_returned_node_count, result_nodes);
    }
    // -> type is the string "context"
    else if (*type == "context"sv) {
        // 1. If start nodes parameter is not null, return error with error code "invalid argument".
        if (start_nodes_parameter.has_value())
            return invalid_argument("A context locator does not take start nodes"sv);

        // 2. Let selector be locator["value"].
        auto selector = locator.get_object("value"sv);
        if (!selector.has_value())
            return invalid_argument("The value of the locator must be an object"sv);

        // 3. Let context id be selector["context"].
        auto context_id = selector->get_string("context"sv);
        if (!context_id.has_value())
            return invalid_argument("The context of the locator must be a string"sv);

        // 4. Let child navigable be the result of trying to get a navigable with context id.
        // 5. If child navigable's parent is not navigable, return error with error code "invalid argument".
        // 6. Let result nodes be locate the container element given child navigable.
        // NB: The children of the navigable are the navigables of the navigable containers in its active document.
        //     The containers of the child navigables can be in shadow trees.
        GC::Ptr<DOM::Element> container;
        document.for_each_shadow_including_descendant([&](DOM::Node& node) {
            auto* navigable_container = as_if<HTML::NavigableContainer>(node);
            if (!navigable_container)
                return TraversalDecision::Continue;
            if (auto content_navigable = navigable_container->content_navigable(); content_navigable && navigable_id(*content_navigable) == *context_id) {
                container = navigable_container;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        if (!container)
            return invalid_argument("The context of the locator is not a child of the browsing context"sv);

        // https://w3c.github.io/webdriver-bidi/#locate-the-container-element
        // 1. Let returned nodes be an empty list.
        // 2. If navigable's container is not null, append navigable's container to returned nodes.
        // 3. Return returned nodes.
        result_nodes.append(*container);

        // 7. Assert: For each node in result nodes, node's node navigable is navigable.
    } else {
        return invalid_argument("Unknown locator type"sv);
    }

    // 13. Assert: maximum returned node count is null or size of result nodes is less than or equal to maximum returned
    //     node count.
    VERIFY(!maximum_returned_node_count.has_value() || result_nodes.size() <= *maximum_returned_node_count);

    // 15. Let result ownership be "none".
    auto result_ownership = ResultOwnership::None;

    // 16. Let serialized nodes be an empty list.
    JsonArray serialized_nodes;

    // 17. For each result node in result nodes:
    for (auto const& result_node : result_nodes) {
        // 1. Let serialized node be the result of serialize as a remote value with result node, serialization options,
        //    result ownership, a new map as serialization internal map, realm and session.
        // 2. Append serialized node to serialized nodes.
        auto value = BindingsGlue::wrap_node(settings, result_node);
        serialized_nodes.must_append(serialize_as_a_remote_value(value, serialization_options, result_ownership, settings));
    }

    // 18. Let result be a map matching the browsingContext.LocateNodesResult production, with the nodes field set
    //     serialized nodes.
    JsonObject result;
    result.set("nodes"sv, move(serialized_nodes));

    // 19. Return success with data result.
    return JsonValue { move(result) };
}

struct Rect {
    double x { 0 };
    double y { 0 };
    double width { 0 };
    double height { 0 };
};

// https://w3c.github.io/webdriver-bidi/#normalize-rect
static Rect normalize_rect(Rect rect)
{
    // 1. Let x be rect's x coordinate.
    // 2. Let y be rect's y coordinate.
    // 3. Let width be rect's width dimension.
    // 4. Let height be rect's height dimension.
    // 5. If width is less than 0, set x to x + width and then set width to -width.
    if (rect.width < 0) {
        rect.x += rect.width;
        rect.width = -rect.width;
    }

    // 6. If height is less than 0, set y to y + height and then set height to -height.
    if (rect.height < 0) {
        rect.y += rect.height;
        rect.height = -rect.height;
    }

    // 7. Return a new DOMRectReadOnly with x coordinate x, y coordinate y, width dimension width and height dimension
    //    height.
    return rect;
}

// https://w3c.github.io/webdriver-bidi/#rectangle-intersection
static Rect rectangle_intersection(Rect rect1, Rect rect2)
{
    // 1. Let rect1 be normalize rect with rect1.
    rect1 = normalize_rect(rect1);

    // 2. Let rect2 be normalize rect with rect2.
    rect2 = normalize_rect(rect2);

    // 3. Let x1_0 be rect1's x coordinate.
    // 4. Let x2_0 be rect2's x coordinate.
    // 5. Let x1_1 be rect1's x coordinate plus rect1's width dimension.
    // 6. Let x2_1 be rect2's x coordinate plus rect2's width dimension.
    // 7. Let x_0 be the maximum element of «x1_0, x2_0».
    auto x_0 = max(rect1.x, rect2.x);

    // 8. Let x_1 be the minimum element of «x1_1, x2_1».
    auto x_1 = min(rect1.x + rect1.width, rect2.x + rect2.width);

    // 9. Let y1_0 be rect1's y coordinate.
    // 10. Let y2_0 be rect2's y coordinate.
    // 11. Let y1_1 be rect1's y coordinate plus rect1's height dimension.
    // 12. Let y2_1 be rect2's y coordinate plus rect2's height dimension.
    // 13. Let y_0 be the maximum element of «y1_0, y2_0».
    auto y_0 = max(rect1.y, rect2.y);

    // 14. Let y_1 be the minimum element of «y1_1, y2_1».
    auto y_1 = min(rect1.y + rect1.height, rect2.y + rect2.height);

    // 15. If x_1 is less than x_0, let width be 0. Otherwise let width be x_1 - x_0.
    auto width = x_1 < x_0 ? 0 : x_1 - x_0;

    // 16. If y_1 is less than y_0, let height be 0. Otherwise let height be y_1 - y_0.
    auto height = y_1 < y_0 ? 0 : y_1 - y_0;

    // 17. Return a new DOMRectReadOnly with x coordinate x_0, y coordinate y_0, width dimension width and height
    //     dimension height.
    return { x_0, y_0, width, height };
}

// https://w3c.github.io/webdriver-bidi/#round-a-number
static double round_a_number(double number)
{
    // 1. If number is not finite or number is an integral Number, return number.
    if (!isfinite(number) || trunc(number) == number)
        return number;

    // 2. If number < 0.5𝔽 and number > +0𝔽, return +0𝔽.
    if (number < 0.5 && number > 0)
        return 0;

    // 3. If number < -0𝔽 and n ≥ -0.5𝔽, return -0𝔽.
    if (number < 0 && number >= -0.5)
        return -0.0;

    // 4. Return the integral Number closest to number, preferring the Number closer to +∞ in the case of a tie.
    return floor(number + 0.5);
}

// https://w3c.github.io/webdriver-bidi/#get-the-origin-rectangle
static Rect get_the_origin_rectangle(DOM::Document& document, StringView origin)
{
    // 1. If origin is "viewport":
    if (origin == "viewport"sv) {
        // 1. Let viewport be document's visual viewport.
        auto viewport = document.visual_viewport();

        // 2. Let viewport rect be a DOMRectReadOnly with x coordinate viewport page left, y coordinate viewport page top,
        //    width dimension viewport width, and height dimension viewport height.
        // 3. Return success with data viewport rect.
        return { viewport->page_left(), viewport->page_top(), viewport->width(), viewport->height() };
    }

    // 2. Assert: origin is "document".
    VERIFY(origin == "document"sv);

    // 3. Let document element be the document element for document.
    auto document_element = document.document_element();
    if (!document_element)
        return {};

    // 4. Let document rect be a DOMRectReadOnly with x coordinate 0, y coordinate 0, width dimension document element
    //    scroll height, and height dimension document element scroll width.
    // NB: This swaps the scroll width and height of the document element, which the rectangle is the size of.
    // 5. Return success with data document rect.
    return { 0, 0, static_cast<double>(document_element->scroll_width()), static_cast<double>(document_element->scroll_height()) };
}

// NB: Scales a bitmap to the given size, picking the nearest pixel of the bitmap.
static ErrorOr<NonnullRefPtr<Gfx::Bitmap>> scale_bitmap(Gfx::Bitmap const& bitmap, Gfx::IntSize size)
{
    if (bitmap.size() == size)
        return NonnullRefPtr { const_cast<Gfx::Bitmap&>(bitmap) };

    auto scaled = TRY(Gfx::Bitmap::create(bitmap.format(), bitmap.alpha_type(), size));
    for (int y = 0; y < size.height(); ++y) {
        auto source_y = min(bitmap.height() - 1, static_cast<int>((static_cast<double>(y) + 0.5) * bitmap.height() / size.height()));
        for (int x = 0; x < size.width(); ++x) {
            auto source_x = min(bitmap.width() - 1, static_cast<int>((static_cast<double>(x) + 0.5) * bitmap.width() / size.width()));
            scaled->set_pixel(x, y, bitmap.get_pixel(source_x, source_y));
        }
    }
    return scaled;
}

// https://w3c.github.io/webdriver-bidi/#encode-a-canvas-as-base64
static Response encode_a_canvas_as_base64(Gfx::Bitmap const& bitmap, Optional<String> const& type, Optional<double> quality)
{
    // 1. If format is not null, let type be the type field of format, and let quality be the quality field of format.
    // 2. Otherwise, let type be "image/png" and let quality be undefined.
    // 3. Let file be a serialization of the bitmap as a file for canvas with type and quality.
    // https://html.spec.whatwg.org/multipage/canvas.html#a-serialisation-of-the-bitmap-as-a-file
    // NB: A type that is not supported serializes the bitmap as image/png.
    ByteBuffer file;
    if (type.has_value() && *type == "image/jpeg"sv) {
        AllocatingMemoryStream stream;
        Gfx::JPEGEncoderOptions options;
        if (quality.has_value())
            options.quality = static_cast<u8>(clamp(*quality, 0.0, 1.0) * 100);
        if (Gfx::JPEGWriter::encode(stream, bitmap, options).is_error())
            return Error::from_code(ErrorCode::UnableToCaptureScreen, "Failed to serialize the screenshot"sv);
        auto data = stream.read_until_eof();
        if (data.is_error())
            return Error::from_code(ErrorCode::UnableToCaptureScreen, "Failed to serialize the screenshot"sv);
        file = data.release_value();
    } else {
        auto encoded = Gfx::PNGWriter::encode(bitmap);
        if (encoded.is_error())
            return Error::from_code(ErrorCode::UnableToCaptureScreen, "Failed to serialize the screenshot"sv);
        file = encoded.release_value();
    }

    // 4. Let encoded string be the forgiving-base64 encode of file.
    auto encoded_string = MUST(encode_base64(file));

    // 5. Return success with data encoded string.
    return JsonValue { move(encoded_string) };
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-captureScreenshot
void capture_screenshot(HTML::Navigable& navigable, JsonObject const& params, OnCommandComplete on_complete)
{
    // 1. Let navigable id be the value of the context field of command parameters if present, or null otherwise.
    // 2. Let navigable be the result of trying to get a navigable with navigable id.
    auto* local_navigable = as_if<HTML::LocalNavigable>(navigable);
    if (!local_navigable || !local_navigable->active_document() || !local_navigable->active_window())
        return on_complete(Error::from_code(ErrorCode::NoSuchFrame, "Browsing context not found"sv));

    // NB: The parameters are checked before the screenshot is captured.
    // 6. Let origin be the value of the context field of command parameters if present, or "viewport" otherwise.
    // NB: This means the origin field.
    auto origin = "viewport"_string;
    if (auto value = params.get("origin"sv); value.has_value()) {
        if (!value->is_string() || !value->as_string().is_one_of("viewport"sv, "document"sv))
            return on_complete(invalid_argument("Parameter 'origin' must be \"viewport\" or \"document\""sv));
        origin = value->as_string();
    }

    Optional<String> format_type;
    Optional<double> format_quality;
    if (auto value = params.get("format"sv); value.has_value()) {
        // browsingContext.ImageFormat = { type: text, ? quality: 0.0..1.0 }
        if (!value->is_object() || !value->as_object().get_string("type"sv).has_value())
            return on_complete(invalid_argument("Parameter 'format' must be an image format"sv));
        format_type = *value->as_object().get_string("type"sv);
        if (auto quality = value->as_object().get("quality"sv); quality.has_value()) {
            auto number = quality->get_number_with_precision_loss<double>();
            if (!number.has_value() || *number < 0 || *number > 1)
                return on_complete(invalid_argument("The quality of the image format must be between 0 and 1"sv));
            format_quality = number;
        }
    }

    Optional<JsonObject> clip;
    if (auto value = params.get("clip"sv); value.has_value()) {
        if (!value->is_object())
            return on_complete(invalid_argument("Parameter 'clip' must be a clip rectangle"sv));
        auto const& clip_object = value->as_object();
        auto type = clip_object.get_string("type"sv);
        if (type == "box"sv) {
            // browsingContext.BoxClipRectangle = { type: "box", x: float, y: float, width: float, height: float }
            for (auto field : { "x"sv, "y"sv, "width"sv, "height"sv }) {
                auto field_value = clip_object.get(field);
                if (!field_value.has_value() || !field_value->is_number())
                    return on_complete(invalid_argument("A box clip rectangle must have numbers for x, y, width and height"sv));
            }
        } else if (type == "element"sv) {
            // browsingContext.ElementClipRectangle = { type: "element", element: script.SharedReference }
            auto element = clip_object.get("element"sv);
            if (!element.has_value() || !element->is_object() || !element->as_object().get_string("sharedId"sv).has_value())
                return on_complete(invalid_argument("An element clip rectangle must have a shared reference"sv));
            if (auto handle = element->as_object().get("handle"sv); handle.has_value() && !handle->is_string())
                return on_complete(invalid_argument("The handle of a shared reference must be a string"sv));
        } else {
            return on_complete(invalid_argument("Parameter 'clip' must be a clip rectangle"sv));
        }
        clip = clip_object;
    }

    Optional<u64> max_width;
    Optional<u64> max_height;
    if (auto value = params.get("imageSize"sv); value.has_value()) {
        // browsingContext.ImageSize = { ? maxWidth: (js-uint .ge 1), ? maxHeight: (js-uint .ge 1) }
        if (!value->is_object())
            return on_complete(invalid_argument("Parameter 'imageSize' must be an image size"sv));
        if (auto width = value->as_object().get("maxWidth"sv); width.has_value()) {
            max_width = as_js_uint(*width);
            if (!max_width.has_value() || *max_width < 1)
                return on_complete(invalid_argument("The maxWidth of the image size must be an integer greater than 0"sv));
        }
        if (auto height = value->as_object().get("maxHeight"sv); height.has_value()) {
            max_height = as_js_uint(*height);
            if (!max_height.has_value() || *max_height < 1)
                return on_complete(invalid_argument("The maxHeight of the image size must be an integer greater than 0"sv));
        }
    }

    // 3. If the implementation is unable to capture a screenshot of navigable for any reason then return error with
    //    error code unsupported operation.

    // 4. Let document be navigable's active document.
    GC::Ref document = *local_navigable->active_document();
    auto window = local_navigable->active_window();
    if (document->hidden() || !document->is_fully_active())
        return on_complete(Error::from_code(ErrorCode::UnsupportedOperation, "Unable to capture a screenshot of an inactive or hidden navigable"sv));

    // NB: Visibility and activity changes complete a pending capture without waiting for its callback to be collected.
    struct State : RefCounted<State>
        , Weakable<State> {
        AK_ALLOC_WITH_KMALLOC;

        OnCommandComplete on_complete;
        GC::Root<DOM::DocumentObserver> observer;
        Optional<WebIDL::UnsignedLong> callback_id;

        void stop_waiting_for_frame()
        {
            if (!observer)
                return;
            if (auto window = observer->document()->window(); window && callback_id.has_value())
                window->animation_frame_callback_driver().remove(*callback_id);
            callback_id.clear();
            observer->set_document_became_inactive(nullptr);
            observer->set_document_visibility_state_observer(nullptr);
            observer = {};
        }

        void finish(Response response)
        {
            auto callback = move(on_complete);
            stop_waiting_for_frame();
            if (callback)
                callback(move(response));
        }

        void complete(Response response)
        {
            // NB: Removing the callback may release the last other reference to this state.
            auto protect = NonnullRefPtr { *this };
            finish(move(response));
        }

        void fail()
        {
            complete(Error::from_code(ErrorCode::UnsupportedOperation, "Unable to capture a screenshot of the navigable"sv));
        }

        ~State()
        {
            finish(Error::from_code(ErrorCode::UnsupportedOperation, "Unable to capture a screenshot of the navigable"sv));
        }
    };
    auto state = adopt_ref(*new State);
    state->on_complete = move(on_complete);
    state->observer = DOM::DocumentObserver::create(document);
    state->observer->set_document_became_inactive([weak_state = state->make_weak_ptr()] {
        if (weak_state)
            weak_state->fail();
    });
    state->observer->set_document_visibility_state_observer([weak_state = state->make_weak_ptr()](HTML::VisibilityState visibility) {
        if (visibility == HTML::VisibilityState::Hidden && weak_state)
            weak_state->fail();
    });

    // 5. Immediately after the next invocation of the run the animation frame callbacks algorithm for document:
    state->callback_id = window->animation_frame_callback_driver().add_after_callbacks(GC::create_function(GC::Heap::the(), [state, document, navigable = GC::Ref { *local_navigable }, origin = move(origin), clip = move(clip), max_width, max_height, format_type = move(format_type), format_quality](double) mutable {
        if (!state->on_complete)
            return;
        state->stop_waiting_for_frame();
        if (document->hidden() || !document->is_fully_active() || navigable->active_document() != document)
            return state->fail();
        auto& settings = document->relevant_settings_object();
        HTML::TemporaryExecutionContext execution_context { settings };

        // NB: A viewport change can leave layout stale when the animation frame callback runs.
        document->update_layout(DOM::UpdateLayoutReason::ProcessScreenshot);

        // 7. Let origin rect be the result of trying to get the origin rectangle given origin and document.
        auto origin_rect = get_the_origin_rectangle(document, origin);

        // 8. Let clip rect be origin rect.
        auto clip_rect = origin_rect;

        // 9. If command parameters contains "clip":
        if (clip.has_value()) {
            // 1. Let clip be command parameters["clip"].
            // 2. Run the steps under the first matching condition:
            // -> clip matches the browsingContext.ElementClipRectangle production:
            if (clip->get_string("type"sv) == "element"sv) {
                // 1. Let environment settings be the environment settings object whose relevant global object's
                //    associated Document is document.
                // 2. Let realm be environment settings' realm execution context's Realm component.
                // 3. Let element be the result of trying to deserialize remote reference with clip["element"], realm, and
                //    session.
                auto element_value = deserialize_shared_reference(*clip->get_object("element"sv), settings);
                if (element_value.is_error())
                    return state->complete(element_value.release_error());

                // 4. If element doesn't implement Element return error with error code no such element.
                auto* element = element_value.value().is_object() ? Bindings::impl_from<DOM::Element>(&element_value.value().as_object()) : nullptr;
                if (!element)
                    return state->complete(Error::from_code(ErrorCode::NoSuchElement, "The clip is not an element"sv));

                // 5. If element's node document is not document, return error with error code no such element.
                if (&element->document() != document.ptr())
                    return state->complete(Error::from_code(ErrorCode::NoSuchElement, "The clip element is not in the document"sv));

                // 6. Let viewport rect be get the origin rectangle given "viewport" and document.
                auto viewport_rect = get_the_origin_rectangle(document, "viewport"sv);

                // 7. Let element rect be get the bounding box for element.
                auto element_rect = element->get_bounding_client_rect();

                // 8. Let clip rect be a DOMRectReadOnly with x coordinate element rect["x"] + viewport rect["x"], y
                //    coordinate element rect["y"] + viewport rect["y"], width element rect["width"], and height element
                //    rect["height"].
                clip_rect = {
                    element_rect.x().to_double() + viewport_rect.x,
                    element_rect.y().to_double() + viewport_rect.y,
                    element_rect.width().to_double(),
                    element_rect.height().to_double(),
                };
            }
            // -> clip matches the browsingContext.BoxClipRectangle production:
            else {
                // 1. Let clip x be clip["x"] plus origin rect's x coordinate.
                // 2. Let clip y be clip["y"] plus origin rect's y coordinate.
                // 3. Let clip rect be a DOMRectReadOnly with x coordinate clip x, y coordinate clip y, width clip["width"],
                //    and height clip["height"].
                clip_rect = {
                    clip->get_double_with_precision_loss("x"sv).value() + origin_rect.x,
                    clip->get_double_with_precision_loss("y"sv).value() + origin_rect.y,
                    clip->get_double_with_precision_loss("width"sv).value(),
                    clip->get_double_with_precision_loss("height"sv).value(),
                };
            }
        }

        // NB: All coordinates are now measured from the origin of the document.

        // 10. Let rect be the rectangle intersection of origin rect and clip rect.
        auto rect = rectangle_intersection(origin_rect, clip_rect);

        // 11. If rect's width dimension is 0 or rect's height dimension is 0, return error with error code unable to
        //     capture screen.
        if (rect.width == 0 || rect.height == 0)
            return state->complete(Error::from_code(ErrorCode::UnableToCaptureScreen, "The screenshot area is empty"sv));

        // 12. Let image size be the value of the imageSize field of command parameters if present, or null otherwise.
        // 13. Let canvas be render document to a canvas with document, rect and image size.
        // https://w3c.github.io/webdriver-bidi/#render-document-to-a-canvas
        // 1. Let ratio be determine the device pixel ratio given document's default view.
        auto ratio = document->window()->device_pixel_ratio();

        // 2. Let framebuffer width be rect's width dimension multiplied by ratio, so it matches the width of rect in
        //    device pixels.
        auto framebuffer_width = rect.width * ratio;

        // 3. Let framebuffer height be rect's height dimension multiplied by ratio, so it matches the height of rect in
        //    device pixels.
        auto framebuffer_height = rect.height * ratio;

        // NB: A framebuffer larger than a bitmap can be is a screenshot the implementation is unable to capture.
        static constexpr double MAXIMUM_FRAMEBUFFER_DIMENSION = 32768;
        if (!(framebuffer_width <= MAXIMUM_FRAMEBUFFER_DIMENSION) || !(framebuffer_height <= MAXIMUM_FRAMEBUFFER_DIMENSION))
            return state->complete(Error::from_code(ErrorCode::UnsupportedOperation, "The screenshot is too large"sv));

        // 4. Let scale be 1.
        double scale = 1;

        // 5. If image size is not null:
        //     1. Let max width be the maxWidth field of image size if present, or null otherwise.
        //     2. Let max height be the maxHeight field of image size if present, or null otherwise.
        //     3. If max width is not null and max width divided by framebuffer width is less than scale, set scale to
        //        max width divided by framebuffer width.
        if (max_width.has_value() && static_cast<double>(*max_width) / framebuffer_width < scale)
            scale = static_cast<double>(*max_width) / framebuffer_width;

        //     4. If max height is not null and max height divided by framebuffer height is less than scale, set scale to
        //        max height divided by framebuffer height.
        if (max_height.has_value() && static_cast<double>(*max_height) / framebuffer_height < scale)
            scale = static_cast<double>(*max_height) / framebuffer_height;

        double paint_width = 0;
        double paint_height = 0;

        // 6. If scale is less than 1:
        if (scale < 1) {
            // 1. Let scale width be round a number given framebuffer width multiplied by scale.
            // 2. Let scale height be round a number given framebuffer height multiplied by scale.
            // 3. Let paint width be the maximum of 1 and scale width.
            // 4. Let paint height be the maximum of 1 and scale height.
            paint_width = max(1.0, round_a_number(framebuffer_width * scale));
            paint_height = max(1.0, round_a_number(framebuffer_height * scale));
        }
        // 7. Otherwise:
        else {
            // 1. Let paint width be framebuffer width.
            // 2. Let paint height be framebuffer height.
            paint_width = framebuffer_width;
            paint_height = framebuffer_height;
        }

        // 11. Let source x be rect's x coordinate multiplied by ratio, and let source y be rect's y coordinate multiplied
        //     by ratio.
        // NB: The compositor captures the viewport, so the source rectangle is measured from the viewport's origin.
        // FIXME: Paint the parts of the document outside the viewport that the "document" origin includes, which are
        //        left blank.
        auto viewport_rect = get_the_origin_rectangle(document, "viewport"sv);
        auto source_x = (rect.x - viewport_rect.x) * ratio;
        auto source_y = (rect.y - viewport_rect.y) * ratio;
        if (!(fabs(source_x) <= NumericLimits<i32>::max() / 2) || !(fabs(source_y) <= NumericLimits<i32>::max() / 2))
            return state->complete(Error::from_code(ErrorCode::UnsupportedOperation, "The screenshot is too large"sv));

        // 12. Let source rectangle be the rectangle whose corners are the four points (source x, source y), (source x +
        //     framebuffer width, source y), (source x + framebuffer width, source y + framebuffer height), and (source x,
        //     source y + framebuffer height).
        Gfx::IntRect source_rectangle {
            static_cast<int>(round(source_x)),
            static_cast<int>(round(source_y)),
            max(1, static_cast<int>(round(framebuffer_width))),
            max(1, static_cast<int>(round(framebuffer_height))),
        };

        // 13. Let destination rectangle be the rectangle whose corners are the four points (0, 0), (paint width, 0),
        //     (paint width, paint height), and (0, paint height).
        Gfx::IntSize destination_size { max(1, static_cast<int>(paint_width)), max(1, static_cast<int>(paint_height)) };

        // 14. Complete implementation specific steps equivalent to painting the region of document's framebuffer
        //     specified by source rectangle on the region of canvas context's output bitmap specified by destination
        //     rectangle.
        auto viewport_device_rect = document->page().enclosing_device_rect(navigable->viewport_rect()).to_type<int>();
        if (viewport_device_rect.is_empty())
            return state->complete(Error::from_code(ErrorCode::UnableToCaptureScreen, "The viewport is empty"sv));

        auto framebuffer = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, viewport_device_rect.size());
        if (framebuffer.is_error())
            return state->complete(Error::from_code(ErrorCode::UnableToCaptureScreen, "Unable to allocate the screenshot"sv));

        auto bitmap = framebuffer.release_value();
        HTML::PaintConfig paint_config { .canvas_fill_rect = Gfx::IntRect { {}, viewport_device_rect.size() } };
        navigable->render_screenshot(bitmap, paint_config, [state, framebuffer = bitmap, source_rectangle, destination_size, format_type = move(format_type), format_quality] mutable {
            auto cropped = framebuffer->cropped(source_rectangle);
            // NB: A screenshot too large to allocate is an unsupported operation.
            if (cropped.is_error())
                return state->complete(Error::from_code(ErrorCode::UnsupportedOperation, "The screenshot is too large"sv));

            auto canvas = scale_bitmap(cropped.value(), destination_size);
            if (canvas.is_error())
                return state->complete(Error::from_code(ErrorCode::UnableToCaptureScreen, "Unable to scale the screenshot"sv));

            // 14. Let format be the format field of command parameters.
            // 15. Let encoding result be the result of trying to encode a canvas as Base64 with canvas and format.
            auto encoding_result = encode_a_canvas_as_base64(canvas.value(), format_type, format_quality);
            if (encoding_result.is_error())
                return state->complete(move(encoding_result));

            // 16. Let body be a map matching the browsingContext.CaptureScreenshotResult production, with the data field
            //     set to encoding result.
            JsonObject body;
            body.set("data"sv, encoding_result.release_value());

            // 17. Return success with data body.
            state->complete(JsonValue { move(body) });
        });
    }));
    document->page().client().request_frame();
}

}
