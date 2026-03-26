/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferProcessor.h>

namespace Web::MediaSourceExtensions {

SourceBufferProcessor::SourceBufferProcessor()
{
}
SourceBufferProcessor::~SourceBufferProcessor() = default;

AppendMode SourceBufferProcessor::mode() const
{
    return m_mode;
}

bool SourceBufferProcessor::is_buffer_full() const
{
    return m_buffer_full_flag;
}

void SourceBufferProcessor::set_append_error_callback(AppendErrorCallback callback)
{
    m_append_error_callback = move(callback);
}

void SourceBufferProcessor::append_to_input_buffer(ReadonlyBytes bytes)
{
    m_input_buffer.append(bytes);
}

// https://w3c.github.io/media-source/#sourcebuffer-segment-parser-loop
void SourceBufferProcessor::run_segment_parser_loop()
{
    m_append_error_callback();
}

// https://w3c.github.io/media-source/#sourcebuffer-reset-parser-state
void SourceBufferProcessor::reset_parser_state()
{
    // 1. If the [[append state]] equals PARSING_MEDIA_SEGMENT and the [[input buffer]] contains some
    //    complete coded frames, then run the coded frame processing algorithm until all of these
    //    complete coded frames have been processed.
    if (m_append_state == AppendState::ParsingMediaSegment) {
        // FIXME: Process any complete coded frames
    }

    // FIXME: 2. Unset the last decode timestamp on all track buffers.
    // FIXME: 3. Unset the last frame duration on all track buffers.
    // FIXME: 4. Unset the highest end timestamp on all track buffers.

    // FIXME: 5. Set the need random access point flag on all track buffers to true.

    // 6. If the mode attribute equals "sequence", then
    if (m_mode == AppendMode::Sequence) {
        // FIXME: set the [[group start timestamp]] to the [[group end timestamp]]
    }

    // 7. Remove all bytes from the [[input buffer]].
    m_input_buffer.clear();

    // 8. Set [[append state]] to WAITING_FOR_SEGMENT.
    m_append_state = AppendState::WaitingForSegment;
}

// https://w3c.github.io/media-source/#sourcebuffer-coded-frame-eviction
void SourceBufferProcessor::run_coded_frame_eviction()
{
    // FIXME: 1. Let new data equal the data that is about to be appended to this SourceBuffer.
    //        2. If the [[buffer full flag]] equals false, then abort these steps.
    //        3. Let removal ranges equal a list of presentation time ranges that can be evicted from the presentation
    //           to make room for the new data.
    //        4. For each range in removal ranges, run the coded frame removal algorithm with start and end equal to
    //           the removal range start and end timestamp respectively.
}

}
