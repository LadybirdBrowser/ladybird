/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Base64.h>
#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class JS_API Uint8ArrayConstructorHelpers {
public:
    static void initialize(Realm&, Object& constructor);

private:
    JS_DECLARE_NATIVE_FUNCTION(from_base64);
    JS_DECLARE_NATIVE_FUNCTION(from_hex);
};

class JS_API Uint8ArrayPrototypeHelpers {
public:
    static void initialize(Realm&, Object& prototype);

private:
    JS_DECLARE_NATIVE_FUNCTION(to_base64);
    JS_DECLARE_NATIVE_FUNCTION(to_hex);
    JS_DECLARE_NATIVE_FUNCTION(set_from_base64);
    JS_DECLARE_NATIVE_FUNCTION(set_from_hex);
};

enum class Alphabet {
    Base64,
    Base64URL,
};

struct DecodeResult {
    size_t read { 0 };          // [[Read]]
    ByteBuffer bytes;           // [[Bytes]]
    Optional<Completion> error; // [[Error]]
};

JS_API ThrowCompletionOr<GC::Ref<TypedArrayBase>> validate_uint8_array(VM&);
JS_API ThrowCompletionOr<ByteBuffer> get_uint8_array_bytes(VM&, TypedArrayBase const&);
JS_API void set_uint8_array_bytes(TypedArrayBase&, ReadonlyBytes);
JS_API DecodeResult from_base64(VM&, StringView string, Alphabet alphabet, AK::LastChunkHandling last_chunk_handling, Optional<size_t> max_length = {});
JS_API DecodeResult from_hex(VM&, StringView string, Optional<size_t> max_length = {});

}
