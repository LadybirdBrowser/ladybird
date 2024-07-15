/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>

namespace JS {

class Uint8ArrayPrototypeHelpers {
public:
    static void initialize(Realm&, Object& prototype);

private:
    JS_DECLARE_NATIVE_FUNCTION(to_base64);
    JS_DECLARE_NATIVE_FUNCTION(to_hex);
};

enum class Alphabet {
    Base64,
    Base64URL,
};

ThrowCompletionOr<NonnullGCPtr<TypedArrayBase>> validate_uint8_array(VM&);
ThrowCompletionOr<ByteBuffer> get_uint8_array_bytes(VM&, TypedArrayBase const&);

}
