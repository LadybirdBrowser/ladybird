/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Parser.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebDriver/ExecuteScript.h>
#include <LibWeb/WebDriver/HeapTimer.h>

namespace Web::WebDriver {

// https://w3ctag.github.io/promises-guide/#should-promise-call
static GC::Ref<WebIDL::Promise> promise_call(JS::Realm& realm, JS::ThrowCompletionOr<JS::Value> result)
{
    // If the developer supplies you with a function that you expect to return a promise, you should also allow it to
    // return a thenable or non-promise value, or even throw an exception, and treat all these cases as if they had
    // returned an analogous promise. This should be done by converting the returned value to a promise, as if by using
    // Promise.resolve(), and catching thrown exceptions and converting those into a promise as if by using
    // Promise.reject(). We call this "promise-calling" the function.
    if (result.is_error())
        return WebIDL::create_rejected_promise(realm, result.error_value());
    return WebIDL::create_resolved_promise(realm, result.release_value());
}

// https://w3c.github.io/webdriver/#dfn-execute-a-function-body
static JS::ThrowCompletionOr<JS::Value> execute_a_function_body(HTML::BrowsingContext const& browsing_context, StringView body, ReadonlySpan<JS::Value> parameters)
{
    // 1. Let window be the associated window of the current browsing context’s active document.
    auto window = browsing_context.active_document()->window();

    // 2. Let environment settings be the environment settings object for window.
    auto& environment_settings = Web::HTML::relevant_settings_object(*window);

    // 3. Let global scope be environment settings realm’s global environment.
    auto& realm = environment_settings.realm();
    auto& global_scope = realm.global_environment();

    // FIXME: This does not handle scripts which contain `await` statements. It is not as as simple as declaring this
    //        function async, unfortunately. See: https://github.com/w3c/webdriver/issues/1436
    auto source_text = ByteString::formatted(
        R"~~~(function() {{
            {}
        }})~~~",
        body);

    auto parser = JS::Parser { JS::Lexer { source_text } };
    auto function_expression = parser.parse_function_node<JS::FunctionExpression>();

    // 4. If body is not parsable as a FunctionBody or if parsing detects an early error, return Completion { [[Type]]: normal, [[Value]]: null, [[Target]]: empty }.
    if (parser.has_errors())
        return JS::js_null();

    // 5. If body begins with a directive prologue that contains a use strict directive then let strict be true, otherwise let strict be false.
    // NOTE: Handled in step 8 below.

    // 6. Prepare to run a script with realm.
    HTML::prepare_to_run_script(realm);

    // 7. Prepare to run a callback with environment settings.
    HTML::prepare_to_run_callback(realm);

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
    auto function = JS::ECMAScriptFunctionObject::create_from_function_node(
        function_expression,
        ""_fly_string,
        realm,
        &global_scope,
        nullptr);

    // 9. Let completion be Function.[[Call]](window, parameters) with function as the this value.
    // NOTE: This is not entirely clear, but I don't think they mean actually passing `function` as
    // the this value argument, but using it as the object [[Call]] is executed on.
    auto completion = JS::call(realm.vm(), *function, window, parameters);

    // 10. Clean up after running a callback with environment settings.
    HTML::clean_up_after_running_callback(realm);

    // 11. Clean up after running a script with realm.
    HTML::clean_up_after_running_script(realm);

    // 12. Return completion.
    return completion;
}

static void fire_completion_when_resolved(GC::Ref<WebIDL::Promise> promise, GC::Ref<HeapTimer> timer, GC::Ref<OnScriptComplete> on_complete)
{
    auto reaction_steps = GC::create_function(promise->heap(), [promise, timer, on_complete](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        if (timer->is_timed_out())
            return JS::js_undefined();
        timer->stop();

        auto const& underlying_promise = as<JS::Promise>(*promise->promise());
        on_complete->function()({ underlying_promise.state(), underlying_promise.result() });

        return JS::js_undefined();
    });

    WebIDL::react_to_promise(promise, reaction_steps, reaction_steps);
}

