/*
 * Copyright (c) 2025, contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Export.h>
#include <LibMedia/FFmpeg/FFmpegForward.h>

namespace Media::FFmpeg {

// MSEDemuxer is a specialized demuxer for Media Source Extensions (MSE) that supports
// progressive/streaming media playback. Unlike FFmpegDemuxer which requires complete
// file data upfront, MSEDemuxer accepts fragmented MP4 segments incrementally.
//
// MSE Workflow:
// 1. append_initialization_segment() - Parse ftyp + moov boxes to get codec info
// 2. append_media_segment() - Repeatedly append moof + mdat boxes with actual media data
// 3. get_next_sample_for_track() - Pull decoded packets for playback
//
// This demuxer uses a custom AVIOContext that reads from a growing ByteBuffer,
// allowing FFmpeg to parse fragmented MP4 as data arrives.
class MEDIA_API MSEDemuxer : public Demuxer {
public:
    static DecoderErrorOr<NonnullRefPtr<MSEDemuxer>> create();
    virtual ~MSEDemuxer() override;

    // MSE-specific methods for progressive data appending
    DecoderErrorOr<void> append_initialization_segment(ReadonlyBytes);
    DecoderErrorOr<void> append_media_segment(ReadonlyBytes);
    DecoderErrorOr<void> remove(AK::Duration start, AK::Duration end);

    // Demuxer interface implementation
    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType type) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType type) override;
    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions) override;
    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;
    virtual DecoderErrorOr<CodecID> get_codec_id_for_track(Track const& track) override;
    virtual DecoderErrorOr<ReadonlyBytes> get_codec_initialization_data_for_track(Track const& track) override;
    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const& track) override;

private:
    struct TrackContext {
        TrackContext();
        ~TrackContext();
        TrackContext(TrackContext&&) = default;

        AVFormatContext* format_context { nullptr };
        AVPacket* packet { nullptr };
        bool peeked_packet_already { false };
    };

    // Custom read callback for AVIOContext
    static int avio_read_callback(void* opaque, uint8_t* buf, int buf_size);
    static int64_t avio_seek_callback(void* opaque, int64_t offset, int whence);

    MSEDemuxer();

    DecoderErrorOr<void> initialize_format_context();
    TrackContext& get_track_context(Track const&);
    DecoderErrorOr<Track> get_track_for_stream_index(u32 stream_index);

    // Growing buffer that holds all appended data
    ByteBuffer m_buffer;

    // Current read position in buffer (used by avio callbacks)
    size_t m_read_position { 0 };

    // Size of the initialization segment (to skip past it when seeking to start)
    size_t m_init_segment_size { 0 };

    // FFmpeg context
    AVFormatContext* m_format_context { nullptr };
    AVIOContext* m_avio_context { nullptr };

    // Per-track contexts for independent packet reading
    HashMap<Track, NonnullOwnPtr<TrackContext>> m_track_contexts;

    // Track whether we've processed initialization segment
    bool m_initialized { false };

    // Estimated duration from initialization segment (may be updated as we append more data)
    AK::Duration m_duration;
};

}
