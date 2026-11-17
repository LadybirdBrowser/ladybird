/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>

namespace Compositing {

// The records of one canvas, on the display list tape (see Rust/src/display_list/canvas_2d.rs), in the stream that
// WebContent shares between all of its canvases. Replaying segments strictly in order guarantees that a command
// referencing another canvas (DrawCanvas) observes all of that canvas's earlier commands without requiring an eager
// flush at record time.
struct Canvas2DCommandStreamSegment {
    CanvasId canvas_id;
    ByteBuffer tape;
    bool present { false };
};

}
