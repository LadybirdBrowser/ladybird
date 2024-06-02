/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/Transformer.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::Streams {

JS::ThrowCompletionOr<Transformer> Transformer::from_value(JS::VM& vm, JS::Value value)
{
    if (!value.is_object())
        return Transformer {};

    auto& object = value.as_object();

    Transformer transformer {
        .start = TRY(property_to_callback(vm, value, "start", WebIDL::OperationReturnsPromise::No)),
        .transform = TRY(property_to_callback(vm, value, "transform", WebIDL::OperationReturnsPromise::Yes)),
        .flush = TRY(property_to_callback(vm, value, "flush", WebIDL::OperationReturnsPromise::Yes)),
        .cancel = TRY(property_to_callback(vm, value, "cancel", WebIDL::OperationReturnsPromise::Yes)),
        .readable_type = {},
        .writable_type = {},
    };

    if (TRY(object.has_property("readableType")))
        transformer.readable_type = TRY(object.get("readableType"));

    if (TRY(object.has_property("writableType")))
        transformer.writable_type = TRY(object.get("writableType"));

    return transformer;
}

}
