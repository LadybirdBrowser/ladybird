/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/GraphDescription.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>

namespace Web::WebAudio::Render {

struct WireFlags {
    static constexpr u32 contains_external_resources = 1u << 1;
};

struct WireGraphBuildResult {
    GraphDescription description;
    NonnullOwnPtr<GraphResourceRegistry> resources;

    u32 flags { 0 };
    f32 context_sample_rate_hz { 0.0f };
    u32 param_automation_event_count { 0 };
};

// Binary wire encoding for RenderGraphDescription
WEB_API ErrorOr<ByteBuffer> encode_render_graph_for_media_server(GraphDescription const&, f32 context_sample_rate, GraphResourceResolver const&);

// Decode a wire message into a runnable RenderGraphDescription plus a resource registry.
//
// - BufferTable payloads are materialized into resources and referenced by buffer_id.
// - AudioBufferSource node descriptions contain metadata and buffer_id but omit PCM channels.
// - MediaElementAudioSource provider_id is kept; the actual provider must be supplied out-of-band.
WEB_API ErrorOr<WireGraphBuildResult> decode_render_graph_wire_format(ReadonlyBytes bytes);

}
