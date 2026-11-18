/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/Color/ColorMetadata.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/Forward.h>

namespace Media::Codecs {

// Follows the HDR metadata a video bitstream carries alongside its pictures. The scene peak is each frame's own, while
// the rest carries over to the frames that follow until the bitstream changes it or a new configuration starts over.
class MEDIACODECS_API HighDynamicRangeMetadataTracker {
public:
    HighDynamicRangeMetadataTracker(CodecID, ReadonlyBytes configuration);

    // Takes the frame's new configuration if it has one, then gives back the metadata of the frame.
    ColorMetadata const& apply_coded_frame(CodedFrame const&);

    // For a decoder that walks the NAL units of H.264 and H.265 frames itself. A new configuration calls for
    // forget_stream(), and each frame for begin_frame() before its NAL units are applied. The frame starts from the
    // scene peak its container gave it, which the bitstream may replace.
    void forget_stream();
    void begin_frame(CodedFrame const&);
    void apply_nal_unit(ReadonlyBytes);

    ColorMetadata const& metadata() const { return m_metadata; }

private:
    void apply_configuration(ReadonlyBytes);
    void take_bitstream_metadata(ColorMetadata const&);

    CodecID m_codec_id;
    u8 m_nal_unit_length_size { 4 };
    ColorMetadata m_metadata;
    // A frame that starts a coded video sequence keeps only the values its own SEI messages gave.
    bool m_frame_gave_mastering_display_color_volume { false };
    bool m_frame_gave_content_light_level_information { false };
    bool m_next_picture_begins_coded_video_sequence { false };
};

}
