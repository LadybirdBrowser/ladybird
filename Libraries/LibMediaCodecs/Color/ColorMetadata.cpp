/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMediaCodecs/Color/ColorMetadata.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::ColorMetadata const& metadata)
{
    TRY(encoder.encode(metadata.cicp));
    return {};
}

template<>
ErrorOr<Media::ColorMetadata> decode(Decoder& decoder)
{
    return Media::ColorMetadata {
        .cicp = TRY(decoder.decode<Media::CodingIndependentCodePoints>()),
    };
}

}
