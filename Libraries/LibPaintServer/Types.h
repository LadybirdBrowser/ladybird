/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGfx/BitmapInfo.h>
#include <LibGfx/Resource/Resource.h>

namespace PaintServer {

using ResourceID = Gfx::ResourceID;
using SurfaceID = u64;
using FrameID = u64;
using RequestID = u64;
using GPUEpoch = u64;
using ReleaseToken = Gfx::ReleaseToken;
using SyncToken = u64;
using OperationID = u64;
using ConnectionID = u32;
using ArenaID = u32;
using PageID = u64;
using ImageID = Gfx::ImageID;

enum class ClientKind : u8 {
    Broker = 1,
    RenderClient = 2,
};

enum class ArenaPacketKind : u8 {
    Frame = 1,
    Resource = 2,
    Canvas = 3,
};

enum class CompletionStatus : u8 {
    Rendered = 0,
    Ingested = 1,
};

enum class FrameOutputType : u8 {
    Presentation = 0,
    Offscreen = 1,
};

enum class OffscreenTargetKind : u8 {
    ShareableBitmap = 0,
    ContentImage = 1,
};

enum class OffscreenBackendPreference : u8 {
    PreferGPU = 0,
    RequireCPU = 1,
};

struct OffscreenRenderTarget {
    OffscreenTargetKind kind { OffscreenTargetKind::ShareableBitmap };
    OffscreenBackendPreference backend_preference { OffscreenBackendPreference::RequireCPU };
    RequestID request_id { 0 };
    ImageID image_id { 0 };
};

struct FrameHeader {
    FrameOutputType output_type { FrameOutputType::Presentation };
    OffscreenRenderTarget offscreen_target;
    Gfx::FloatSize viewport_size { 1.0f, 1.0f };
    Gfx::FloatPoint presentation_offset { 0.0f, 0.0f };
    u64 payload_generation { 0 };
    f32 device_pixels_per_css_pixel { 1.0f };
    u64 webcontent_submission_timestamp_ms { 0 };
    u32 payload_length { 0 };
};

struct ArenaFramePacketPrefix {
    ArenaPacketKind kind { ArenaPacketKind::Frame };
    FrameHeader frame_header;
    SyncToken render_wait_sync_token { 0 };
};

struct ArenaResourcePacket {
    ArenaPacketKind kind { ArenaPacketKind::Resource };
    Gfx::ResourceInfo descriptor;
    u32 data_size { 0 };
};

struct ArenaCanvasPacketPrefix {
    ArenaPacketKind kind { ArenaPacketKind::Canvas };
    ImageID image_id { 0 };
    Gfx::IntSize size;
    Gfx::BitmapFormat format { Gfx::BitmapFormat::BGRA8888 };
    u32 payload_length { 0 };
};

}
