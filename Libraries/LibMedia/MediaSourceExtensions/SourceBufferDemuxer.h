/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/FixedArray.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Export.h>
#include <LibMedia/TimeRanges.h>

namespace Media::MediaSourceExtensions {

// SourceBufferDemuxer stores the coded frames of every track of a SourceBuffer and implements the Demuxer
// interface so that it can be used as a media source for PlaybackManager's data providers. It is shared between
// the SourceBuffer's track buffers (which write frames) and PlaybackManager (which reads them).
class MEDIA_API SourceBufferDemuxer final : public Media::Demuxer {
public:
    struct RemovedFrame {
        size_t byte_size { 0 };
        AK::Duration decode_timestamp;
    };

    struct FrameRun {
        Vector<Media::CodedFrame> frames;
        AK::Duration presentation_start;
        AK::Duration highest_presentation_start;
        AK::Duration presentation_end;
    };

    // The cursor advances one frame at a time until something displaces it, in which case the codec
    // configuration to decode from must be resolved rather than carried over from the previous frame.
    enum class CursorContinuity : u8 {
        Continuous,
        Jumped,
        NeedsReanchoring,
    };

    explicit SourceBufferDemuxer(Vector<Media::Track> const&);
    virtual ~SourceBufferDemuxer() override;

    Media::TimeRanges track_buffer_ranges(Media::Track const&) const;
    Vector<Media::TimeRanges> track_buffered_ranges() const;
    Media::TimeRanges buffered_ranges() const;
    AK::Duration highest_presentation_timestamp(Media::Track const&) const;

    void add_coded_frame(Media::Track const&, Media::CodedFrame);
    void carry_codec_configuration_of_dropped_frame(Media::Track const&, Media::CodedFrame const&);
    void remove_coded_frames_and_dependants_in_range(Media::Track const&, AK::Duration start, AK::Duration end);
    Optional<AK::Duration> remove_coded_frames_and_dependants_in_range_returning_presentation_timestamp_at(Media::Track const&, AK::Duration start, AK::Duration end, Optional<AK::Duration> last_decode_timestamp);

    size_t total_bytes(Media::Track const&) const;

    Optional<AK::Duration> earliest_evictable_frame_timestamp(Media::Track const&, AK::Duration current_time) const;
    size_t take_earliest_frame_and_dependants(Media::Track const&);

    Optional<AK::Duration> latest_evictable_frame_timestamp(Media::Track const&, AK::Duration current_time) const;
    RemovedFrame take_latest_frame(Media::Track const&);

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
    struct TrackData {
        AK_ALLOC_WITH_KMALLOC;

        explicit TrackData(Media::Track const& track)
            : track(track)
        {
        }

        Media::Track track;

        Vector<FrameRun> runs;
        size_t current_run { 0 };
        size_t current_frame { 0 };
        CursorContinuity cursor_continuity { CursorContinuity::Continuous };
        Optional<AK::Duration> cursor_presentation_timestamp;
        // The presentation time that reads have reached in the buffered ranges, which a read may only move past
        // within the range that contains it. Seeking sets it to the seek's target.
        Optional<AK::Duration> read_anchor;

        Optional<AK::Duration> last_appended_decode_timestamp;
        Optional<FixedArray<u8>> last_delivered_codec_configuration;
        Optional<FixedArray<u8>> last_appended_codec_configuration;
        Optional<FixedArray<u8>> codec_configuration_of_dropped_frame;

        Media::TimeRanges track_buffer_ranges;
        AK::Duration maximum_frame_duration;
        size_t frames_at_maximum_duration { 0 };
        size_t total_bytes { 0 };
        Atomic<bool> aborted { false };
        Media::ReadBlockedChangeHandler read_blocked_change_handler;
    };

    TrackData& track_data(Media::Track const&);
    TrackData const& track_data(Media::Track const&) const;

    static bool contributes_to_buffered_ranges(TrackData const&);
    static AK::Duration maximum_time_range_gap(TrackData const&);
    static Media::TimeRanges coalesced_track_buffer_ranges(TrackData const&);
    static void count_frame_duration(TrackData&, AK::Duration);
    static void decrement_frames_with_maximum_duration(TrackData&, size_t count);
    static Optional<ReadonlyBytes> codec_configuration_at_position(TrackData const&, size_t run_index, size_t frame_index);
    static bool is_frame_evictable(TrackData const&, Media::CodedFrame const&, AK::Duration current_time);
    static bool run_ends_at_last_appended_frame(TrackData const&, FrameRun const&);
    static void note_cursor_jumped(TrackData&);
    static void verify_runs_are_ordered_around_index(TrackData const&, size_t run_index);
    static void split_run(TrackData&, size_t run_index, size_t split_at, FixedArray<u8> codec_configuration_before_tail);
    static size_t erase_frames_and_dependants(TrackData&, size_t run_index, size_t first_frame, size_t minimum_frame_count);
    Optional<size_t> find_run_to_play_from_while_locked(TrackData const&, AK::Duration) const;
    bool move_cursor_to_presentation_time_while_locked(TrackData&, AK::Duration);

    static void extend_run_bounds_for_frame(FrameRun&, Media::CodedFrame const&);
    static void recalculate_run_bounds(FrameRun&);
    static Optional<ReadonlyBytes> codec_configuration_after_frame_prefix(FrameRun const&, size_t frame_count);

    Vector<Media::TimeRanges> track_buffered_ranges_while_locked() const;
    Media::DemuxerScanState scan_state_while_locked() const;
    AK::Duration highest_end_time_while_locked() const;
    bool may_read_frame_while_locked(TrackData const&, Media::CodedFrame const&) const;
    void buffered_data_changed_while_locked();
    void queue_scan_state_change_dispatch_while_locked();

    Vector<NonnullOwnPtr<TrackData>> m_tracks;

    mutable Mutex m_mutex;
    ConditionVariable m_data_changed { m_mutex };
    bool m_reached_end_of_stream { false };

    // The ranges in which every audio and video track has data, which bound what reads may deliver.
    Media::TimeRanges m_buffered_ranges;

    // Owned by the thread that installed the change handler; mutated only via its event loop.
    Media::DemuxerScanState m_scan_state;
    Function<void()> m_scan_state_change_handler;
    // Guarded by m_mutex, so that track buffer mutations may move off the main thread.
    Core::EventLoop* m_scan_state_change_handler_event_loop { nullptr };
    bool m_scan_state_change_dispatch_pending { false };
};

}
