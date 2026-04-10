/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Console.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
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
            if (is<WebIDL::DOMException>(object)) {
                auto const& exception = static_cast<WebIDL::DOMException const&>(object);
                dbgln("\033[31;1mUnhandled JavaScript exception{}:\033[0m {}: {}", error_in_promise == ErrorInPromise::Yes ? " (in promise)" : "", exception.name(), exception.message());
            } else {
                dbgln("\033[31;1mUnhandled JavaScript exception{}:\033[0m {}", error_in_promise == ErrorInPromise::Yes ? " (in promise)" : "", JS::Value(&object));
            }
        } else {
            dbgln("\033[31;1mUnhandled JavaScript exception{}:\033[0m [{}] {}", error_in_promise == ErrorInPromise::Yes ? " (in promise)" : "", name, message);
        }
        if (auto const* error_data = object.error_data()) {
            String exception_name;
            String exception_message;
            if (auto const* exception = as_if<WebIDL::DOMException>(object)) {
                exception_name = exception->name().to_string();
                exception_message = MUST(exception->message().view().to_utf8());
            } else {
                exception_name = name.to_string_without_side_effects();
                exception_message = message.to_string_without_side_effects();
            }
            dbgln("{}", error_data->stack_string(JS::CompactTraceback::Yes));
            console.report_exception(exception_name, exception_message, *error_data, error_in_promise == ErrorInPromise::Yes);
            return;
        }
    } else {
        dbgln("\033[31;1mUnhandled JavaScript exception{}:\033[0m {}", error_in_promise == ErrorInPromise::Yes ? " (in promise)" : "", value);
    }

    auto message = value.to_string_without_side_effects();
    auto error = JS::Error::create(realm, Utf16String::from_utf8(message));
    console.report_exception("Error"_string, message, *error, error_in_promise == ErrorInPromise::Yes);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#report-the-exception
void report_exception(JS::Completion const& throw_completion, JS::Realm& realm)
{
    VERIFY(throw_completion.type() == JS::Completion::Type::Throw);
    report_exception_to_console(throw_completion.value(), realm, ErrorInPromise::No);
}

}
