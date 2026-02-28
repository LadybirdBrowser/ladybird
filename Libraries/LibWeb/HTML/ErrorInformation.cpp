/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/ErrorInformation.h>

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
    attributes.message = [&] {
        if (auto object = exception.as_if<JS::Object>()) {
            if (MUST(object->has_own_property(vm.names.message))) {
                auto message = object->get_without_side_effects(vm.names.message);
                return message.to_string_without_side_effects();
            }
        }

        return MUST(String::formatted("Uncaught exception: {}", exception));
    }();

    // FIXME: This offset is relative to the javascript source. Other browsers appear to do it relative
    //        to the entire source document! Calculate that somehow.

    // NB: If we got an Error object, then try and extract the information from the location the object was made.
    if (auto error = exception.as_if<JS::Error>()) {
        for (auto const& frame : error->traceback()) {
            auto source_range = frame.source_range();
            if (source_range.start.line != 0 || source_range.start.column != 0) {
                attributes.filename = MUST(String::from_byte_string(source_range.filename()));
                attributes.lineno = source_range.start.line;
                attributes.colno = source_range.start.column;
                break;
            }
        }
    }
    // NB: Otherwise, we fall back to try and find the location of the invocation of the function itself.
    else {
        for (ssize_t i = vm.execution_context_stack().size() - 1; i >= 0; --i) {
            auto& frame = vm.execution_context_stack()[i];
            if (frame->executable) {
                auto source_range = frame->executable->source_range_at(frame->program_counter).realize();
                attributes.filename = MUST(String::from_byte_string(source_range.filename()));
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
