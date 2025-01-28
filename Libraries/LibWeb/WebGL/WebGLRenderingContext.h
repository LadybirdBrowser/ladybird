/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLContextAttributes.h>
#include <LibWeb/WebGL/WebGLRenderingContextImpl.h>

namespace Web::WebGL {

class WebGLRenderingContext : public Bindings::PlatformObject
    , public WebGLRenderingContextImpl {
    WEB_PLATFORM_OBJECT(WebGLRenderingContext, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WebGLRenderingContext);

public:
    static JS::ThrowCompletionOr<GC::Ptr<WebGLRenderingContext>> create(JS::Realm&, HTML::HTMLCanvasElement& canvas_element, JS::Value options);

    virtual ~WebGLRenderingContext() override;

    // FIXME: This is a hack required to visit context from WebGLObject.
    //        It should be gone once WebGLRenderingContextBase inherits from PlatformObject.
    GC::Cell const* gc_cell() const override { return this; }

    void present() override;
    void needs_to_present() override;

    GC::Ref<HTML::HTMLCanvasElement> canvas_for_binding() const;

    bool is_context_lost() const;
    Optional<WebGLContextAttributes> get_context_attributes();

    RefPtr<Gfx::PaintingSurface> surface();
    void allocate_painting_surface_if_needed();

    void set_size(Gfx::IntSize const&);
    void reset_to_default_state();

    Optional<Vector<String>> get_supported_extensions();
    JS::Object* get_extension(String const& name);

    WebIDL::Long drawing_buffer_width() const;
    WebIDL::Long drawing_buffer_height() const;

private:
    virtual void initialize(JS::Realm&) override;

    WebGLRenderingContext(JS::Realm&, HTML::HTMLCanvasElement&, NonnullOwnPtr<OpenGLContext> context, WebGLContextAttributes context_creation_parameters, WebGLContextAttributes actual_context_parameters);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTML::HTMLCanvasElement> m_canvas_element;

    // https://www.khronos.org/registry/webgl/specs/latest/1.0/#context-creation-parameters
    // Each WebGLRenderingContext has context creation parameters, set upon creation, in a WebGLContextAttributes object.
    WebGLContextAttributes m_context_creation_parameters {};

    // https://www.khronos.org/registry/webgl/specs/latest/1.0/#actual-context-parameters
    // Each WebGLRenderingContext has actual context parameters, set each time the drawing buffer is created, in a WebGLContextAttributes object.
    WebGLContextAttributes m_actual_context_parameters {};

    // https://www.khronos.org/registry/webgl/specs/latest/1.0/#webgl-context-lost-flag
    // Each WebGLRenderingContext has a webgl context lost flag, which is initially unset.
    bool m_context_lost { false };

    // WebGL presents its drawing buffer to the HTML page compositor immediately before a compositing operation, but only if at least one of the following has occurred since the previous compositing operation:
    // - Context creation
    // - Canvas resize
    // - clear, drawArrays, or drawElements has been called while the drawing buffer is the currently bound framebuffer
    bool m_should_present { true };

    GLenum m_error { 0 };

    // Extensions
    // "Multiple calls to getExtension with the same extension string, taking into account case-insensitive comparison, must return the same object as long as the extension is enabled."
    GC::Ptr<Extensions::ANGLEInstancedArrays> m_angle_instanced_arrays_extension;
    GC::Ptr<Extensions::OESVertexArrayObject> m_oes_vertex_array_object_extension;
    GC::Ptr<Extensions::WebGLCompressedTextureS3tc> m_webgl_compressed_texture_s3tc_extension;
    GC::Ptr<Extensions::WebGLDrawBuffers> m_webgl_draw_buffers_extension;

    virtual void set_error(GLenum error) override;
};

void fire_webgl_context_event(HTML::HTMLCanvasElement& canvas_element, FlyString const& type);
void fire_webgl_context_creation_error(HTML::HTMLCanvasElement& canvas_element);

}
