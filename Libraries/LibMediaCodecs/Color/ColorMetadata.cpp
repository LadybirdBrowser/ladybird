/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMediaCodecs/Color/ColorMetadata.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::MasteringDisplayColorVolume const& volume)
{
    for (auto const& chromaticity : { volume.red, volume.green, volume.blue, volume.white_point }) {
        TRY(encoder.encode(chromaticity.x));
        TRY(encoder.encode(chromaticity.y));
    }
    TRY(encoder.encode(volume.maximum_luminance));
    TRY(encoder.encode(volume.minimum_luminance));
    return {};
}

template<>
ErrorOr<Media::MasteringDisplayColorVolume> decode(Decoder& decoder)
{
    Media::MasteringDisplayColorVolume volume;
    for (auto* chromaticity : { &volume.red, &volume.green, &volume.blue, &volume.white_point }) {
        chromaticity->x = TRY(decoder.decode<float>());
        chromaticity->y = TRY(decoder.decode<float>());
        if (!isfinite(chromaticity->x) || !isfinite(chromaticity->y))
            return Error::from_string_literal("IPC: Invalid mastering display chromaticity");
    }
    volume.maximum_luminance = TRY(decoder.decode<float>());
    volume.minimum_luminance = TRY(decoder.decode<float>());
    if (!isfinite(volume.maximum_luminance) || !isfinite(volume.minimum_luminance))
        return Error::from_string_literal("IPC: Invalid mastering display luminance");
    return volume;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Media::ContentLightLevelInformation const& information)
{
    TRY(encoder.encode(information.maximum_content_light_level));
    TRY(encoder.encode(information.maximum_frame_average_light_level));
    return {};
}

template<>
ErrorOr<Media::ContentLightLevelInformation> decode(Decoder& decoder)
{
    return Media::ContentLightLevelInformation {
        .maximum_content_light_level = TRY(decoder.decode<u16>()),
        .maximum_frame_average_light_level = TRY(decoder.decode<u16>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Media::ColorMetadata const& metadata)
{
    TRY(encoder.encode(metadata.cicp));
    TRY(encoder.encode(metadata.mastering_display_color_volume));
    TRY(encoder.encode(metadata.content_light_level_information));
    TRY(encoder.encode(metadata.maximum_scene_luminance));
    return {};
}

template<>
ErrorOr<Media::ColorMetadata> decode(Decoder& decoder)
{
    Media::ColorMetadata metadata {
        .cicp = TRY(decoder.decode<Media::CodingIndependentCodePoints>()),
        .mastering_display_color_volume = TRY(decoder.decode<Optional<Media::MasteringDisplayColorVolume>>()),
        .content_light_level_information = TRY(decoder.decode<Optional<Media::ContentLightLevelInformation>>()),
        .maximum_scene_luminance = TRY(decoder.decode<Optional<float>>()),
    };
    if (auto luminance = metadata.maximum_scene_luminance; luminance.has_value()) {
        if (!isfinite(*luminance))
            return Error::from_string_literal("IPC: Maximum scene luminance is not finite");
        if (*luminance <= 0)
            return Error::from_string_literal("IPC: Maximum scene luminance is not positive");
    }
    return metadata;
}

}
