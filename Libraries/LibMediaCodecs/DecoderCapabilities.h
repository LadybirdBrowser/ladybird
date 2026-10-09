/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <LibIPC/Forward.h>
#include <LibMediaCodecs/Export.h>

namespace Media {

struct DecoderCapabilities {
    bool smooth { false };
    bool power_efficient { false };

    bool operator==(DecoderCapabilities const&) const = default;
};

class ParsedCodec;

using DecoderCapabilitiesFunction = Optional<DecoderCapabilities> (*)(ParsedCodec const&);

// Asks the installed decoders for the codec's track type.
MEDIACODECS_API Optional<DecoderCapabilities> decoder_capabilities(ParsedCodec const&);

}

namespace IPC {

template<>
MEDIACODECS_API ErrorOr<void> encode(Encoder&, Media::DecoderCapabilities const&);

template<>
MEDIACODECS_API ErrorOr<Media::DecoderCapabilities> decode(Decoder&);

}
