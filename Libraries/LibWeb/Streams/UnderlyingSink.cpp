/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Streams/UnderlyingSink.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::Streams {

JS::ThrowCompletionOr<UnderlyingSink> UnderlyingSink::from_value(JS::VM& vm, JS::Value value)
{
    if (!value.is_object())
        return UnderlyingSink {};

    auto& object = value.as_object();

    UnderlyingSink underlying_sink {
        .start = TRY(WebIDL::property_to_callback(vm, value, "start"_utf16_fly_string, WebIDL::OperationReturnsPromise::No)),
        .write = TRY(WebIDL::property_to_callback(vm, value, "write"_utf16_fly_string, WebIDL::OperationReturnsPromise::Yes)),
        .close = TRY(WebIDL::property_to_callback(vm, value, "close"_utf16_fly_string, WebIDL::OperationReturnsPromise::Yes)),
        .abort = TRY(WebIDL::property_to_callback(vm, value, "abort"_utf16_fly_string, WebIDL::OperationReturnsPromise::Yes)),
        .type = {},
    };

    if (TRY(object.has_property("type"_utf16_fly_string)))
        underlying_sink.type = TRY(object.get("type"_utf16_fly_string));

    return underlying_sink;
}

}
