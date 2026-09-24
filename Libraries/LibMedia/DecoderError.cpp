/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/DecoderError.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::DecoderError const& error)
{
    TRY(encoder.encode(error.category()));
    TRY(encoder.encode(ByteString { error.description() }));
    return {};
}

template<>
ErrorOr<Media::DecoderError> decode(Decoder& decoder)
{
    auto category = TRY(decoder.decode<Media::DecoderErrorCategory>());
    if (category > Media::DecoderErrorCategory::UnrecognizedFormat)
        return Error::from_string_literal("IPC: Invalid decoder error category");
    auto description = TRY(decoder.decode<ByteString>());
    return Media::DecoderError::with_description(category, move(description));
}

}
