/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Forward.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#include <LibMediaCodecs/Export.h>

namespace Media {

struct ColorMetadata {
    CodingIndependentCodePoints cicp;

    bool operator==(ColorMetadata const&) const = default;

    void adopt_specified_values(ColorMetadata const& other)
    {
        cicp.adopt_specified_values(other.cicp);
    }
};

}

namespace IPC {

template<>
MEDIACODECS_API ErrorOr<void> encode(Encoder&, Media::ColorMetadata const&);

template<>
MEDIACODECS_API ErrorOr<Media::ColorMetadata> decode(Decoder&);

}
