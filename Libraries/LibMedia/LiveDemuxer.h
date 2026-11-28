/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/FixedArray.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibAudio/SampleSpecification.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaTime.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/CodedFrame.h>

namespace Media {

// A live track placed at the reader's current playback position. Reads never report being blocked,
// so a silent track does not hold up other tracks.
class MEDIA_API LiveDemuxer final : public Demuxer {
public:
    // Audio queued ahead of playback for the output to consume.
    static constexpr AK::Duration AUDIO_TARGET_LATENCY = AK::Duration::from_milliseconds(100);
    // Audio arriving while this much is already waiting to play is dropped rather than played late.
    static constexpr AK::Duration AUDIO_MAXIMUM_LATENCY = AK::Duration::from_milliseconds(300);
    // Video that isn't being read piles up to this many frames, after which a new key frame is needed.
    static constexpr size_t MAXIMUM_QUEUED_VIDEO_FRAMES = 120;
    // Frames follow the cadence of their source's timestamps while that keeps them this close to when they arrived.
    static constexpr AK::Duration VIDEO_MAXIMUM_SMOOTHING_DELAY = AK::Duration::from_milliseconds(150);

    // Audio is interleaved 32-bit float PCM in the given specification.
    static NonnullRefPtr<LiveDemuxer> create_for_audio(u64 track_identifier, Audio::SampleSpecification, Utf16String const& label = {});
    static NonnullRefPtr<LiveDemuxer> create_for_video(u64 track_identifier, Utf16String const& label = {});
    virtual ~LiveDemuxer() override;

    Track const& track() const { return m_track; }

    // The clock of the playback reading this track.
    void set_time_reader(MediaTimeReader);

    // Takes whole frames of native-endian 32-bit float samples.
    void push_audio(ReadonlyBytes interleaved_samples);
    // The time up to which audio has been placed on the timeline, which video arriving now should play alongside.
    AK::Duration audio_end_time() const;

    enum class VideoPacing : u8 {
        // Use source timestamps to smooth arrival jitter.
        FromSource,
        // Frames play when they arrive, for a playback whose position isn't moving along with them.
        OnArrival,
    };
    // The source timestamp only needs to advance at the rate the frames were captured at.
    void push_video_frame(CodecID, FrameFlags, AK::Duration source_timestamp, FixedArray<u8> data, AK::Duration live_edge, VideoPacing = VideoPacing::FromSource);
    // Called on the thread that created the demuxer, when the frames to come can't be decoded without a key frame.
    Function<void()> on_key_frame_needed;

    void end();

    virtual DecoderErrorOr<void> create_context_for_track(Track const&) override;
    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;
    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;
    virtual DecoderLatency decoder_latency() const override { return DecoderLatency::Low; }
    virtual AK::Duration select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode) override;
    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions) override;
    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;
    virtual DemuxerScanState const& scan_state() const LIFETIME_BOUND override;
    virtual void set_scan_state_change_handler(ScanStateChangeHandler) override;
    virtual void set_blocking_reads_aborted_for_track(Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) override;
    virtual void set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler) override;

private:
    explicit LiveDemuxer(Track);

    AK::Duration current_time_while_locked() const;
    i64 frame_index_at(AK::Duration) const;
    AK::Duration time_at_frame_index(i64) const;
    size_t pending_audio_frame_count_while_locked() const;
    AK::Duration audio_end_time_while_locked() const;
    DecoderErrorOr<CodedFrame> get_next_audio_block();
    DecoderErrorOr<CodedFrame> get_next_video_frame();
    CodedFrame take_audio_block_while_locked(size_t frame_count, bool silent);
    void request_key_frame_while_locked();

    Track m_track;
    NonnullRefPtr<Core::WeakEventLoopReference> m_event_loop;

    mutable Mutex m_mutex;
    ConditionVariable m_data_changed { m_mutex };
    Atomic<bool> m_aborted { false };
    bool m_ended { false };
    bool m_needs_codec_configuration { true };
    Optional<MediaTimeReader> m_time_reader;

    // Audio that has arrived and not yet been read, from m_pending_audio_offset on.
    Vector<float> m_pending_audio;
    size_t m_pending_audio_offset { 0 };
    i64 m_next_audio_frame_index { 0 };

    Vector<CodedFrame> m_video_frames;
    bool m_waiting_for_key_frame { true };
    bool m_key_frame_request_is_pending { false };
    bool m_reader_depends_on_earlier_frames { false };
    Optional<AK::Duration> m_last_video_source_timestamp;
    // The frame whose arrival the following ones are timed from.
    AK::Duration m_video_anchor_source_timestamp;
    AK::Duration m_video_anchor_timestamp;
    Optional<AK::Duration> m_last_video_timestamp;

    // Only touched on the thread that created the demuxer.
    DemuxerScanState m_scan_state;
    ScanStateChangeHandler m_scan_state_change_handler;
};

}
