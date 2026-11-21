/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/WebGL/Types.h>
#include <LibCompositing/WebGL/WebGLCommandList.h>
#include <LibCompositing/WebGL/WebGLSharedCommandBuffer.h>
#include <LibWeb/Loader/ResourceLoader.h>

// What the WebGL implementation in Libraries/LibWeb/Bindings/Rust/src/web_gl asks of the rest of LibWeb besides its
// canvas.

namespace Web::WebGL {

// What the WebGL implementation in Rust states for itself besides the command structs: the framing and the shared
// data region that the recorder and the replayer of the command stream have to agree on, and the largest drawing
// buffer that the compositor allocates. Rust compares its own constants with these before the first context records
// (fingerprint.rs).
struct ConstantsRestatedInRust {
    size_t record_alignment { 0 };
    size_t header_size { 0 };
    size_t header_payload_size_offset { 0 };
    size_t shared_data_region_offset { 0 };
    size_t shared_data_region_capacity { 0 };
    size_t max_drawing_buffer_dimension { 0 };
};

// NB: The recorder writes the header of a command and that of a synchronous call the same way.
static_assert(sizeof(Compositing::WebGL::WebGLSyncCallHeader) == sizeof(Compositing::WebGL::WebGLCommandHeader));
static_assert(offsetof(Compositing::WebGL::WebGLSyncCallHeader, payload_size) == offsetof(Compositing::WebGL::WebGLCommandHeader, payload_size));
static_assert(offsetof(Compositing::WebGL::WebGLSyncCallHeader, type) == 0 && offsetof(Compositing::WebGL::WebGLCommandHeader, type) == 0);
static_assert(sizeof(Compositing::WebGL::WebGLSyncCallType) == sizeof(u16) && sizeof(Compositing::WebGL::WebGLCommandType) == sizeof(u16));
static_assert(sizeof(Compositing::WebGL::WebGLCommandHeader::payload_size) == sizeof(u32));

// NB: `DataSpan` of libcompositing_rust::webgl::wire is two `u32`s in this order.
static_assert(sizeof(Compositing::WebGL::WebGLDataSpan) == 2 * sizeof(u32));
static_assert(offsetof(Compositing::WebGL::WebGLDataSpan, offset) == 0);
static_assert(offsetof(Compositing::WebGL::WebGLDataSpan, size) == sizeof(u32));

extern "C" {

u8 web_navigator_compatibility_mode();
void web_webgl_constants_restated_in_rust(ConstantsRestatedInRust* out_constants);
}

// The navigator compatibility mode, numbered as Web::NavigatorCompatibilityMode is: Chrome, Gecko, WebKit.
u8 web_navigator_compatibility_mode()
{
    return to_underlying(Web::ResourceLoader::navigator_compatibility_mode());
}

void web_webgl_constants_restated_in_rust(ConstantsRestatedInRust* out_constants)
{
    *out_constants = {
        .record_alignment = Compositing::WebGL::WebGLCommandList::command_alignment,
        .header_size = sizeof(Compositing::WebGL::WebGLCommandHeader),
        .header_payload_size_offset = offsetof(Compositing::WebGL::WebGLCommandHeader, payload_size),
        .shared_data_region_offset = Compositing::WebGL::WebGLSharedCommandBuffer::data_region_offset,
        .shared_data_region_capacity = Compositing::WebGL::WebGLSharedCommandBuffer::default_data_region_capacity,
        .max_drawing_buffer_dimension = Compositing::WebGL::max_webgl_drawing_buffer_dimension,
    };
}

}
