/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NumericLimits.h>
#include <AK/Types.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibPaintServer/Compositor/DrawCommands.h>

namespace PaintServer {

enum class VisualContextNodeKind : u8 {
    Empty = 0,
    Scroll,
    Clip,
    Transform,
    Perspective,
    ClipPath,
    Effects,
};

enum : u8 {
    None = 0,
    HasEmptyEffectiveClip = 1 << 0,
};

struct DisplayListCommandRecord {
    u32 command_offset { 0 };
    u32 visual_context_index { 0 };
};

struct DisplayListScrollFrameRecord {
    u32 scroll_frame_id { 0 };
    u32 parent_scroll_frame_id { NumericLimits<u32>::max() };
    Gfx::FloatPoint scroll_offset;
};

struct VisualContextNodeRecord {
    u8 kind { to_underlying(VisualContextNodeKind::Empty) };
    u8 flags { None };
    u16 padding { 0 };
    u32 parent_index { 0 };
    u32 data_offset { 0 };
    u32 data_size { 0 };
};

struct DisplayListPayloadFooter {
    u32 total_payload_bytes { 0 };
    u32 command_stream_offset { 0 };
    u32 command_stream_size { 0 };
    u32 command_records_offset { 0 };
    u32 command_record_count { 0 };
    u32 visual_context_nodes_offset { 0 };
    u32 visual_context_node_count { 0 };
    u32 scroll_frames_offset { 0 };
    u32 scroll_frame_count { 0 };
};

}
