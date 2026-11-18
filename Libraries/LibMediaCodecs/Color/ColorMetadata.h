/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibIPC/Forward.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#include <LibMediaCodecs/Export.h>

namespace Media {

struct Chromaticity {
    float x { 0 };
    float y { 0 };

    bool operator==(Chromaticity const&) const = default;
};

// SMPTE ST 2086 mastering display color volume, with luminances in cd/m².
struct MasteringDisplayColorVolume {
    Chromaticity red;
    Chromaticity green;
    Chromaticity blue;
    Chromaticity white_point;
    float maximum_luminance { 0 };
    float minimum_luminance { 0 };

    bool operator==(MasteringDisplayColorVolume const&) const = default;
};

// CTA-861.3 content light level information, in cd/m². A value of zero is unknown.
struct ContentLightLevelInformation {
    u16 maximum_content_light_level { 0 };
    u16 maximum_frame_average_light_level { 0 };

    bool operator==(ContentLightLevelInformation const&) const = default;
};

struct ColorMetadata {
    CodingIndependentCodePoints cicp;
    Optional<MasteringDisplayColorVolume> mastering_display_color_volume;
    Optional<ContentLightLevelInformation> content_light_level_information;
    // The brightest light in the frame's scene in cd/m².
    Optional<float> maximum_scene_luminance;

    bool operator==(ColorMetadata const&) const = default;

    void adopt_specified_values(ColorMetadata const& other)
    {
        cicp.adopt_specified_values(other.cicp);
        if (other.mastering_display_color_volume.has_value())
            mastering_display_color_volume = other.mastering_display_color_volume;
        if (auto const& information = other.content_light_level_information; information.has_value()) {
            auto merged_information = content_light_level_information.value_or({});
            if (information->maximum_content_light_level != 0)
                merged_information.maximum_content_light_level = information->maximum_content_light_level;
            if (information->maximum_frame_average_light_level != 0)
                merged_information.maximum_frame_average_light_level = information->maximum_frame_average_light_level;
            content_light_level_information = merged_information;
        }
        if (other.maximum_scene_luminance.has_value())
            maximum_scene_luminance = other.maximum_scene_luminance;
    }
};

}

namespace IPC {

template<>
MEDIACODECS_API ErrorOr<void> encode(Encoder&, Media::MasteringDisplayColorVolume const&);

template<>
MEDIACODECS_API ErrorOr<Media::MasteringDisplayColorVolume> decode(Decoder&);

template<>
MEDIACODECS_API ErrorOr<void> encode(Encoder&, Media::ContentLightLevelInformation const&);

template<>
MEDIACODECS_API ErrorOr<Media::ContentLightLevelInformation> decode(Decoder&);

template<>
MEDIACODECS_API ErrorOr<void> encode(Encoder&, Media::ColorMetadata const&);

template<>
MEDIACODECS_API ErrorOr<Media::ColorMetadata> decode(Decoder&);

}
