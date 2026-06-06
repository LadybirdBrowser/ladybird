/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/WebGLDrawBuffers.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class WebGLDrawBuffers : public Bindings::Wrappable {
    WEB_WRAPPABLE(WebGLDrawBuffers, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(WebGLDrawBuffers);

public:
    static JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> create(GC::Ref<WebGLRenderingContextBase>);

    void draw_buffers_webgl(Vector<GLenum> buffers);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    WebGLDrawBuffers(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
