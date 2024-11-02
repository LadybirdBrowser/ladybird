/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NumericLimits.h>
#include <AK/Variant.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLAllCollection.h>
#include <LibWeb/HTML/HTMLFormControlsCollection.h>
#include <LibWeb/HTML/HTMLOptionsCollection.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebDriver/Contexts.h>
#include <LibWeb/WebDriver/ElementReference.h>
#include <LibWeb/WebDriver/JSON.h>

namespace Web::WebDriver {

#define TRY_OR_JS_ERROR(expression)                                                                     \
    ({                                                                                                  \
        auto&& _temporary_result = (expression);                                                        \
        if (_temporary_result.is_error()) [[unlikely]]                                                  \
            return WebDriver::Error::from_code(ErrorCode::JavascriptError, "Script returned an error"); \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>,    \
            "Do not return a reference from a fallible expression");                                    \
        _temporary_result.release_value();                                                              \
    })

using SeenMap = HashTable<JS::RawGCPtr<JS::Object const>>;

// https://w3c.github.io/webdriver/#dfn-collection
static bool is_collection(JS::Object const& value)
{
    // A collection is an Object that implements the Iterable interface, and whose:
    return (
        // - initial value of the toString own property is "Arguments"
        value.has_parameter_map()
        // - instance of Array
        || is<JS::Array>(value)
        // - instance of DOMTokenList
        || is<DOM::DOMTokenList>(value)
        // - instance of FileList
        || is<FileAPI::FileList>(value)
        // - instance of HTMLAllCollection
        || is<HTML::HTMLAllCollection>(value)
        // - instance of HTMLCollection
        || is<DOM::HTMLCollection>(value)
        // - instance of HTMLFormControlsCollection
        || is<HTML::HTMLFormControlsCollection>(value)
        // - instance of HTMLOptionsCollection
        || is<HTML::HTMLOptionsCollection>(value)
        // - instance of NodeList
        || is<DOM::NodeList>(value));
}

// https://w3c.github.io/webdriver/#dfn-clone-an-object
static Response clone_an_object(HTML::BrowsingContext const& browsing_context, JS::Object const& value, SeenMap& seen, auto const& clone_algorithm)
{
    auto& vm = browsing_context.vm();

    // 1. If value is in seen, return error with error code javascript error.
    if (seen.contains(value))
        return WebDriver::Error::from_code(ErrorCode::JavascriptError, "Attempted to recursively clone an Object"sv);

    // 2. Append value to seen.
    seen.set(value);

    // 3. Let result be the value of the first matching statement, matching on value:
    auto result = TRY(([&]() -> Response {
        // -> a collection
        if (is_collection(value)) {
            // A new Array which length property is equal to the result of getting the property length of value.
            auto length_property = TRY_OR_JS_ERROR(value.get(vm.names.length));

            auto length = TRY_OR_JS_ERROR(length_property.to_length(vm));
            if (length > NumericLimits<u32>::max())
                return WebDriver::Error::from_code(ErrorCode::JavascriptError, "Length of Object too large"sv);

            return JsonValue { JsonArray { length } };
        }
        // -> Otherwise
        else {
            // A new Object.
            return JsonValue { JsonObject {} };
        }
    }()));

    Optional<WebDriver::Error> error;

    // 4. For each enumerable property in value, run the following substeps:
    (void)value.enumerate_object_properties([&](auto property) -> Optional<JS::Completion> {
        // 1. Let name be the name of the property.
        auto name = MUST(JS::PropertyKey::from_value(vm, property));

        // 2. Let source property value be the result of getting a property named name from value. If doing so causes
        //    script to be run and that script throws an error, return error with error code javascript error.
        auto source_property_value = value.get(name);
        if (source_property_value.is_error()) {
            error = WebDriver::Error::from_code(ErrorCode::JavascriptError, "Script returned an error");
            return JS::normal_completion({});
        }

        // 3. Let cloned property result be the result of calling the clone algorithm with session, source property
        //    value and seen.
        auto cloned_property_result = clone_algorithm(browsing_context, source_property_value.value(), seen);

        // 4. If cloned property result is a success, set a property of result with name name and value equal to cloned
        //    property result's data.
        if (!cloned_property_result.is_error()) {
            if (result.is_array() && name.is_number())
                result.as_array().set(name.as_number(), cloned_property_result.value());
            else if (result.is_object())
                result.as_object().set(name.to_string(), cloned_property_result.value());
        }
        // 5. Otherwise, return cloned property result.
        else {
            error = cloned_property_result.release_error();
            return JS::normal_completion({});
        }

        return {};
    });

    if (error.has_value())
        return error.release_value();

    // 5. Remove the last element of seen.
    seen.remove(value);

    // 6. Return success with data result.
    return result;
}

