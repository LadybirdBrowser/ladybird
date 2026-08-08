/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibMedia/ContainerID.h>

namespace Media {

Optional<ContainerMimeType> container_mime_type_from_mime_type(StringView type, StringView subtype)
{
    auto media_type = [&]() -> Optional<ContainerMediaType> {
        if (type == "audio"sv)
            return ContainerMediaType::Audio;
        if (type == "video"sv)
            return ContainerMediaType::Video;
        if (type == "application"sv)
            return ContainerMediaType::Application;
        return {};
    }();
    if (!media_type.has_value())
        return {};

    if (subtype == "mp4"sv)
        return ContainerMimeType { ContainerID::ISOBMFF, *media_type };

    if (subtype == "webm"sv) {
        if (*media_type == ContainerMediaType::Application)
            return {};
        return ContainerMimeType { ContainerID::WebM, *media_type };
    }

    if (first_is_one_of(subtype, "matroska"sv, "x-matroska"sv)) {
        if (*media_type == ContainerMediaType::Application)
            return {};
        return ContainerMimeType { ContainerID::Matroska, *media_type };
    }

    if (subtype == "ogg"sv)
        return ContainerMimeType { ContainerID::Ogg, *media_type };

    if (*media_type != ContainerMediaType::Audio)
        return {};

    if (first_is_one_of(subtype, "mpeg"sv, "mp3"sv, "x-mp3"sv))
        return ContainerMimeType { ContainerID::MPEGAudio, *media_type };
    if (subtype == "aac"sv)
        return ContainerMimeType { ContainerID::ADTS, *media_type };
    if (subtype == "flac"sv)
        return ContainerMimeType { ContainerID::FLAC, *media_type };
    if (first_is_one_of(subtype, "wav"sv, "wave"sv, "x-wav"sv))
        return ContainerMimeType { ContainerID::WAV, *media_type };

    return {};
}

}
