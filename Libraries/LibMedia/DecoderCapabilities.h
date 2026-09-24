/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Export.h>

namespace Media {

struct DecoderCapabilities {
    bool smooth { false };
    bool power_efficient { false };

    bool operator==(DecoderCapabilities const&) const = default;
};

}

namespace IPC {

template<>
MEDIA_API ErrorOr<void> encode(Encoder&, Media::DecoderCapabilities const&);

template<>
MEDIA_API ErrorOr<Media::DecoderCapabilities> decode(Decoder&);

}
