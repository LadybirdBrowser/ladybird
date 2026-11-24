/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/FFmpeg/VideoFramesAwaitingOutput.h>

namespace Media::FFmpeg {

// Frames that FFmpeg never output, such as ones it drops for corruption, stop being kept once this many later ones
// have gone in.
static constexpr u64 MAXIMUM_FRAMES_AWAITING_OUTPUT = 256;

DecoderErrorOr<u64> VideoFramesAwaitingOutput::add(CodedFrame const& coded_frame, DecodeIntent intent)
{
    auto key = m_next_key++;
    if (m_frames.size() >= MAXIMUM_FRAMES_AWAITING_OUTPUT) {
        auto stale_frame_count = m_frames.size();
        m_frames.remove_all_matching([&](u64 frame_key, auto const&) { return frame_key + MAXIMUM_FRAMES_AWAITING_OUTPUT <= key; });
        stale_frame_count -= m_frames.size();
        if (stale_frame_count > 0)
            dbgln("FFmpeg: {} frames were never output", stale_frame_count);
    }
    auto const& high_dynamic_range_metadata = m_high_dynamic_range_metadata_tracker.apply_coded_frame(coded_frame);
    DECODER_TRY_ALLOC(m_frames.try_set(key, { coded_frame.presentation_timestamp(), coded_frame.duration(), intent, high_dynamic_range_metadata }));
    return key;
}

Optional<VideoFramesAwaitingOutput::Frame> VideoFramesAwaitingOutput::take(i64 key)
{
    return m_frames.take(static_cast<u64>(key));
}

}