// https://w3c.github.io/webdriver/#dfn-internal-json-clone
static Response internal_json_clone(HTML::BrowsingContext const& browsing_context, JS::Value value, SeenMap& seen)
{
    auto& vm = browsing_context.vm();

    // To internal JSON clone given session, value and seen, return the value of the first matching statement, matching
    // on value:

    // -> undefined
    // -> null
    if (value.is_nullish()) {
        // Return success with data null.
        return JsonValue {};
    }

    // -> type Boolean
    // -> type Number
    // -> type String
    //     Return success with data value.
    if (value.is_boolean())
        return JsonValue { value.as_bool() };
    if (value.is_number())
        return JsonValue { value.as_double() };
    if (value.is_string())
        return JsonValue { value.as_string().byte_string() };

    // AD-HOC: BigInt and Symbol not mentioned anywhere in the WebDriver spec, as it references ES5.
    //         It assumes that all primitives are handled above, and the value is an object for the remaining steps.
    if (value.is_bigint())
        return WebDriver::Error::from_code(ErrorCode::JavascriptError, "Cannot clone a BigInt"sv);
    if (value.is_symbol())
        return WebDriver::Error::from_code(ErrorCode::JavascriptError, "Cannot clone a Symbol"sv);

    VERIFY(value.is_object());
    auto const& object = static_cast<JS::Object const&>(value.as_object());

    // -> instance of Element
    if (is<DOM::Element>(object)) {
        auto const& element = static_cast<DOM::Element const&>(object);

        // If the element is stale, return error with error code stale element reference.
        if (is_element_stale(element)) {
            return WebDriver::Error::from_code(ErrorCode::StaleElementReference, "Referenced element has become stale"sv);
        }
        // Otherwise:
        else {
            // 1. Let reference be the web element reference object for session and value.
            auto reference = web_element_reference_object(browsing_context, element);

            // 2. Return success with data reference.
            return JsonValue { move(reference) };
        }
    }

    // -> instance of ShadowRoot
    if (is<DOM::ShadowRoot>(object)) {
        auto const& shadow_root = static_cast<DOM::ShadowRoot const&>(object);

        // If the shadow root is detached, return error with error code detached shadow root.
        if (is_shadow_root_detached(shadow_root)) {
            return WebDriver::Error::from_code(ErrorCode::DetachedShadowRoot, "Referenced shadow root has become detached"sv);
        }
        // Otherwise:
        else {
            // 1. Let reference be the shadow root reference object for session and value.
            auto reference = shadow_root_reference_object(browsing_context, shadow_root);

            // 2. Return success with data reference.
            return JsonValue { move(reference) };
        }
    }

    // -> a WindowProxy object
    if (is<HTML::WindowProxy>(object)) {
        auto const& window_proxy = static_cast<HTML::WindowProxy const&>(object);

        // If the associated browsing context of the WindowProxy object in value has been destroyed, return error
        // with error code stale element reference.
        if (window_proxy.associated_browsing_context()->has_navigable_been_destroyed()) {
            return WebDriver::Error::from_code(ErrorCode::StaleElementReference, "Browsing context has been discarded"sv);
        }
        // Otherwise:
        else {
            // 1. Let reference be the WindowProxy reference object for value.
            auto reference = window_proxy_reference_object(window_proxy);

            // 2. Return success with data reference.
            return JsonValue { move(reference) };
        }
    }

    // -> has an own property named "toJSON" that is a Function
    if (auto to_json = object.get_without_side_effects(vm.names.toJSON); to_json.is_function()) {
        // Return success with the value returned by Function.[[Call]](toJSON) with value as the this value.
        auto to_json_result = TRY_OR_JS_ERROR(to_json.as_function().internal_call(value, JS::MarkedVector<JS::Value> { vm.heap() }));
        if (!to_json_result.is_string())
            return WebDriver::Error::from_code(ErrorCode::JavascriptError, "toJSON did not return a String"sv);

        return JsonValue { to_json_result.as_string().byte_string() };
    }

    // -> Otherwise
    // 1. Let result be clone an object with session value and seen, and internal JSON clone as the clone algorithm.
    auto result = TRY(clone_an_object(browsing_context, object, seen, internal_json_clone));

    // 2. Return success with data result.
    return result;
}

// https://w3c.github.io/webdriver/#dfn-json-clone
Response json_clone(HTML::BrowsingContext const& browsing_context, JS::Value value)
{
    SeenMap seen;

    // To JSON clone given session and value, return the result of internal JSON clone with session, value and an empty List.
    return internal_json_clone(browsing_context, value, seen);
}

}
