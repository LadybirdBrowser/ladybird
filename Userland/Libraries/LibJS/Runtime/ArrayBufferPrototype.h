/*
 * Copyright (c) 2020, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class ArrayBufferPrototype final : public PrototypeObject<ArrayBufferPrototype, ArrayBuffer> {
    JS_PROTOTYPE_OBJECT(ArrayBufferPrototype, ArrayBuffer, ArrayBuffer);
    JS_DECLARE_ALLOCATOR(ArrayBufferPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~ArrayBufferPrototype() override = default;

private:
    explicit ArrayBufferPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(byte_length_getter);
    JS_DECLARE_NATIVE_FUNCTION(detached_getter);
    JS_DECLARE_NATIVE_FUNCTION(max_byte_length);
    JS_DECLARE_NATIVE_FUNCTION(resizable);
    JS_DECLARE_NATIVE_FUNCTION(resize);
    JS_DECLARE_NATIVE_FUNCTION(slice);
    JS_DECLARE_NATIVE_FUNCTION(transfer);
    JS_DECLARE_NATIVE_FUNCTION(transfer_to_fixed_length);
};

}
