/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <LibMedia/Forward.h>
#include <LibMedia/TimeRanges.h>
#include <LibMedia/Track.h>

namespace Web::MediaSourceExtensions {

class ByteStreamParser;
struct DemuxedCodedFrame;
class TrackBuffer;
class TrackBufferDemuxer;

// These enums are declared separately from the bindings to avoid including the entire prototype header here.

// https://w3c.github.io/media-source/#dfn-append-state
enum class AppendState : u8 {
    WaitingForSegment,
    ParsingInitSegment,
    ParsingMediaSegment,
};

// https://w3c.github.io/media-source/#dom-appendmode
enum class AppendMode : u8 {
    Segments,
    Sequence,
};

struct InitializationSegmentTrack {
    Media::Track track;
    NonnullRefPtr<TrackBufferDemuxer> demuxer;
};

struct InitializationSegmentData {
    Vector<InitializationSegmentTrack> audio_tracks;
    Vector<InitializationSegmentTrack> video_tracks;
    Vector<InitializationSegmentTrack> text_tracks;
};

class SourceBufferProcessor : public AtomicRefCounted<SourceBufferProcessor> {
public:
    SourceBufferProcessor();
    ~SourceBufferProcessor();

    void set_parser(NonnullOwnPtr<ByteStreamParser>&&);

    bool updating() const { return m_updating; }
    void set_updating(bool value) { m_updating = value; }

    AppendMode mode() const;
    bool is_parsing_media_segment() const;
    bool generate_timestamps_flag() const;
    AK::Duration group_end_timestamp() const;
    bool is_buffer_full() const;

    void set_mode(AppendMode);
    void set_generate_timestamps_flag(bool);
    void set_group_start_timestamp(Optional<AK::Duration>);
    bool first_initialization_segment_received_flag() const;
    void set_first_initialization_segment_received_flag(bool);
    void set_pending_initialization_segment_for_change_type_flag(bool);

    using DurationChangeCallback = Function<void(double new_duration)>;
    using InitializationSegmentCallback = Function<void(InitializationSegmentData&&)>;
    using AppendErrorCallback = Function<void()>;
    using CodedFrameProcessingDoneCallback = Function<void()>;
    using AppendDoneCallback = Function<void()>;

    void set_duration_change_callback(DurationChangeCallback);
    void set_first_initialization_segment_callback(InitializationSegmentCallback);
    void set_append_error_callback(AppendErrorCallback);
    void set_coded_frame_processing_done_callback(CodedFrameProcessingDoneCallback);
    void set_append_done_callback(AppendDoneCallback);

    void append_to_input_buffer(ReadonlyBytes);

    void run_segment_parser_loop();
    void reset_parser_state();
    void run_coded_frame_eviction();

    void set_reached_end_of_stream();
    void clear_reached_end_of_stream();

    Media::TimeRanges buffered_ranges() const;

private:
    void drop_consumed_bytes_from_input_buffer();
    void unset_all_track_buffer_timestamps();
    void set_need_random_access_point_flag_on_all_track_buffers(bool);

    void initialization_segment_received();
    void run_coded_frame_processing(Vector<DemuxedCodedFrame>&);

    // https://w3c.github.io/media-source/#dom-sourcebuffer-updating
    bool m_updating { false };
    // https://w3c.github.io/media-source/#dfn-input-buffer
    ByteBuffer m_input_buffer;
    OwnPtr<ByteStreamParser> m_parser;
    NonnullRefPtr<Media::ReadonlyBytesCursor> m_cursor;
    HashMap<u64, NonnullOwnPtr<TrackBuffer>> m_track_buffers;

    DurationChangeCallback m_duration_change_callback;
    InitializationSegmentCallback m_first_initialization_segment_callback;
    AppendErrorCallback m_append_error_callback;
    CodedFrameProcessingDoneCallback m_coded_frame_processing_done_callback;
    AppendDoneCallback m_append_done_callback;

    // https://w3c.github.io/media-source/#dfn-append-state
    AppendState m_append_state { AppendState::WaitingForSegment };
    // https://w3c.github.io/media-source/#dom-appendmode
    AppendMode m_mode { AppendMode::Segments };
    // https://w3c.github.io/media-source/#dfn-group-start-timestamp
    Optional<AK::Duration> m_group_start_timestamp;
    // https://w3c.github.io/media-source/#dfn-group-end-timestamp
    AK::Duration m_group_end_timestamp;
    // https://w3c.github.io/media-source/#dfn-generate-timestamps-flag
    bool m_generate_timestamps_flag { false };
    // https://w3c.github.io/media-source/#dfn-first-initialization-segment-received-flag
    bool m_first_initialization_segment_received_flag { false };
    // https://w3c.github.io/media-source/#dfn-pending-initialization-segment-for-changetype-flag
    bool m_pending_initialization_segment_for_change_type_flag { false };
    // https://w3c.github.io/media-source/#dfn-buffer-full-flag
    bool m_buffer_full_flag { false };
};

}
