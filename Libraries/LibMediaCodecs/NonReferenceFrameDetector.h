/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/Forward.h>

namespace Media {

// Recognizes the coded frames that no other frame is predicted from, which only need decoding to be shown. Frames
// carry the stream parameters this depends on, so every frame of a track must pass through it in decode order.
class MEDIACODECS_API NonReferenceFrameDetector {
public:
    bool frame_is_non_reference(CodedFrame const&);

private:
    void apply_codec_configuration(ReadonlyBytes);
    bool h264_frame_is_non_reference(ReadonlyBytes) const;
    bool h265_frame_is_non_reference(ReadonlyBytes);

    CodecID m_codec_id { CodecID::Unknown };
    // Empty while the frames cannot be split into NAL units, so that none of them is reported.
    Optional<u8> m_nal_unit_length_size;
    // Taken over every H.265 SPS since the last configuration, since overestimating it only leaves frames unreported.
    u8 m_highest_temporal_id { 0 };
};

}
