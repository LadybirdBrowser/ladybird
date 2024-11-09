/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/UnderlyingSink.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::Streams {

JS::ThrowCompletionOr<UnderlyingSink> UnderlyingSink::from_value(JS::VM& vm, JS::Value value)
{
    if (!value.is_object())
        return UnderlyingSink {};

    auto& object = value.as_object();

    UnderlyingSink underlying_sink {
        .start = TRY(property_to_callback(vm, value, "start", WebIDL::OperationReturnsPromise::No)),
        .write = TRY(property_to_callback(vm, value, "write", WebIDL::OperationReturnsPromise::Yes)),
        .close = TRY(property_to_callback(vm, value, "close", WebIDL::OperationReturnsPromise::Yes)),
        .abort = TRY(property_to_callback(vm, value, "abort", WebIDL::OperationReturnsPromise::Yes)),
        .type = {},
    };

    if (TRY(object.has_property("type")))
        underlying_sink.type = TRY(object.get("type"));

    return underlying_sink;
}

}
