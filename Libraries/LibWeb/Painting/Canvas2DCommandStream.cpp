/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Painting/Canvas2DCommandStream.h>

namespace Web::Painting {

void Canvas2DCommandStream::append_segment(CanvasId canvas_id, bool present)
{
    if (!m_segments.is_empty())
        m_command_count_excluding_tail += m_segments.last().commands.size();
    m_segments.append({ .canvas_id = canvas_id, .commands = {}, .present = present });
}

Gfx::CanvasCommandList& Canvas2DCommandStream::commands_for(CanvasId canvas_id)
{
    // A presented segment must keep its position in the stream, so recording
    // after a present marker starts a new segment for the same canvas.
    if (m_segments.is_empty() || m_segments.last().canvas_id != canvas_id || m_segments.last().present)
        append_segment(canvas_id, false);
    return m_segments.last().commands;
}

void Canvas2DCommandStream::record_present(CanvasId canvas_id)
{
    if (!m_segments.is_empty() && m_segments.last().canvas_id == canvas_id) {
        m_segments.last().present = true;
        return;
    }
    append_segment(canvas_id, true);
}

size_t Canvas2DCommandStream::total_command_count() const
{
    if (m_segments.is_empty())
        return 0;
    return m_command_count_excluding_tail + m_segments.last().commands.size();
}

Vector<Canvas2DCommandStreamSegment> Canvas2DCommandStream::take_segments()
{
    m_command_count_excluding_tail = 0;
    return move(m_segments);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::Canvas2DCommandStreamSegment const& segment)
{
    TRY(encoder.encode(segment.canvas_id));
    TRY(encoder.encode(segment.commands));
    TRY(encoder.encode(segment.present));
    return {};
}

template<>
ErrorOr<Web::Painting::Canvas2DCommandStreamSegment> decode(Decoder& decoder)
{
    return Web::Painting::Canvas2DCommandStreamSegment {
        .canvas_id = TRY(decoder.decode<Web::Painting::CanvasId>()),
        .commands = TRY(decoder.decode<Gfx::CanvasCommandList>()),
        .present = TRY(decoder.decode<bool>()),
    };
}

}