void execute_script(HTML::BrowsingContext const& browsing_context, String body, GC::RootVector<JS::Value> arguments, Optional<u64> const& timeout_ms, GC::Ref<OnScriptComplete> on_complete)
{
    auto const* document = browsing_context.active_document();
    auto& realm = document->realm();
    auto& vm = document->vm();

    // 5. Let timer be a new timer.
    auto timer = realm.create<HeapTimer>();

    // 6. If timeout is not null:
    if (timeout_ms.has_value()) {
        // 1. Start the timer with timer and timeout.
        timer->start(timeout_ms.value(), GC::create_function(vm.heap(), [on_complete]() {
            on_complete->function()({ .state = JS::Promise::State::Pending });
        }));
    }

    // AD-HOC: An execution context is required for Promise creation hooks.
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 7. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 8. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, &browsing_context, promise, body = move(body), arguments = move(arguments)]() mutable {
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let scriptPromise be the result of promise-calling execute a function body, with arguments body and arguments.
        auto script_promise = promise_call(realm, execute_a_function_body(browsing_context, body, arguments));

        WebIDL::react_to_promise(script_promise,
            // 2. Upon fulfillment of scriptPromise with value v, resolve promise with value v.
            GC::create_function(realm.heap(), [&realm, promise](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::resolve_promise(realm, promise, value);
                return JS::js_undefined();
            }),

            // 3. Upon rejection of scriptPromise with value r, reject promise with value r.
            GC::create_function(realm.heap(), [&realm, promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::reject_promise(realm, promise, reason);
                return JS::js_undefined();
            }));
    }));

    // 9. Wait until promise is resolved, or timer's timeout fired flag is set, whichever occurs first.
    fire_completion_when_resolved(promise, timer, on_complete);
}

// https://w3c.github.io/webdriver/#execute-async-script
void execute_async_script(HTML::BrowsingContext const& browsing_context, String body, GC::RootVector<JS::Value> arguments, Optional<u64> const& timeout_ms, GC::Ref<OnScriptComplete> on_complete)
{
    auto const* document = browsing_context.active_document();
    auto& realm = document->realm();
    auto& vm = document->vm();

    // 5. Let timer be a new timer.
    auto timer = realm.create<HeapTimer>();

    // 6. If timeout is not null:
    if (timeout_ms.has_value()) {
        // 1. Start the timer with timer and timeout.
        timer->start(timeout_ms.value(), GC::create_function(vm.heap(), [on_complete]() {
            on_complete->function()({ .state = JS::Promise::State::Pending });
        }));
    }

    // AD-HOC: An execution context is required for Promise creation hooks.
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 7. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 8. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&vm, &realm, &browsing_context, promise, body = move(body), arguments = move(arguments)]() mutable {
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let resolvingFunctions be CreateResolvingFunctions(promise).
        auto resolving_functions = as<JS::Promise>(*promise->promise()).create_resolving_functions();

        // 2. Append resolvingFunctions.[[Resolve]] to arguments.
        arguments.append(resolving_functions.resolve);

        // 3. Let scriptResult be the result of calling execute a function body, with arguments body and arguments.
        auto script_result = execute_a_function_body(browsing_context, body, move(arguments));

        // 4. If scriptResult.[[Type]] is not normal, then reject promise with value scriptResult.[[Value]], and abort these steps.
        // NOTE: Prior revisions of this specification did not recognize the return value of the provided script.
        //       In order to preserve legacy behavior, the return value only influences the command if it is a
        //       "thenable"  object or if determining this produces an exception.
        if (script_result.is_throw_completion()) {
            WebIDL::reject_promise(realm, promise, script_result.error_value());
            return;
        }

        // 5. If Type(scriptResult.[[Value]]) is not Object, then abort these steps.
        if (!script_result.value().is_object())
            return;

        // 6. Let then be Get(scriptResult.[[Value]], "then").
        auto then = script_result.value().as_object().get(vm.names.then);

        // 7. If then.[[Type]] is not normal, then reject promise with value then.[[Value]], and abort these steps.
        if (then.is_throw_completion()) {
            WebIDL::reject_promise(realm, promise, then.error_value());
            return;
        }

        // 8. If IsCallable(then.[[Type]]) is false, then abort these steps.
        if (!then.value().is_function())
            return;

        // 9. Let scriptPromise be PromiseResolve(Promise, scriptResult.[[Value]]).
        auto script_promise = WebIDL::create_resolved_promise(realm, script_result.value());

        WebIDL::react_to_promise(script_promise,
            // 10. Upon fulfillment of scriptPromise with value v, resolve promise with value v.
            GC::create_function(realm.heap(), [&realm, promise](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::resolve_promise(realm, promise, value);
                return JS::js_undefined();
            }),

            // 11. Upon rejection of scriptPromise with value r, reject promise with value r.
            GC::create_function(realm.heap(), [&realm, promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::reject_promise(realm, promise, reason);
                return JS::js_undefined();
            }));
    }));

    // 9. Wait until promise is resolved, or timer's timeout fired flag is set, whichever occurs first.
    fire_completion_when_resolved(promise, timer, on_complete);
}

}
