/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Export.h>
#include <LibMedia/FFmpeg/FFmpegForward.h>
#include <LibMedia/FFmpeg/FFmpegIOContext.h>

namespace Media::FFmpeg {

class MEDIA_API FFmpegDemuxer : public Demuxer {
public:
    static DecoderErrorOr<NonnullRefPtr<FFmpegDemuxer>> from_stream(NonnullRefPtr<MediaStream> const&);

    virtual ~FFmpegDemuxer() override;

    virtual DecoderErrorOr<void> create_context_for_track(Track const&) override;

    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;

    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions) override;

    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;

    virtual DecoderErrorOr<CodecID> get_codec_id_for_track(Track const&) override;

    virtual DecoderErrorOr<ReadonlyBytes> get_codec_initialization_data_for_track(Track const&) override;

    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;

    virtual void set_blocking_reads_aborted_for_track(Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) override;
    virtual bool is_read_blocked_for_track(Track const&) override;

private:
    struct StreamInfo {
        Track track;
        CodecID codec_id;
        ByteBuffer codec_initialization_data;
        AK::Duration duration;
        i32 time_base_numerator;
        i32 time_base_denominator;
    };

    struct TrackContext {
        TrackContext(NonnullRefPtr<MediaStreamCursor>&& cursor, NonnullOwnPtr<FFmpegIOContext>&& io_context)
            : cursor(move(cursor))
            , io_context(move(io_context))
        {
        }
        ~TrackContext();
        TrackContext(TrackContext&&) = default;

        NonnullRefPtr<MediaStreamCursor> cursor;
        NonnullOwnPtr<FFmpegIOContext> io_context;
        AVFormatContext* format_context { nullptr };
        AVPacket* packet { nullptr };
        bool is_seekable { true };
        bool peeked_packet_already { false };
    };

    FFmpegDemuxer(NonnullRefPtr<MediaStream> const&);

    StreamInfo const& get_track_info(Track const&) const;
    TrackContext& get_track_context(Track const&);

    NonnullRefPtr<MediaStream> m_stream;
    AK::Duration m_total_duration;
    Vector<StreamInfo> m_stream_info;
    Array<int, to_underlying(TrackType::Unknown)> m_preferred_track_for_type;

    HashMap<Track, NonnullOwnPtr<TrackContext>> m_track_contexts;
};

}
