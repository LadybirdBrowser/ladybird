/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class SharedArrayBufferPrototype final : public PrototypeObject<SharedArrayBufferPrototype, ArrayBuffer> {
    JS_PROTOTYPE_OBJECT(SharedArrayBufferPrototype, ArrayBuffer, SharedArrayBuffer);
    JS_DECLARE_ALLOCATOR(SharedArrayBufferPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~SharedArrayBufferPrototype() override = default;

private:
    explicit SharedArrayBufferPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(byte_length_getter);
    JS_DECLARE_NATIVE_FUNCTION(slice);
};

}
