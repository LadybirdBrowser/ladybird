/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLOptionsCollection.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebDriver/Contexts.h>
#include <LibWeb/WebDriver/ExecuteScript.h>

namespace Web::WebDriver {

#define TRY_OR_JS_ERROR(expression)                                                                  \
    ({                                                                                               \
        auto&& _temporary_result = (expression);                                                     \
        if (_temporary_result.is_error()) [[unlikely]]                                               \
            return ExecuteScriptResultType::JavaScriptError;                                         \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        _temporary_result.release_value();                                                           \
    })

static ErrorOr<JsonValue, ExecuteScriptResultType> internal_json_clone_algorithm(JS::Realm&, JS::Value, HashTable<JS::Object*>& seen);
static ErrorOr<JsonValue, ExecuteScriptResultType> clone_an_object(JS::Realm&, JS::Object&, HashTable<JS::Object*>& seen, auto const& clone_algorithm);

// https://w3c.github.io/webdriver/#dfn-collection
static bool is_collection(JS::Object const& value)
{
    // A collection is an Object that implements the Iterable interface, and whose:
    return (
        // - initial value of the toString own property is "Arguments"
        value.has_parameter_map()
        // - instance of Array
        || is<JS::Array>(value)
        // - instance of FileList
        || is<FileAPI::FileList>(value)
        // - instance of HTMLAllCollection
        || false // FIXME
        // - instance of HTMLCollection
        || is<DOM::HTMLCollection>(value)
        // - instance of HTMLFormControlsCollection
        || false // FIXME
        // - instance of HTMLOptionsCollection
        || is<HTML::HTMLOptionsCollection>(value)
        // - instance of NodeList
        || is<DOM::NodeList>(value));
}

// https://w3c.github.io/webdriver/#dfn-json-clone
static ErrorOr<JsonValue, ExecuteScriptResultType> json_clone(JS::Realm& realm, JS::Value value)
{
    // To perform a JSON clone return the result of calling the internal JSON clone algorithm with arguments value and an empty List.
    auto seen = HashTable<JS::Object*> {};
    return internal_json_clone_algorithm(realm, value, seen);
}

// https://w3c.github.io/webdriver/#dfn-internal-json-clone-algorithm
static ErrorOr<JsonValue, ExecuteScriptResultType> internal_json_clone_algorithm(JS::Realm& realm, JS::Value value, HashTable<JS::Object*>& seen)
{
    auto& vm = realm.vm();

    // When required to run the internal JSON clone algorithm with arguments value and seen, a remote end must return the value of the first matching statement, matching on value:
    // -> undefined
    // -> null
    if (value.is_nullish()) {
        // Success with data null.
        return JsonValue {};
    }

    // -> type Boolean
    // -> type Number
    // -> type String
    //     Success with data value.
    if (value.is_boolean())
        return JsonValue { value.as_bool() };
    if (value.is_number())
        return JsonValue { value.as_double() };
    if (value.is_string())
        return JsonValue { value.as_string().byte_string() };

    // NOTE: BigInt and Symbol not mentioned anywhere in the WebDriver spec, as it references ES5.
    //       It assumes that all primitives are handled above, and the value is an object for the remaining steps.
    if (value.is_bigint() || value.is_symbol())
        return ExecuteScriptResultType::JavaScriptError;

    // FIXME: -> a collection
    // FIXME: -> instance of element
    // FIXME: -> instance of shadow root

    // -> a WindowProxy object
    if (is<HTML::WindowProxy>(value.as_object())) {
        auto const& window_proxy = static_cast<HTML::WindowProxy&>(value.as_object());

        // If the associated browsing context of the WindowProxy object in value has been destroyed, return error with
        // error code stale element reference.
        if (window_proxy.associated_browsing_context()->has_navigable_been_destroyed())
            return ExecuteScriptResultType::BrowsingContextDiscarded;

        // Otherwise return success with data set to WindowProxy reference object for value.
        return window_proxy_reference_object(window_proxy);
    }

    // -> has an own property named "toJSON" that is a Function
    auto to_json = value.as_object().get_without_side_effects(vm.names.toJSON);
    if (to_json.is_function()) {
        // Return success with the value returned by Function.[[Call]](toJSON) with value as the this value.
        auto to_json_result = TRY_OR_JS_ERROR(to_json.as_function().internal_call(value, JS::MarkedVector<JS::Value> { vm.heap() }));
        if (!to_json_result.is_string())
            return ExecuteScriptResultType::JavaScriptError;
        return to_json_result.as_string().byte_string();
    }

    // -> Otherwise
    // 1. If value is in seen, return error with error code javascript error.
    if (seen.contains(&value.as_object()))
        return ExecuteScriptResultType::JavaScriptError;

    // 2. Append value to seen.
    seen.set(&value.as_object());

    ScopeGuard remove_seen { [&] {
        // 4. Remove the last element of seen.
        seen.remove(&value.as_object());
    } };

    // 3. Let result be the value of running the clone an object algorithm with arguments value and seen, and the internal JSON clone algorithm as the clone algorithm.
    auto result = TRY(clone_an_object(realm, value.as_object(), seen, internal_json_clone_algorithm));

    // 5. Return result.
    return result;
}

// https://w3c.github.io/webdriver/#dfn-clone-an-object
static ErrorOr<JsonValue, ExecuteScriptResultType> clone_an_object(JS::Realm& realm, JS::Object& value, HashTable<JS::Object*>& seen, auto const& clone_algorithm)
{
    auto& vm = realm.vm();

    // 1. Let result be the value of the first matching statement, matching on value:
    auto get_result = [&]() -> ErrorOr<Variant<JsonArray, JsonObject>, ExecuteScriptResultType> {
        // -> a collection
        if (is_collection(value)) {
            // A new Array which length property is equal to the result of getting the property length of value.
            auto length_property = TRY_OR_JS_ERROR(value.internal_get_own_property(vm.names.length));
            if (!length_property->value.has_value())
                return ExecuteScriptResultType::JavaScriptError;
            auto length = TRY_OR_JS_ERROR(length_property->value->to_length(vm));
            if (length > NumericLimits<u32>::max())
                return ExecuteScriptResultType::JavaScriptError;
            auto array = JsonArray {};
            for (size_t i = 0; i < length; ++i)
                array.must_append(JsonValue {});
            return array;
        }
        // -> Otherwise
        else {
            // A new Object.
            return JsonObject {};
        }
    };
    auto result = TRY(get_result());

    // 2. For each enumerable own property in value, run the following substeps:
    for (auto& key : MUST(value.Object::internal_own_property_keys())) {
        // 1. Let name be the name of the property.
        auto name = MUST(JS::PropertyKey::from_value(vm, key));

        if (!value.storage_get(name)->attributes.is_enumerable())
            continue;

        // 2. Let source property value be the result of getting a property named name from value. If doing so causes script to be run and that script throws an error, return error with error code javascript error.
        auto source_property_value = TRY_OR_JS_ERROR(value.internal_get_own_property(name));
        if (!source_property_value.has_value() || !source_property_value->value.has_value())
            continue;

        // 3. Let cloned property result be the result of calling the clone algorithm with arguments source property value and seen.
        auto cloned_property_result = clone_algorithm(realm, *source_property_value->value, seen);

        // 4. If cloned property result is a success, set a property of result with name name and value equal to cloned property result’s data.
        if (!cloned_property_result.is_error()) {
            result.visit(
                [&](JsonArray& array) {
                    // NOTE: If this was a JS array, only indexed properties would be serialized anyway.
                    if (name.is_number())
                        array.set(name.as_number(), cloned_property_result.value());
                },
                [&](JsonObject& object) {
                    object.set(name.to_string(), cloned_property_result.value());
                });
        }
        // 5. Otherwise, return cloned property result.
        else {
            return cloned_property_result;
        }
    }

    return result.visit([&](auto const& value) -> JsonValue { return value; });
}

// https://w3c.github.io/webdriver/#dfn-execute-a-function-body
static JS::ThrowCompletionOr<JS::Value> execute_a_function_body(Web::Page& page, ByteString const& body, JS::MarkedVector<JS::Value> parameters)
{
    // FIXME: If at any point during the algorithm a user prompt appears, immediately return Completion { [[Type]]: normal, [[Value]]: null, [[Target]]: empty }, but continue to run the other steps of this algorithm in parallel.

    // 1. Let window be the associated window of the current browsing context’s active document.
    // FIXME: This will need adjusting when WebDriver supports frames.
    auto window = page.top_level_browsing_context().active_document()->window();

    // 2. Let environment settings be the environment settings object for window.
    auto& environment_settings = Web::HTML::relevant_settings_object(*window);

    // 3. Let global scope be environment settings realm’s global environment.
    auto& global_scope = environment_settings.realm().global_environment();

    auto& realm = window->realm();

    auto source_text = ByteString::formatted("function() {{ {} }}", body);
    auto parser = JS::Parser { JS::Lexer { source_text } };
    auto function_expression = parser.parse_function_node<JS::FunctionExpression>();

    // 4. If body is not parsable as a FunctionBody or if parsing detects an early error, return Completion { [[Type]]: normal, [[Value]]: null, [[Target]]: empty }.
    if (parser.has_errors())
        return JS::js_null();

    // 5. If body begins with a directive prologue that contains a use strict directive then let strict be true, otherwise let strict be false.
    // NOTE: Handled in step 8 below.

    // 6. Prepare to run a script with environment settings.
    environment_settings.prepare_to_run_script();

    // 7. Prepare to run a callback with environment settings.
    environment_settings.prepare_to_run_callback();

    // 8. Let function be the result of calling FunctionCreate, with arguments:
    // kind
    //    Normal.
    // list
    //    An empty List.
    // body
    //    The result of parsing body above.
    // global scope
    //    The result of parsing global scope above.
    // strict
    //    The result of parsing strict above.
    auto function = JS::ECMAScriptFunctionObject::create(realm, "", move(source_text), function_expression->body(), function_expression->parameters(), function_expression->function_length(), function_expression->local_variables_names(), &global_scope, nullptr, function_expression->kind(), function_expression->is_strict_mode(), function_expression->parsing_insights());

    // 9. Let completion be Function.[[Call]](window, parameters) with function as the this value.
    // NOTE: This is not entirely clear, but I don't think they mean actually passing `function` as
    // the this value argument, but using it as the object [[Call]] is executed on.
    auto completion = function->internal_call(window, move(parameters));

    // 10. Clean up after running a callback with environment settings.
    environment_settings.clean_up_after_running_callback();

    // 11. Clean up after running a script with environment settings.
    environment_settings.clean_up_after_running_script();

    // 12. Return completion.
    return completion;
}

ExecuteScriptResultSerialized execute_script(Web::Page& page, ByteString const& body, JS::MarkedVector<JS::Value> arguments, Optional<u64> const& timeout)
{
    // FIXME: Use timeout.
    (void)timeout;

    auto* window = page.top_level_browsing_context().active_window();
    auto& realm = window->realm();

    // 4. Let promise be a new Promise.
    // NOTE: For now we skip this and handle a throw completion manually instead of using 'promise-calling'.

    // FIXME: 5. Run the following substeps in parallel:
    auto result = [&] {
        // 1. Let scriptPromise be the result of promise-calling execute a function body, with arguments body and arguments.
        auto completion = execute_a_function_body(page, body, move(arguments));

        // 2. Upon fulfillment of scriptPromise with value v, resolve promise with value v.
        // 3. Upon rejection of scriptPromise with value r, reject promise with value r.
        auto result_type = completion.is_error()
            ? ExecuteScriptResultType::PromiseRejected
            : ExecuteScriptResultType::PromiseResolved;
        auto result_value = completion.is_error()
            ? *completion.throw_completion().value()
            : completion.value();

        return ExecuteScriptResult { result_type, result_value };
    }();

    // FIXME: 6. If promise is still pending and the session script timeout is reached, return error with error code script timeout.
    // 7. Upon fulfillment of promise with value v, let result be a JSON clone of v, and return success with data result.
    // 8. Upon rejection of promise with reason r, let result be a JSON clone of r, and return error with error code javascript error and data result.
    auto json_value_or_error = json_clone(realm, result.value);
    if (json_value_or_error.is_error()) {
        auto error_object = JsonObject {};
        error_object.set("name", "Error");
        error_object.set("message", "Could not clone result value");
        return { ExecuteScriptResultType::JavaScriptError, move(error_object) };
    }
    return { result.type, json_value_or_error.release_value() };
}

ExecuteScriptResultSerialized execute_async_script(Web::Page& page, ByteString const& body, JS::MarkedVector<JS::Value> arguments, Optional<u64> const& timeout)
{
    auto* document = page.top_level_browsing_context().active_document();
    auto* window = page.top_level_browsing_context().active_window();
    auto& realm = window->realm();
    auto& vm = window->vm();
    auto start = MonotonicTime::now();

    auto has_timed_out = [&] {
        return timeout.has_value() && (MonotonicTime::now() - start) > AK::Duration::from_seconds(static_cast<i64>(*timeout));
    };

    // AD-HOC: An execution context is required for Promise creation hooks.
    HTML::TemporaryExecutionContext execution_context { document->relevant_settings_object() };

    // 4. Let promise be a new Promise.
    auto promise_capability = WebIDL::create_promise(realm);
    JS::NonnullGCPtr promise { verify_cast<JS::Promise>(*promise_capability->promise()) };

    // FIXME: 5 Run the following substeps in parallel:
    [&] {
        // 1. Let resolvingFunctions be CreateResolvingFunctions(promise).
        auto resolving_functions = promise->create_resolving_functions();

        // 2. Append resolvingFunctions.[[Resolve]] to arguments.
        arguments.append(resolving_functions.resolve);

        // 3. Let result be the result of calling execute a function body, with arguments body and arguments.
        // FIXME: 'result' -> 'scriptResult' (spec issue)
        auto script_result = execute_a_function_body(page, body, move(arguments));

        // 4.If scriptResult.[[Type]] is not normal, then reject promise with value scriptResult.[[Value]], and abort these steps.
        // NOTE: Prior revisions of this specification did not recognize the return value of the provided script.
        //       In order to preserve legacy behavior, the return value only influences the command if it is a
        //       "thenable"  object or if determining this produces an exception.
        if (script_result.is_throw_completion()) {
            promise->reject(*script_result.throw_completion().value());
            return;
        }

        // 5. If Type(scriptResult.[[Value]]) is not Object, then abort these steps.
        if (!script_result.value().is_object())
            return;

        // 6. Let then be Get(scriptResult.[[Value]], "then").
        auto then = script_result.value().as_object().get(vm.names.then);

        // 7. If then.[[Type]] is not normal, then reject promise with value then.[[Value]], and abort these steps.
        if (then.is_throw_completion()) {
            promise->reject(*then.throw_completion().value());
            return;
        }

        // 8. If IsCallable(then.[[Type]]) is false, then abort these steps.
        if (!then.value().is_function())
            return;

        // 9. Let scriptPromise be PromiseResolve(Promise, scriptResult.[[Value]]).
        auto script_promise_or_error = JS::promise_resolve(vm, realm.intrinsics().promise_constructor(), script_result.value());
        if (script_promise_or_error.is_throw_completion())
            return;
        auto& script_promise = static_cast<JS::Promise&>(*script_promise_or_error.value());

        vm.custom_data()->spin_event_loop_until([&] {
            if (script_promise.state() != JS::Promise::State::Pending)
                return true;
            if (has_timed_out())
                return true;
            return false;
        });

        // 10. Upon fulfillment of scriptPromise with value v, resolve promise with value v.
        if (script_promise.state() == JS::Promise::State::Fulfilled)
            WebIDL::resolve_promise(realm, promise_capability, script_promise.result());

        // 11. Upon rejection of scriptPromise with value r, reject promise with value r.
        if (script_promise.state() == JS::Promise::State::Rejected)
            WebIDL::reject_promise(realm, promise_capability, script_promise.result());
    }();

    // 6. If promise is still pending and session script timeout milliseconds is reached, return error with error code script timeout.
    vm.custom_data()->spin_event_loop_until([&] {
        if (has_timed_out()) {
            return true;
        }

        return promise->state() != JS::Promise::State::Pending;
    });

    if (has_timed_out()) {
        auto error_object = JsonObject {};
        error_object.set("name", "Error");
        error_object.set("message", "script timeout");
        return { ExecuteScriptResultType::Timeout, move(error_object) };
    }

    auto json_value_or_error = json_clone(realm, promise->result());
    if (json_value_or_error.is_error()) {
        auto error_object = JsonObject {};
        error_object.set("name", "Error");
        error_object.set("message", "Could not clone result value");
        return { ExecuteScriptResultType::JavaScriptError, move(error_object) };
    }

    // 7. Upon fulfillment of promise with value v, let result be a JSON clone of v, and return success with data result.
    if (promise->state() == JS::Promise::State::Fulfilled) {
        return { ExecuteScriptResultType::PromiseResolved, json_value_or_error.release_value() };
    }
    // 8. Upon rejection of promise with reason r, let result be a JSON clone of r, and return error with error code javascript error and data result.
    if (promise->state() == JS::Promise::State::Rejected) {
        return { ExecuteScriptResultType::PromiseRejected, json_value_or_error.release_value() };
    }

    VERIFY_NOT_REACHED();
}

}
