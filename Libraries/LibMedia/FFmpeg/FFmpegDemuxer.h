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
#include <AK/OwnPtr.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/Containers/ContainerNavigator.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerScanThread.h>
#include <LibMedia/Export.h>
#include <LibMedia/FFmpeg/FFmpegForward.h>
#include <LibMedia/FFmpeg/FFmpegIOContext.h>
#include <LibMedia/Forward.h>

namespace Media::FFmpeg {

class MEDIA_API FFmpegDemuxer : public Demuxer {
public:
    static bool should_attempt(NonnullRefPtr<MediaStream> const&);
    static DecoderErrorOr<NonnullRefPtr<Demuxer>> from_stream(NonnullRefPtr<MediaStream> const&);
    static bool supports_container_mime_type(ContainerMimeType);
    static bool supports_codec_in_container(ContainerID, CodecID);

    virtual ~FFmpegDemuxer() override;

    virtual DecoderErrorOr<void> create_context_for_track(Track const&) override;

    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;

    virtual AK::Duration select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode) override;
    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions) override;

    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;
    virtual Optional<AK::UnixDateTime> start_time_realtime() const override;

    virtual DemuxerScanState const& scan_state() const LIFETIME_BOUND override;
    virtual void set_scan_state_change_handler(Function<void()>) override;

    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;

    virtual void set_blocking_reads_aborted_for_track(Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) override;
    virtual void set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler) override;

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
        bool needs_codec_configuration { true };
        Optional<AK::Duration> pending_timestamp_offset;
        AK::Duration timestamp_offset;
    };

    struct BufferedScanPayload {
        NonnullOwnPtr<ContainerNavigator> navigator;
        Vector<Track> tracks;
        AK::Duration initial_duration;
    };

    FFmpegDemuxer(NonnullRefPtr<MediaStream> const&);

    static OwnPtr<ContainerNavigator> create_single_track_container_navigator(AVFormatContext&, AK::Duration, NonnullRefPtr<MediaStream> const&);

    void start_buffered_scan_thread(AVFormatContext&);

    StreamInfo const& get_track_info(Track const&) const;
    TrackContext& get_track_context(Track const&);

    NonnullRefPtr<MediaStream> m_stream;
    AK::Duration m_total_duration;
    Optional<AK::UnixDateTime> m_start_time_realtime;
    Vector<StreamInfo> m_stream_info;
    RefPtr<DemuxerScanThread<BufferedScanPayload>> m_buffered_scan_thread;
    DemuxerScanState m_fallback_scan_state;
    Array<int, to_underlying(TrackType::Unknown)> m_preferred_track_for_type;

    HashMap<Track, NonnullOwnPtr<TrackContext>> m_track_contexts;
};

}
