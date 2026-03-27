/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibMedia/Containers/Matroska/Document.h>
#include <LibMedia/Containers/Matroska/Utilities.h>
#include <LibMedia/Track.h>
#include <LibWeb/MediaSourceExtensions/ByteStreamParser.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/mse-byte-stream-format-webm/
class WebMByteStreamParser final : public ByteStreamParser {
public:
    WebMByteStreamParser();
    virtual ~WebMByteStreamParser() override;

    virtual Media::DecoderErrorOr<void> skip_ignored_bytes(Media::MediaStreamCursor&) override;
    virtual Media::DecoderErrorOr<SegmentType> sniff_segment_type(Media::MediaStreamCursor&) override;
    virtual Media::DecoderErrorOr<void> parse_initialization_segment(Media::MediaStreamCursor&) override;
    virtual Media::DecoderErrorOr<ParseMediaSegmentResult> parse_media_segment(Media::MediaStreamCursor&) override;

    virtual Optional<AK::Duration> duration() const override
    {
        if (!m_segment_information.has_value())
            return {};
        auto duration = m_segment_information->duration();
        if (!duration.has_value())
            return {};
        return duration;
    }

    virtual Media::CodecID codec_id_for_track(u64 track_number) const override
    {
        auto entry = m_track_entries.get(track_number);
        if (!entry.has_value())
            return Media::CodecID::Unknown;
        return Media::Matroska::codec_id_from_matroska_id_string((*entry)->codec_id());
    }

    virtual ReadonlyBytes codec_initialization_data_for_track(u64 track_number) const override
    {
        auto entry = m_track_entries.get(track_number);
        if (!entry.has_value())
            return {};
        return (*entry)->codec_private_data();
    }

    virtual Vector<Media::Track> const& video_tracks() const override { return m_video_tracks; }
    virtual Vector<Media::Track> const& audio_tracks() const override { return m_audio_tracks; }
    virtual Vector<Media::Track> const& text_tracks() const override { return m_text_tracks; }

private:
    Optional<Media::Matroska::SegmentInformation> m_segment_information;
    OrderedHashMap<u64, NonnullRefPtr<Media::Matroska::TrackEntry>> m_track_entries;
    Media::Matroska::TrackBlockContexts m_track_block_contexts;

    Vector<Media::Track> m_video_tracks;
    Vector<Media::Track> m_audio_tracks;
    Vector<Media::Track> m_text_tracks;

    struct MediaSegmentParsingData {
        AK::Duration timecode;
        Optional<size_t> remaining_bytes;
        HashTable<u64> seen_track_numbers {};
        AK::Duration last_block_timestamp { AK::Duration::min() };
    };

    Optional<MediaSegmentParsingData> m_current_media_segment_data;
    bool m_cluster_has_been_read { false };
};

}
