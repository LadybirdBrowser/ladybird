/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace HTTP {

struct Header {
    ByteString name;
    ByteString value;
};

}

namespace IPC {

template<>
inline ErrorOr<void> encode(Encoder& encoder, HTTP::Header const& header)
{
    TRY(encoder.encode(header.name));
    TRY(encoder.encode(header.value));
    return {};
}

template<>
inline ErrorOr<HTTP::Header> decode(Decoder& decoder)
{
    auto name = TRY(decoder.decode<ByteString>());
    auto value = TRY(decoder.decode<ByteString>());
    return HTTP::Header { move(name), move(value) };
}

}
