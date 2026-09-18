/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include <AK/Array.h>
#include <AK/MemoryStream.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

template<typename T>
static void decode_and_reencode(ReadonlyBytes bytes)
{
    FixedMemoryStream stream(bytes);
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder(stream, attachments);
    auto decoded = decoder.decode<T>();
    if (decoded.is_error())
        return;
    IPC::MessageBuffer encoded;
    IPC::Encoder encoder(encoded);
    // Do not require byte equality: accepted noncanonical encodings may normalize.
    if (encoder.encode(decoded.value()).is_error())
        return;
    FixedMemoryStream second_stream(encoded.data().span());
    IPC::Decoder second(second_stream, attachments);
    auto roundtrip = second.decode<T>();
    VERIFY(!roundtrip.is_error());
    VERIFY(second_stream.is_eof());
    VERIFY(attachments.is_empty());
}

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    if (size < 1 || size > 65536)
        return 0;
    Fuzzing::BoundedInput input({ data, size });
    auto kind = input.byte() % 9;
    switch (kind) {
    case 0:
        decode_and_reencode<bool>(input.remaining());
        break;
    case 1:
        decode_and_reencode<String>(input.remaining());
        break;
    case 2:
        decode_and_reencode<Utf16String>(input.remaining());
        break;
    case 3:
        decode_and_reencode<Optional<ByteString>>(input.remaining());
        break;
    case 4:
        decode_and_reencode<Variant<u32, ByteString, Optional<u64>>>(input.remaining());
        break;
    case 5:
        decode_and_reencode<Vector<u8>>(input.remaining());
        break;
    case 6:
        decode_and_reencode<Array<u32, 4>>(input.remaining());
        break;
    case 7:
        decode_and_reencode<URL::URL>(input.remaining());
        break;
    case 8:
        decode_and_reencode<URL::Origin>(input.remaining());
        break;
    }
    return 0;
}
