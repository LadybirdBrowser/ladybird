/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/MediaTime.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::MediaTimeReader const& reader)
{
    return encoder.encode(reader.buffer());
}

template<>
ErrorOr<Media::MediaTimeReader> decode(Decoder& decoder)
{
    auto buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    return Media::MediaTimeReader::create(move(buffer));
}

}
