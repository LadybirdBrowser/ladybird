/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Console.h>
#include <LibJS/Debugger.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/HTML/DedicatedWorkerGlobalScope.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/ErrorInformation.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

void report_exception_to_console(JS::Value value, JS::Realm& realm, ErrorInPromise error_in_promise)
{
    auto& console = realm.intrinsics().console_object()->console();

    if (value.is_object()) {
        auto& object = value.as_object();
        auto& vm = object.vm();
        auto name = object.get_without_side_effects(vm.names.name);
        auto message = object.get_without_side_effects(vm.names.message);
        if (name.is_accessor() || message.is_accessor()) {
            // The result is not going to be useful, let's just print the value. This affects DOMExceptions, for example.
            if (auto exception = Bindings::dom_exception_report_details(object); exception.has_value()) {
                dbgln("\033[31;1mUnhandled JavaScript exception{}:\033[0m {}: {}", error_in_promise == ErrorInPromise::Yes ? " (in promise)" : "", exception->name, exception->message);
            } else {
                dbgln("\033[31;1mUnhandled JavaScript exception{}:\033[0m {}", error_in_promise == ErrorInPromise::Yes ? " (in promise)" : "", JS::Value(&object));
            }
        } else {
            dbgln("\033[31;1mUnhandled JavaScript exception{}:\033[0m [{}] {}", error_in_promise == ErrorInPromise::Yes ? " (in promise)" : "", name, message);
        }
        if (auto const* error_data = object.error_data()) {
            Utf16String exception_name;
            Utf16String exception_message;
            if (auto exception = Bindings::dom_exception_report_details(object); exception.has_value()) {
                exception_name = Utf16String::from_utf8(exception->name.bytes());
                exception_message = Utf16String::from_utf8(exception->message.bytes());
            } else {
                exception_name = name.to_utf16_string_without_side_effects();
                exception_message = message.to_utf16_string_without_side_effects();
            }
            dbgln("{}", error_data->stack_string(JS::CompactTraceback::Yes));
            console.report_exception(exception_name, exception_message, *error_data, error_in_promise == ErrorInPromise::Yes);
            return;
        }
    } else {
        dbgln("\033[31;1mUnhandled JavaScript exception{}:\033[0m {}", error_in_promise == ErrorInPromise::Yes ? " (in promise)" : "", value);
    }

    auto utf16_message = value.to_utf16_string_without_side_effects();
    auto error = JS::Error::create(realm, utf16_message);
    console.report_exception("Error"_utf16, utf16_message, *error, error_in_promise == ErrorInPromise::Yes);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#report-an-exception
void report_an_exception(JS::Object& global, JS::Value exception, OmitError omit_error, GC::Ptr<Script> script)
{
    auto& realm = relevant_realm(global);
    auto& vm = realm.vm();

    // 1. Let notHandled be true.
    bool not_handled = true;

    // 2. Let errorInfo be the result of extracting error information from exception.
    auto error_info = extract_error_information(vm, exception);

    // 3. Let script be a script found in an implementation-defined way, or null. This should usually be the
    //    running script (most notably during run a classic script).
    // NB: The running script's execution context is gone by the time its error gets reported, so callers that know
    //     the script pass it in.
    if (!script) {
        vm.get_active_script_or_module().visit(
            [&](GC::Ref<JS::Script> const& js_script) {
                script = script_from_host_defined(js_script->host_defined());
            },
            [&](GC::Ref<JS::Module> const& module) {
                script = script_from_host_defined(module->host_defined());
            },
            [](auto const&) { });
    }

    // NB: A parse error is created before its script runs, so take its location from the script.
    if (script && !script->parse_error().is_null() && script->parse_error() == exception) {
        error_info.filename = Utf16String::from_utf8(script->filename());
        if (auto const& position = script->parse_error_position(); position.has_value()) {
            error_info.lineno = position->line;
            error_info.colno = position->column;
        }
    }

    // 4. If script is a classic script and script's muted errors is true, then set errorInfo[error] to null,
    //    errorInfo[message] to "Script error.", errorInfo[filename] to the empty string, errorInfo[lineno] to
    //    0, and errorInfo[colno] to 0.
    if (auto* classic_script = as_if<ClassicScript>(script.ptr()); classic_script && classic_script->muted_errors() == ClassicScript::MutedErrors::Yes) {
        error_info.error = JS::js_null();
        error_info.message = "Script error."_utf16;
        error_info.filename = Utf16String {};
        error_info.lineno = 0;
        error_info.colno = 0;
    }

    // 5. If omitError is true, then set errorInfo[error] to null.
    if (omit_error == OmitError::Yes)
        error_info.error = JS::js_null();

    // 6. If global is not in error reporting mode, then:
    // NB: WorkletGlobalScope's IDL interface does not inherit from EventTarget, so nothing can observe its in error
    //     reporting mode and only Window and WorkerGlobalScope keep one.
    auto* window_or_worker = window_or_worker_global_scope_from_global_object(global);
    if (window_or_worker && !window_or_worker->is_in_error_reporting_mode()) {
        // 1. Set global's in error reporting mode to true.
        window_or_worker->set_in_error_reporting_mode(true);

        // 2. If global implements EventTarget, then set notHandled to the result of firing an event named
        //    error at global, using ErrorEvent, with the cancelable attribute initialized to true, and
        //    additional attributes initialized according to errorInfo.
        ErrorEventInit event_init = {};
        event_init.cancelable = true;
        event_init.message = error_info.message;
        event_init.filename = error_info.filename;
        event_init.lineno = error_info.lineno;
        event_init.colno = error_info.colno;
        event_init.error = error_info.error;

        not_handled = window_or_worker->this_impl().dispatch_event(ErrorEvent::create(EventNames::error, event_init, HighResolutionTime::current_high_resolution_time(global)));

        // 3. Set global's in error reporting mode to false.
        window_or_worker->set_in_error_reporting_mode(false);
    }

    // 7. If notHandled is true, then:
    if (not_handled) {
        // 1. Set errorInfo[error] to null.
        error_info.error = JS::js_null();

        // 2. If global implements DedicatedWorkerGlobalScope, queue a global task on the DOM manipulation
        //    task source with the global's associated Worker's relevant global object to run these steps:
        if (auto* dedicated_worker_global_scope = Bindings::impl_from<DedicatedWorkerGlobalScope>(&global)) {
            if (auto* page = dedicated_worker_global_scope->page()) {
                page->client().page_did_report_worker_exception(
                    error_info.message,
                    error_info.filename,
                    error_info.lineno,
                    error_info.colno);
            }
        }
        // 3. Otherwise, the user agent may report exception to a developer console.
        else {
            report_exception_to_console(exception, realm, ErrorInPromise::No);
        }
    }
}

// https://html.spec.whatwg.org/multipage/webappapis.html#report-the-exception
void report_exception(JS::Completion const& throw_completion, JS::Realm& realm)
{
    VERIFY(throw_completion.type() == JS::Completion::Type::Throw);
    report_exception_to_console(throw_completion.value(), realm, ErrorInPromise::No);
    if (auto* debugger = realm.vm().debugger())
        debugger->did_finish_exception_propagation(throw_completion.value());
}

}
