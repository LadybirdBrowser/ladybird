/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/ErrorInformation.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#extract-error
ErrorInformation extract_error_information(JS::VM& vm, JS::Value exception)
{
    // 1. Let attributes be an empty map keyed by IDL attributes.
    ErrorInformation attributes;

    // 2. Set attributes[error] to exception.
    attributes.error = exception;

    // 3. Set attributes[message], attributes[filename], attributes[lineno], and attributes[colno] to
    //    implementation-defined values derived from exception.
    // NB: An Error's or a DOMException's message is prefixed with its name, as Error.prototype.toString() does.
    auto prefix_with_name = [](Utf16String name, Utf16String message) {
        if (message.is_empty())
            return name;
        if (name.is_empty())
            return message;
        return Utf16String::formatted("{}: {}", name, message);
    };

    attributes.message = [&] {
        if (auto object = exception.as_if<JS::Object>()) {
            // NB: A DOMException's name and message are accessors on its prototype.
            if (auto const* dom_exception = Bindings::dom_exception_from_object(*object))
                return prefix_with_name(dom_exception->name().to_utf16_string(), dom_exception->message().to_utf16_string());

            if (MUST(object->has_own_property(vm.names.message))) {
                auto message = object->get_without_side_effects(vm.names.message).to_utf16_string_without_side_effects();
                if (is<JS::Error>(*object)) {
                    auto name = object->get_without_side_effects(vm.names.name);
                    if (name.is_string())
                        return prefix_with_name(name.to_utf16_string_without_side_effects(), move(message));
                }
                return message;
            }
        }

        return Utf16String::formatted("Uncaught exception: {}", exception);
    }();

    // FIXME: This offset is relative to the javascript source. Other browsers appear to do it relative
    //        to the entire source document! Calculate that somehow.

    // NB: If we got an Error object, then try and extract the information from the location the object was made.
    if (auto object = exception.as_if<JS::Object>(); object && object->error_data()) {
        for (auto const& frame : object->error_data()->traceback()) {
            auto source_range = frame.source_range();
            if (source_range.start.line != 0 || source_range.start.column != 0) {
                attributes.filename = source_range.filename();
                attributes.lineno = source_range.start.line;
                attributes.colno = source_range.start.column;
                break;
            }
        }
    }
    // NB: Otherwise, we fall back to try and find the location of the invocation of the function itself.
    else {
        for (auto const& frame : vm.stack_trace()) {
            if (frame.source_range.has_value()) {
                auto const& source_range = *frame.source_range;
                attributes.filename = source_range.filename();
                attributes.lineno = source_range.start.line;
                attributes.colno = source_range.start.column;
                break;
            }
        }
    }

    // 4. Return attributes.
    return attributes;
}

}
