/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibGC/Ptr.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

using CanvasOwner = Variant<GC::Ref<HTML::HTMLCanvasElement>, GC::Ref<HTML::OffscreenCanvas>>;
HTML::CanvasHost& canvas_host_for(CanvasOwner const&);

// https://registry.khronos.org/webgl/specs/latest/1.0/#WEBGLCONTEXTATTRIBUTES
// `ContextAttributes` of Bindings/Rust/src/web_gl/exports.rs.
struct ContextAttributes {
    bool alpha { true };
    bool depth { true };
    bool stencil { false };
    bool antialias { true };
    bool premultiplied_alpha { true };
    bool preserve_drawing_buffer { false };
    // 0 is "default", 1 is "low-power" and 2 is "high-performance".
    u8 power_preference { 0 };
    bool fail_if_major_performance_caveat { false };
    bool desynchronized { false };
};

JS::ThrowCompletionOr<ContextAttributes> convert_to_context_attributes(JS::VM&, JS::Value);

// The C++ side of a WebGLRenderingContext or a WebGL2RenderingContext, which are implemented in Rust
// (Libraries/LibWeb/Bindings/Rust/src/web_gl): what the rest of LibWeb calls, and what the Rust code calls back.
class WEB_API WebGLRenderingContextBase : public Bindings::RustInterfaceCell {
public:
    HTML::CanvasHost& canvas_host() const;

    bool is_context_lost() const;
    Optional<Compositing::CanvasId> canvas_id() const;

    void set_size(Gfx::IntSize const&);
    void reset_to_default_state() { }
    void prepare_for_compositing();
    void clear_drawing_buffer();
    Optional<Gfx::DecodedImageFrame> read_back_drawing_buffer(Gfx::IntRect const&);

    // https://registry.khronos.org/webgl/specs/latest/1.0/#CONTEXT_LOST
    // The compositor process (and with it the GL state) went away: set the context lost
    // flag, stop talking to the dead host, and fire webglcontextlost at the canvas.
    void lose_context_from_compositor_loss();

    // The compositor came back. If the page asked for restoration (called preventDefault on
    // webglcontextlost), build a fresh remote context and fire webglcontextrestored so the
    // page can re-create its now-lost GL resources.
    void restore_context_after_compositor_reconnect();

protected:
    static WebGLRenderingContextBase* create(JS::Realm&, CanvasOwner, ContextAttributes const&, WebGLVersion);
};

}
