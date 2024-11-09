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
        auto name = object.get_without_side_effects(vm.names.name).value_or(JS::js_undefined());
        auto message = object.get_without_side_effects(vm.names.message).value_or(JS::js_undefined());
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
        if (is<JS::Error>(object)) {
            // FIXME: We should be doing this for DOMException as well
            //        https://webidl.spec.whatwg.org/#js-DOMException-specialness
            //        "Additionally, if an implementation gives native Error objects special powers or nonstandard properties (such as a stack property), it should also expose those on DOMException objects."
            auto const& error_value = static_cast<JS::Error const&>(object);
            dbgln("{}", error_value.stack_string(JS::CompactTraceback::Yes));
            console.report_exception(error_value, error_in_promise == ErrorInPromise::Yes);

            return;
        }
    } else {
        dbgln("\033[31;1mUnhandled JavaScript exception{}:\033[0m {}", error_in_promise == ErrorInPromise::Yes ? " (in promise)" : "", value);
    }

    console.report_exception(*JS::Error::create(realm, value.to_string_without_side_effects()), error_in_promise == ErrorInPromise::Yes);
}

// https://html.spec.whatwg.org/#report-the-exception
void report_exception(JS::Completion const& throw_completion, JS::Realm& realm)
{
    VERIFY(throw_completion.type() == JS::Completion::Type::Throw);
    VERIFY(throw_completion.value().has_value());
    report_exception_to_console(*throw_completion.value(), realm, ErrorInPromise::No);
}

}
