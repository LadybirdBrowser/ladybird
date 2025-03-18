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
    auto function = JS::ECMAScriptFunctionObject::create(realm, ""_fly_string, move(source_text), function_expression->body(), function_expression->parameters(), function_expression->function_length(), function_expression->local_variables_names(), &global_scope, nullptr, function_expression->kind(), function_expression->is_strict_mode(), function_expression->parsing_insights());

    // 9. Let completion be Function.[[Call]](window, parameters) with function as the this value.
    // NOTE: This is not entirely clear, but I don't think they mean actually passing `function` as
    // the this value argument, but using it as the object [[Call]] is executed on.
    auto completion = function->internal_call(window, parameters);

    // 10. Clean up after running a callback with environment settings.
    HTML::clean_up_after_running_callback(realm);

    // 11. Clean up after running a script with realm.
    HTML::clean_up_after_running_script(realm);

    // 12. Return completion.
    return completion;
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
        HTML::TemporaryExecutionContext execution_context { realm };

        // 1. Let scriptPromise be the result of promise-calling execute a function body, with arguments body and arguments.
        auto script_result = execute_a_function_body(browsing_context, body, move(arguments));

        // FIXME: This isn't right, we should be reacting to this using WebIDL::react_to_promise()
        // 2. Upon fulfillment of scriptPromise with value v, resolve promise with value v.
        if (script_result.has_value()) {
            WebIDL::resolve_promise(realm, promise, script_result.release_value());
        }

        // 3. Upon rejection of scriptPromise with value r, reject promise with value r.
        if (script_result.is_throw_completion()) {
            WebIDL::reject_promise(realm, promise, *script_result.throw_completion().value());
        }
    }));

    // 9. Wait until promise is resolved, or timer's timeout fired flag is set, whichever occurs first.
    auto reaction_steps = GC::create_function(vm.heap(), [promise, timer, on_complete](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        if (timer->is_timed_out())
            return JS::js_undefined();
        timer->stop();

        auto promise_promise = GC::Ref { as<JS::Promise>(*promise->promise()) };
        on_complete->function()({ promise_promise->state(), promise_promise->result() });

        return JS::js_undefined();
    });

    WebIDL::react_to_promise(promise, reaction_steps, reaction_steps);
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
    auto promise_capability = WebIDL::create_promise(realm);
    GC::Ref promise { as<JS::Promise>(*promise_capability->promise()) };

    // 8. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&vm, &realm, &browsing_context, timer, promise_capability, promise, body = move(body), arguments = move(arguments)]() mutable {
        HTML::TemporaryExecutionContext execution_context { realm };

        // 1. Let resolvingFunctions be CreateResolvingFunctions(promise).
        auto resolving_functions = promise->create_resolving_functions();

        // 2. Append resolvingFunctions.[[Resolve]] to arguments.
        arguments.append(resolving_functions.resolve);

        // 3. Let scriptResult be the result of calling execute a function body, with arguments body and arguments.
        auto script_result = execute_a_function_body(browsing_context, body, move(arguments));

        // 4. If scriptResult.[[Type]] is not normal, then reject promise with value scriptResult.[[Value]], and abort these steps.
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

        vm.custom_data()->spin_event_loop_until(GC::create_function(vm.heap(), [timer, &script_promise]() {
            return timer->is_timed_out() || script_promise.state() != JS::Promise::State::Pending;
        }));

        // 10. Upon fulfillment of scriptPromise with value v, resolve promise with value v.
        if (script_promise.state() == JS::Promise::State::Fulfilled)
            WebIDL::resolve_promise(realm, promise_capability, script_promise.result());

        // 11. Upon rejection of scriptPromise with value r, reject promise with value r.
        if (script_promise.state() == JS::Promise::State::Rejected)
            WebIDL::reject_promise(realm, promise_capability, script_promise.result());
    }));

    // 9. Wait until promise is resolved, or timer's timeout fired flag is set, whichever occurs first.
    auto reaction_steps = GC::create_function(vm.heap(), [promise, timer, on_complete](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        if (timer->is_timed_out())
            return JS::js_undefined();
        timer->stop();

        on_complete->function()({ promise->state(), promise->result() });
        return JS::js_undefined();
    });

    WebIDL::react_to_promise(promise_capability, reaction_steps, reaction_steps);
}

}
