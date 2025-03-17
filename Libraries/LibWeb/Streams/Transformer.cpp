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
        .start = TRY(WebIDL::property_to_callback(vm, value, "start"_fly_string, WebIDL::OperationReturnsPromise::No)),
        .transform = TRY(WebIDL::property_to_callback(vm, value, "transform"_fly_string, WebIDL::OperationReturnsPromise::Yes)),
        .flush = TRY(WebIDL::property_to_callback(vm, value, "flush"_fly_string, WebIDL::OperationReturnsPromise::Yes)),
        .cancel = TRY(WebIDL::property_to_callback(vm, value, "cancel"_fly_string, WebIDL::OperationReturnsPromise::Yes)),
        .readable_type = {},
        .writable_type = {},
    };

    if (TRY(object.has_property("readableType"_fly_string)))
        transformer.readable_type = TRY(object.get("readableType"_fly_string));

    if (TRY(object.has_property("writableType"_fly_string)))
        transformer.writable_type = TRY(object.get("writableType"_fly_string));

    return transformer;
}

}
