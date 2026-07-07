/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>

namespace Web::Painting {

struct WEB_API Canvas2DCommandStreamSegment {
    CanvasId canvas_id;
    Gfx::CanvasCommandList commands;
    bool present { false };
};

// An ordered stream of canvas 2D commands covering all canvases of a WebContent
// connection. Replaying segments strictly in order guarantees that a command
// referencing another canvas (DrawCanvas) observes all of that canvas's earlier
// commands without requiring an eager flush at record time.
class WEB_API Canvas2DCommandStream : public RefCounted<Canvas2DCommandStream> {
public:
    // The returned reference is invalidated by any subsequent stream mutation
    // (recording for another canvas, a present marker, or a flush) — do not
    // hold it across calls that may touch the stream.
    Gfx::CanvasCommandList& commands_for(CanvasId);
    void record_present(CanvasId);

    bool is_empty() const { return m_segments.is_empty(); }
    size_t total_command_count() const;

    Vector<Canvas2DCommandStreamSegment> take_segments();

private:
    void append_segment(CanvasId, bool present);

    Vector<Canvas2DCommandStreamSegment> m_segments;
    // The tail segment grows through the reference commands_for() hands out,
    // so its command count is only known at query time.
    size_t m_command_count_excluding_tail { 0 };
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::Canvas2DCommandStreamSegment const&);
template<>
WEB_API ErrorOr<Web::Painting::Canvas2DCommandStreamSegment> decode(Decoder&);

}
