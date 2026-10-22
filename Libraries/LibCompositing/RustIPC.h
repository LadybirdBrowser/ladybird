/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/StdLibExtras.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

// The C++ half of the Rust codecs in Rust/src/ipc.rs: the IPC codec of a type whose values Rust makes and reads hands
// its encoder or decoder to Rust, which writes or reads the type's wire format in place.

namespace Compositing {

// Has `encode_in_rust(void* encoder, append)` encode a value into the message `encoder` builds, appending to it with
// `append`, and answer whether the value encoded.
template<typename EncodeInRust>
ErrorOr<void> encode_with_rust(IPC::Encoder& encoder, EncodeInRust encode_in_rust)
{
    auto append = [](void* encoder, u8 const* bytes, size_t length) {
        // NB: Appending to a message fails only when memory runs out.
        MUST(static_cast<IPC::Encoder*>(encoder)->append(bytes, length));
    };
    if (!encode_in_rust(static_cast<void*>(&encoder), +append))
        return Error::from_string_literal("A Rust IPC codec refused a value");
    return {};
}

// Has `decode_in_rust(u8 const* bytes, size_t length, size_t& consumed)` decode a value from the start of the rest of
// the message `decoder` reads, and moves `decoder` past the bytes it took. `decode_in_rust` returns a handle to the
// value, or null where the bytes don't start with one.
template<typename DecodeInRust>
auto decode_with_rust(IPC::Decoder& decoder, DecodeInRust decode_in_rust) -> ErrorOr<decltype(decode_in_rust(nullptr, 0, declval<size_t&>()))>
{
    auto& stream = decoder.stream();
    auto start = stream.offset();
    auto bytes = TRY(stream.read_in_place<u8 const>(stream.remaining()));
    size_t consumed = 0;
    auto handle = decode_in_rust(bytes.data(), bytes.size(), consumed);
    if (!handle)
        return Error::from_string_literal("A Rust IPC codec refused the bytes of a message");
    VERIFY(consumed <= bytes.size());
    TRY(stream.seek(static_cast<i64>(start + consumed)));
    return handle;
}

}
