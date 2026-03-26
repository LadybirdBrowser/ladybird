/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/Function.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#sourcebuffer-append-state
enum class AppendState : u8 {
    WaitingForSegment,
    ParsingInitSegment,
    ParsingMediaSegment,
};

enum class AppendMode : u8 {
    Segments,
    Sequence,
};

class SourceBufferProcessor : public AtomicRefCounted<SourceBufferProcessor> {
public:
    SourceBufferProcessor();
    ~SourceBufferProcessor();

    bool updating() const { return m_updating; }
    void set_updating(bool value) { m_updating = value; }

    AppendMode mode() const;
    bool is_buffer_full() const;
    using AppendErrorCallback = Function<void()>;

    void set_append_error_callback(AppendErrorCallback);

    void append_to_input_buffer(ReadonlyBytes);

    void run_segment_parser_loop();
    void reset_parser_state();
    void run_coded_frame_eviction();

private:
    bool m_updating { false };
    ByteBuffer m_input_buffer;

    AppendErrorCallback m_append_error_callback;
    AppendState m_append_state { AppendState::WaitingForSegment };
    AppendMode m_mode { AppendMode::Segments };
    bool m_buffer_full_flag { false };
};

}
