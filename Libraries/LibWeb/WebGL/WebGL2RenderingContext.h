/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGL2RenderingContextOverloads.h>
#include <LibWeb/WebGL/WebGLContextAttributes.h>

namespace Web::WebGL {

class WebGL2RenderingContext final : public WebGL2RenderingContextOverloads {
    WEB_PLATFORM_OBJECT(WebGL2RenderingContext, WebGL2RenderingContextOverloads);
    GC_DECLARE_ALLOCATOR(WebGL2RenderingContext);

public:
    static JS::ThrowCompletionOr<GC::Ptr<WebGL2RenderingContext>> create(JS::Realm&, HTML::HTMLCanvasElement& canvas_element, JS::Value options);

    virtual ~WebGL2RenderingContext() override;

    void prepare_for_compositing() override;
    void did_update_canvas_content() override;

    virtual GC::Ref<HTML::HTMLCanvasElement> canvas_for_binding() const override;

    Optional<WebGLContextAttributes> get_context_attributes();

    void set_size(Gfx::IntSize const&);
    void reset_to_default_state();

    WebIDL::Long drawing_buffer_width() const;
    WebIDL::Long drawing_buffer_height() const;

private:
    virtual void initialize(JS::Realm&) override;

    WebGL2RenderingContext(JS::Realm&, HTML::HTMLCanvasElement&, NonnullOwnPtr<WebGLContextProxy> context, WebGLContextAttributes context_creation_parameters, WebGLContextAttributes actual_context_parameters);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual bool reestablish_remote_context() override;

    GC::Ref<HTML::HTMLCanvasElement> m_canvas_element;

    // https://www.khronos.org/registry/webgl/specs/latest/1.0/#context-creation-parameters
    // Each WebGLRenderingContext has context creation parameters, set upon creation, in a WebGLContextAttributes object.
    WebGLContextAttributes m_context_creation_parameters {};

    // https://www.khronos.org/registry/webgl/specs/latest/1.0/#actual-context-parameters
    // Each WebGLRenderingContext has actual context parameters, set each time the drawing buffer is created, in a WebGLContextAttributes object.
    WebGLContextAttributes m_actual_context_parameters {};
};

}
