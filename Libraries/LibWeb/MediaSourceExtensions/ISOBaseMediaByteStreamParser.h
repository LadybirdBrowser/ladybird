/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibMedia/Track.h>
#include <LibWeb/MediaSourceExtensions/ByteStreamParser.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/mse-byte-stream-format-isobmff/
class ISOBaseMediaByteStreamParser final : public ByteStreamParser {
public:
    ISOBaseMediaByteStreamParser();
    virtual ~ISOBaseMediaByteStreamParser() override;

    virtual Media::DecoderErrorOr<void> skip_ignored_bytes(Media::MediaStreamCursor&) override;
    virtual Media::DecoderErrorOr<SegmentType> sniff_segment_type(Media::MediaStreamCursor&) override;
    virtual Media::DecoderErrorOr<void> parse_initialization_segment(Media::MediaStreamCursor&) override;
    virtual Media::DecoderErrorOr<ParseMediaSegmentResult> parse_media_segment(Media::MediaStreamCursor&) override;

    virtual Optional<AK::Duration> duration() const override { return m_duration; }
    virtual Media::CodecID codec_id_for_track(u64 track_number) const override;
    virtual ReadonlyBytes codec_initialization_data_for_track(u64 track_number) const override;

    virtual Vector<Media::Track> const& video_tracks() const override { return m_video_tracks; }
    virtual Vector<Media::Track> const& audio_tracks() const override { return m_audio_tracks; }
    virtual Vector<Media::Track> const& text_tracks() const override { return m_text_tracks; }

private:
    struct TrackDescription {
        Media::CodecID codec_id { Media::CodecID::Unknown };
        ByteBuffer codec_initialization_data;
    };

    ByteBuffer m_initialization_segment;
    Optional<AK::Duration> m_duration;

    HashMap<u64, TrackDescription> m_track_descriptions;
    Vector<Media::Track> m_video_tracks;
    Vector<Media::Track> m_audio_tracks;
    Vector<Media::Track> m_text_tracks;
};

}
