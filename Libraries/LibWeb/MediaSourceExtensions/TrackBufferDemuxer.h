/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/FixedArray.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/TimeRanges.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibWeb/Export.h>

namespace Web::MediaSourceExtensions {

// TrackBufferDemuxer stores coded frames for a single track and implements the Demuxer
// interface so that it can be used as a media source for PlaybackManager's data providers.
// It is shared between TrackBuffer (which writes frames) and PlaybackManager (which reads them).
class WEB_API TrackBufferDemuxer final : public Media::Demuxer {
public:
    struct RemovedFrame {
        size_t byte_size { 0 };
        AK::Duration decode_timestamp;
    };

    struct FrameRun {
        Vector<Media::CodedFrame> frames;
        AK::Duration presentation_start;
        AK::Duration presentation_end;
    };

    // The cursor advances one frame at a time until something displaces it, in which case the codec
    // configuration to decode from must be resolved rather than carried over from the previous frame.
    enum class CursorContinuity : u8 {
        Continuous,
        Jumped,
        NeedsReanchoring,
    };

    explicit TrackBufferDemuxer(Media::Track const&);
    virtual ~TrackBufferDemuxer() override;

    Media::Track const& track() const { return m_track; }

    Media::TimeRanges track_buffer_ranges() const;

    void add_coded_frame(Media::CodedFrame);
    void remove_coded_frames_and_dependants_in_range(AK::Duration start, AK::Duration end);
    Optional<AK::Duration> remove_coded_frames_and_dependants_in_range_returning_presentation_timestamp_at(AK::Duration start, AK::Duration end, Optional<AK::Duration> last_decode_timestamp);

    size_t total_bytes() const;

    Optional<AK::Duration> earliest_evictable_frame_timestamp(AK::Duration current_time) const;
    size_t take_earliest_frame_and_dependants();

    Optional<AK::Duration> latest_evictable_frame_timestamp(AK::Duration current_time) const;
    RemovedFrame take_latest_frame();

    void set_reached_end_of_stream();
    void clear_reached_end_of_stream();

    virtual Media::DecoderErrorOr<void> create_context_for_track(Media::Track const&) override;
    virtual Media::DecoderErrorOr<Vector<Media::Track>> get_tracks_for_type(Media::TrackType) override;
    virtual Media::DecoderErrorOr<Optional<Media::Track>> get_preferred_track_for_type(Media::TrackType) override;
    virtual Media::DecoderErrorOr<Media::CodedFrame> get_next_sample_for_track(Media::Track const&) override;
    virtual AK::Duration select_fast_seek_target_for_track(Media::Track const&, AK::Duration target, Media::SeekMode) override;
    virtual Media::DecoderErrorOr<Media::DemuxerSeekResult> seek_to_most_recent_keyframe(Media::Track const&, AK::Duration, Media::DemuxerSeekOptions) override;
    virtual Media::DecoderErrorOr<AK::Duration> duration_of_track(Media::Track const&) override;
    virtual Media::DecoderErrorOr<AK::Duration> total_duration() override;

    virtual Media::DemuxerScanState const& scan_state() const LIFETIME_BOUND override;
    virtual void set_scan_state_change_handler(Function<void()>) override;

    virtual void set_blocking_reads_aborted_for_track(Media::Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Media::Track const&) override;
    virtual void set_read_blocked_change_handler_for_track(Media::Track const&, Media::ReadBlockedChangeHandler) override;

private:
    AK::Duration maximum_time_range_gap() const;
    void count_frame_duration_while_locked(AK::Duration);
    void decrement_frames_with_maximum_duration_while_locked(size_t count);
    Optional<ReadonlyBytes> codec_configuration_at_position_while_locked(size_t run_index, size_t frame_index) const;
    bool is_frame_evictable_while_locked(Media::CodedFrame const&, AK::Duration current_time) const;
    void queue_scan_state_change_dispatch_while_locked();

    bool run_ends_at_last_appended_frame_while_locked(FrameRun const&) const;

    static void extend_run_bounds_for_frame(FrameRun&, Media::CodedFrame const&);
    static void recalculate_run_bounds(FrameRun&);
    static Optional<ReadonlyBytes> codec_configuration_after_frame_prefix_while_locked(FrameRun const&, size_t frame_count);
    void note_cursor_jumped_while_locked();
    void verify_runs_are_ordered_around_index_while_locked(size_t run_index) const;
    void split_run_while_locked(size_t run_index, size_t split_at, FixedArray<u8> codec_configuration_before_tail);
    size_t erase_frames_and_dependants_while_locked(size_t run_index, size_t first_frame, size_t minimum_frame_count);
    Optional<size_t> find_run_to_play_from_while_locked(AK::Duration) const;
    bool move_cursor_to_presentation_time_while_locked(AK::Duration);

    Media::Track m_track;

    mutable Sync::Mutex m_mutex;
    Sync::ConditionVariable m_data_changed { m_mutex };

    Vector<FrameRun> m_runs;
    size_t m_current_run { 0 };
    size_t m_current_frame { 0 };
    CursorContinuity m_cursor_continuity { CursorContinuity::Continuous };
    bool m_reached_end_of_stream { false };

    Optional<AK::Duration> m_cursor_presentation_timestamp;
    Optional<AK::Duration> m_last_appended_decode_timestamp;
    Optional<FixedArray<u8>> m_last_delivered_codec_configuration;
    Optional<FixedArray<u8>> m_last_appended_codec_configuration;

    Media::TimeRanges m_track_buffer_ranges;
    AK::Duration m_maximum_frame_duration;
    size_t m_frames_at_maximum_duration { 0 };
    size_t m_total_bytes { 0 };
    Atomic<bool> m_aborted { false };
    Media::ReadBlockedChangeHandler m_read_blocked_change_handler;

    // Owned by the thread that installed the change handler; mutated only via its event loop.
    Media::DemuxerScanState m_scan_state;
    Function<void()> m_scan_state_change_handler;
    // Guarded by m_mutex, so that track buffer mutations may move off the main thread.
    Core::EventLoop* m_scan_state_change_handler_event_loop { nullptr };
    bool m_scan_state_change_dispatch_pending { false };
};

}
