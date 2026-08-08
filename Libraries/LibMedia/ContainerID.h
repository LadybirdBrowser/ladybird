/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibMedia/Export.h>

namespace Media {

enum class ContainerID : u8 {
    ISOBMFF,
    Matroska,
    WebM,
    Ogg,
    MPEGAudio,
    ADTS,
    FLAC,
    WAV,
};

enum class ContainerMediaType : u8 {
    Audio,
    Video,
    Application,
};

struct ContainerMimeType {
    ContainerID container_id;
    ContainerMediaType media_type;

    bool operator==(ContainerMimeType const&) const = default;
};

MEDIA_API Optional<ContainerMimeType> container_mime_type_from_mime_type(StringView type, StringView subtype);

}
