/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Export.h>
#include <LibMedia/FFmpeg/FFmpegForward.h>
#include <LibMedia/FFmpeg/FFmpegIOContext.h>

namespace Media::FFmpeg {

class MEDIA_API FFmpegDemuxer : public Demuxer {
public:
    static DecoderErrorOr<NonnullRefPtr<FFmpegDemuxer>> from_stream(NonnullRefPtr<IncrementallyPopulatedStream> const&);

    static bool sniff_mp4(IncrementallyPopulatedStream&);

    virtual ~FFmpegDemuxer() override;

    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;

    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions) override;

    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;

    virtual DecoderErrorOr<CodecID> get_codec_id_for_track(Track const&) override;

    virtual DecoderErrorOr<ReadonlyBytes> get_codec_initialization_data_for_track(Track const&) override;

    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;

    virtual void cancel_blocking_reads() override;

private:
    struct TrackContext {
        TrackContext(NonnullOwnPtr<FFmpegIOContext>&& io_context)
            : io_context(move(io_context))
        {
        }
        ~TrackContext();
        TrackContext(TrackContext&&) = default;

        NonnullOwnPtr<FFmpegIOContext> io_context;
        AVFormatContext* format_context { nullptr };
        AVPacket* packet { nullptr };
        bool is_seekable { true };
        bool peeked_packet_already { false };
    };

    FFmpegDemuxer(NonnullRefPtr<IncrementallyPopulatedStream>, NonnullOwnPtr<Media::FFmpeg::FFmpegIOContext>&&);

    TrackContext& get_track_context(Track const&);
    DecoderErrorOr<Track> get_track_for_stream_index(u32 stream_index);

    NonnullRefPtr<IncrementallyPopulatedStream> m_stream;
    NonnullOwnPtr<FFmpegIOContext> m_io_context;
    AVFormatContext* m_format_context;

    Threading::Mutex m_track_contexts_mutex;
    HashMap<Track, NonnullOwnPtr<TrackContext>> m_track_contexts;
};

}
