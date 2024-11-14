/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class Uint8ArrayConstructorHelpers {
public:
    static void initialize(Realm&, Object& constructor);

private:
    JS_DECLARE_NATIVE_FUNCTION(from_base64);
    JS_DECLARE_NATIVE_FUNCTION(from_hex);
};

class Uint8ArrayPrototypeHelpers {
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

enum class LastChunkHandling {
    Loose,
    Strict,
    StopBeforePartial,
};

struct DecodeResult {
    size_t read { 0 };          // [[Read]]
    ByteBuffer bytes;           // [[Bytes]]
    Optional<Completion> error; // [[Error]]
};

ThrowCompletionOr<GC::Ref<TypedArrayBase>> validate_uint8_array(VM&);
ThrowCompletionOr<ByteBuffer> get_uint8_array_bytes(VM&, TypedArrayBase const&);
void set_uint8_array_bytes(TypedArrayBase&, ReadonlyBytes);
DecodeResult from_base64(VM&, StringView string, Alphabet alphabet, LastChunkHandling last_chunk_handling, Optional<size_t> max_length = {});
DecodeResult from_hex(VM&, StringView string, Optional<size_t> max_length = {});

}
