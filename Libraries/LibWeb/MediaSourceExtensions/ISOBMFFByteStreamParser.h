/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibMedia/Containers/ISOBMFF/Boxes.h>
#include <LibMedia/Containers/ISOBMFF/FragmentSampleIterator.h>
#include <LibMedia/Track.h>
#include <LibWeb/MediaSourceExtensions/ByteStreamParser.h>

namespace Media::ISOBMFF {

class Streamer;

}

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/mse-byte-stream-format-isobmff/
class ISOBMFFByteStreamParser final : public ByteStreamParser {
public:
    ISOBMFFByteStreamParser();
    virtual ~ISOBMFFByteStreamParser() override;

    static bool supports_codec(StringView, Media::CodecID);

    virtual Media::DecoderErrorOr<void> skip_ignored_bytes(Media::MediaStreamCursor&) override;
    virtual Media::DecoderErrorOr<SegmentType> sniff_segment_type(Media::MediaStreamCursor&) override;
    virtual Media::DecoderErrorOr<void> parse_initialization_segment(Media::MediaStreamCursor&) override;
    virtual Media::DecoderErrorOr<ParseMediaSegmentResult> parse_media_segment(Media::MediaStreamCursor&) override;
    virtual void reset_parser_state() override;

    virtual Optional<AK::Duration> duration() const override { return m_duration; }

    virtual Media::CodecID codec_id_for_track(u64 track_id) const override
    {
        auto entry = m_track_entries.get(static_cast<u32>(track_id));
        if (!entry.has_value())
            return Media::CodecID::Unknown;
        auto sample_entry = (*entry)->default_sample_entry();
        return sample_entry.has_value() ? sample_entry->codec_id : Media::CodecID::Unknown;
    }

    virtual Vector<Media::Track> const& video_tracks() const override { return m_video_tracks; }
    virtual Vector<Media::Track> const& audio_tracks() const override { return m_audio_tracks; }
    virtual Vector<Media::Track> const& text_tracks() const override { return m_text_tracks; }

private:
    // A movie fragment describes all of its samples before any of their data follows, so the fragment is
    // parsed once and its samples are read across as many appends as it takes for their data to arrive.
    struct MediaSegment {
        Media::ISOBMFF::FragmentSampleIterator samples;
        // Media Data boxes are discovered as the samples they hold are reached.
        size_t next_media_data_header_position { 0 };
        size_t media_data_start { 0 };
        size_t media_data_end { 0 };
        bool has_media_data_box { false };
        // How far into the segment the cursor has reached. The input buffer is rebased as it is consumed,
        // so the segment is only ever moved through by relative distances.
        size_t position { 0 };
    };

    Media::DecoderErrorOr<MediaSegment> parse_movie_fragment(Media::ISOBMFF::Streamer&);

    // Holds a strong reference to the track entry to ensure that addresses are not reused to break comparison.
    struct ActiveSampleDescription {
        bool operator==(ActiveSampleDescription const&) const = default;

        RefPtr<Media::ISOBMFF::TrackEntry> track_entry;
        u32 index { 0 };
    };

    Optional<MediaSegment> m_current_media_segment;

    Optional<AK::Duration> m_duration;
    OrderedHashMap<u32, NonnullRefPtr<Media::ISOBMFF::TrackEntry>> m_track_entries;
    Media::ISOBMFF::TrackFragmentContexts m_track_fragment_contexts;
    HashMap<u32, ActiveSampleDescription> m_active_sample_descriptions;

    Vector<Media::Track> m_video_tracks;
    Vector<Media::Track> m_audio_tracks;
    Vector<Media::Track> m_text_tracks;
};

}
