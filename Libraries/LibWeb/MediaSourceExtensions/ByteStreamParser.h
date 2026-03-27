/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Types.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Forward.h>

namespace Web::MediaSourceExtensions {

enum class SegmentType : u8 {
    Incomplete,
    Unknown,
    InitializationSegment,
    MediaSegment,
};

struct DemuxedCodedFrame {
    u64 track_number { 0 };
    Media::CodedFrame coded_frame;
};

struct ParseMediaSegmentResult {
    bool completed_segment { false };
    Vector<DemuxedCodedFrame> coded_frames;
};

// https://w3c.github.io/media-source/#sourcebuffer-segment-parser-loop
// ByteStreamParser abstracts the format-specific parsing that the segment parser loop requires.
// All methods operate on a MediaStreamCursor which which will be used to determine what the last read data was when
// the loop needs to remove data from the input buffer.
class ByteStreamParser {
public:
    virtual ~ByteStreamParser() = default;

    virtual Media::DecoderErrorOr<void> skip_ignored_bytes(Media::MediaStreamCursor&) = 0;
    virtual Media::DecoderErrorOr<SegmentType> sniff_segment_type(Media::MediaStreamCursor&) = 0;
    virtual Media::DecoderErrorOr<void> parse_initialization_segment(Media::MediaStreamCursor&) = 0;
    virtual Media::DecoderErrorOr<ParseMediaSegmentResult> parse_media_segment(Media::MediaStreamCursor&) = 0;

    virtual Optional<AK::Duration> duration() const = 0;
    virtual Media::CodecID codec_id_for_track(u64 track_number) const = 0;
    virtual ReadonlyBytes codec_initialization_data_for_track(u64 track_number) const = 0;

    virtual Vector<Media::Track> const& video_tracks() const = 0;
    virtual Vector<Media::Track> const& audio_tracks() const = 0;
    virtual Vector<Media::Track> const& text_tracks() const = 0;
};

}
