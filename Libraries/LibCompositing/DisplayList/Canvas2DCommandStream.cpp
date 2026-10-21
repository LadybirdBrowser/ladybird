/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/Canvas2DCommandStream.h>
#include <LibGfx/Font/Font.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Compositing {

FontResourceId Canvas2DCommandStream::add_font(Gfx::Font const& font)
{
    m_fonts.ensure(font.id(), [&]() -> NonnullRefPtr<Gfx::Font const> { return font; });
    return { font.id() };
}

Vector<DisplayListFontResource> Canvas2DCommandStream::take_fonts()
{
    Vector<DisplayListFontResource> fonts;
    fonts.ensure_capacity(m_fonts.size());
    for (auto& entry : m_fonts)
        fonts.unchecked_append({ .id = FontResourceId { entry.key }, .font = move(entry.value) });
    m_fonts.clear();
    return fonts;
}

ImageFrameResourceId Canvas2DCommandStream::add_image_frame(Gfx::DecodedImageFrame const& frame)
{
    m_image_frames.ensure(frame.id(), [&] { return frame; });
    return { frame.id() };
}

Vector<DisplayListImageFrameResource> Canvas2DCommandStream::take_image_frames()
{
    Vector<DisplayListImageFrameResource> frames;
    frames.ensure_capacity(m_image_frames.size());
    for (auto& entry : m_image_frames)
        frames.unchecked_append({ .id = ImageFrameResourceId { entry.key }, .frame = move(entry.value) });
    m_image_frames.clear();
    return frames;
}

void Canvas2DCommandStream::retain_video_frame_hold(NonnullRefPtr<VideoFrameHold> hold)
{
    if (!m_video_frame_holds.is_empty() && m_video_frame_holds.last().ptr() == hold.ptr())
        return;
    m_video_frame_holds.append(move(hold));
}

void Canvas2DCommandStream::append_segment(CanvasId canvas_id, bool present)
{
    m_segments.append({ .canvas_id = canvas_id, .tape = {}, .present = present });
}

void Canvas2DCommandStream::append_command(CanvasId canvas_id, ReadonlyBytes record)
{
    // A presented segment must keep its position in the stream, so recording
    // after a present marker starts a new segment for the same canvas.
    if (m_segments.is_empty() || m_segments.last().canvas_id != canvas_id || m_segments.last().present)
        append_segment(canvas_id, false);
    m_segments.last().tape.append(record);
    ++m_total_command_count;
}

void Canvas2DCommandStream::record_present(CanvasId canvas_id)
{
    if (!m_segments.is_empty() && m_segments.last().canvas_id == canvas_id) {
        m_segments.last().present = true;
        return;
    }
    append_segment(canvas_id, true);
}

Vector<Canvas2DCommandStreamSegment> Canvas2DCommandStream::take_segments()
{
    m_total_command_count = 0;
    return move(m_segments);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::Canvas2DCommandStreamSegment const& segment)
{
    TRY(encoder.encode(segment.canvas_id));
    TRY(encoder.encode(segment.tape));
    TRY(encoder.encode(segment.present));
    return {};
}

template<>
ErrorOr<Compositing::Canvas2DCommandStreamSegment> decode(Decoder& decoder)
{
    auto canvas_id = TRY(decoder.decode<Compositing::CanvasId>());
    auto tape = TRY(decoder.decode<ByteBuffer>());
    auto present = TRY(decoder.decode<bool>());
    return Compositing::Canvas2DCommandStreamSegment {
        .canvas_id = canvas_id,
        .tape = move(tape),
        .present = present,
    };
}

}
