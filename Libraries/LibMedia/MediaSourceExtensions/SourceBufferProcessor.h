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
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/TimeRanges.h>
#include <LibMedia/Track.h>

namespace Media::MediaSourceExtensions {

class ByteStreamParser;
struct DemuxedCodedFrame;
class TrackBuffer;
class TrackBufferDemuxer;

// https://w3c.github.io/media-source/#dom-appendmode
enum class AppendMode : u8 {
    Segments,
    Sequence,
};

// https://w3c.github.io/media-source/#dfn-append-state
enum class AppendState : u8 {
    WaitingForSegment,
    ParsingInitSegment,
    ParsingMediaSegment,
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

// Every mutation of the processor is a command, so that ordering between them is the queue's
// order rather than the caller's control flow. Each corresponds to a spec algorithm or an
// attribute setter's mutating steps.
namespace Commands {

struct SetParser {
    NonnullOwnPtr<ByteStreamParser> parser;
};

// https://w3c.github.io/media-source/#sourcebuffer-buffer-append
struct BufferAppend {
    ByteBuffer data;
};

// https://w3c.github.io/media-source/#sourcebuffer-reset-parser-state
struct ResetParserState { };

// https://w3c.github.io/media-source/#sourcebuffer-coded-frame-removal
struct CodedFrameRemoval {
    AK::Duration start;
    AK::Duration end;
};

// https://w3c.github.io/media-source/#sourcebuffer-coded-frame-eviction
struct CodedFrameEviction {
    size_t new_data_size { 0 };
    AK::Duration current_time;
};

// https://w3c.github.io/media-source/#dom-sourcebuffer-mode
struct SetMode {
    AppendMode mode;
};

// https://w3c.github.io/media-source/#dom-sourcebuffer-timestampoffset
struct SetTimestampOffset {
    AK::Duration timestamp_offset;
};

// https://w3c.github.io/media-source/#dfn-generate-timestamps-flag
struct SetGenerateTimestampsFlag {
    bool flag { false };
};

// https://w3c.github.io/media-source/#dfn-pending-initialization-segment-for-changetype-flag
struct SetPendingInitializationSegmentForChangeTypeFlag {
    bool flag { false };
};

struct SetReachedEndOfStream {
    bool reached { false };
};

}

using Command = Variant<
    Commands::SetParser,
    Commands::BufferAppend,
    Commands::ResetParserState,
    Commands::CodedFrameRemoval,
    Commands::CodedFrameEviction,
    Commands::SetMode,
    Commands::SetTimestampOffset,
    Commands::SetGenerateTimestampsFlag,
    Commands::SetPendingInitializationSegmentForChangeTypeFlag,
    Commands::SetReachedEndOfStream>;

// The processor state that its owner reads synchronously. Republished after each command, so a
// read never races a command that has not finished.
struct PublishedState {
    // https://w3c.github.io/media-source/#track-buffer-ranges
    Media::TimeRanges buffered_ranges;
    // https://w3c.github.io/media-source/#dom-sourcebuffer-timestampoffset
    AK::Duration timestamp_offset;
    // https://w3c.github.io/media-source/#dfn-append-state
    AppendState append_state { AppendState::WaitingForSegment };
    // https://w3c.github.io/media-source/#dom-appendmode
    AppendMode mode { AppendMode::Segments };
    // https://w3c.github.io/media-source/#dfn-generate-timestamps-flag
    bool generate_timestamps_flag { false };
    // https://w3c.github.io/media-source/#dfn-buffer-full-flag
    bool buffer_full { false };
    AK::Duration highest_presentation_timestamp;
    AK::Duration highest_end_time;
};

class MEDIA_API SourceBufferProcessor : public AtomicRefCounted<SourceBufferProcessor> {
public:
    SourceBufferProcessor();
    ~SourceBufferProcessor();

    PublishedState const& published_state() const { return m_published_state; }

    // NB: The spec runs most mutating steps synchronously, and only the buffer append algorithm
    //     asynchronously — so a caller either runs a command immediately, or enqueues it and lets
    //     the algorithm's own task run the queue.
    void run(Command);
    void enqueue(Command);
    void run_pending_commands();

    // NB: Abandons a queued or in-progress buffer append. The spec leaves the interruption point
    //     undefined, so a partly consumed input buffer keeps whatever it already produced; see
    //     https://github.com/w3c/media-source/issues/71.
    void abandon_buffer_append();

    using DurationChangeCallback = Function<void(double new_duration)>;
    using InitializationSegmentCallback = Function<void(InitializationSegmentData&&)>;
    using AppendErrorCallback = Function<void()>;
    using CodedFrameProcessingDoneCallback = Function<void(AK::Duration group_end_timestamp)>;
    using AppendDoneCallback = Function<void()>;

    void set_duration_change_callback(DurationChangeCallback);
    void set_first_initialization_segment_callback(InitializationSegmentCallback);
    void set_append_error_callback(AppendErrorCallback);
    void set_coded_frame_processing_done_callback(CodedFrameProcessingDoneCallback);
    void set_append_done_callback(AppendDoneCallback);

private:
    void execute(Command&);
    void publish();

    void run_segment_parser_loop();
    void reset_parser_state();
    void run_coded_frame_removal(AK::Duration start, AK::Duration end);
    void run_coded_frame_eviction(size_t new_data_size, AK::Duration current_time);
    void set_reached_end_of_stream(bool);

    size_t total_buffered_bytes() const;
    size_t capacity_in_bytes() const;
    AK::Duration highest_presentation_timestamp() const;
    AK::Duration highest_end_time() const;
    Media::TimeRanges buffered_ranges() const;

    void drop_consumed_bytes_from_input_buffer();
    void unset_all_track_buffer_timestamps();
    void set_need_random_access_point_flag_on_all_track_buffers(bool);

    [[nodiscard]] bool initialization_segment_received();
    void run_coded_frame_processing(Vector<DemuxedCodedFrame>&);

    Vector<Command> m_pending_commands;
    bool m_running_commands { false };

    PublishedState m_published_state;

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
    // https://w3c.github.io/media-source/#dom-sourcebuffer-timestampoffset
    AK::Duration m_timestamp_offset;
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
