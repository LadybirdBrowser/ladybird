/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/FFmpeg/FFmpegDemuxer.h>

namespace Media::FFmpeg {

DecoderErrorOr<Vector<Track>> FFmpegDemuxer::get_tracks_for_type(TrackType type)
{
    (void)type;
    return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg not available on this platform");
}

DecoderErrorOr<Optional<AK::Duration>> FFmpegDemuxer::seek_to_most_recent_keyframe(Track track, AK::Duration timestamp, Optional<AK::Duration> earliest_available_sample = OptionalNone())
{
    (void)track;
    (void)timestamp;
    (void)earliest_available_sample;
    return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg not available on this platform");
}

DecoderErrorOr<AK::Duration> FFmpegDemuxer::duration()
{
    return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg not available on this platform");
}

DecoderErrorOr<CodecID> FFmpegDemuxer::get_codec_id_for_track(Track track)
{
    (void)track;
    return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg not available on this platform");
}

DecoderErrorOr<ReadonlyBytes> FFmpegDemuxer::get_codec_initialization_data_for_track(Track track)
{
    (void)track;
    return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg not available on this platform");
}

DecoderErrorOr<Sample> FFmpegDemuxer::get_next_sample_for_track(Track track)
{
    (void)track;
    return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg not available on this platform");
}

}
