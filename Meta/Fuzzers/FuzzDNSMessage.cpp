/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibDNS/Message.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    if (size > 65535)
        return 0;
    FixedMemoryStream stream(ReadonlyBytes { data, size });
    auto parsed = DNS::Messages::Message::from_raw(stream);
    if (parsed.is_error())
        return 0;
    ByteBuffer wire;
    auto encoded = parsed.value().to_raw(wire);
    // Some parsed RR types have no serializer. That is not an oracle failure.
    if (encoded.is_error())
        return 0;
    FixedMemoryStream second_stream(wire.bytes());
    auto second = DNS::Messages::Message::from_raw(second_stream);
    VERIFY(!second.is_error());
    VERIFY(parsed.value().header.id == second.value().header.id);
    VERIFY(parsed.value().questions.size() == second.value().questions.size());
    VERIFY(parsed.value().answers.size() == second.value().answers.size());
    VERIFY(parsed.value().authorities.size() == second.value().authorities.size());
    VERIFY(parsed.value().additional_records.size() == second.value().additional_records.size());
    // Compression pointers and wire lengths may change; do not compare raw packets.
    return 0;
}
