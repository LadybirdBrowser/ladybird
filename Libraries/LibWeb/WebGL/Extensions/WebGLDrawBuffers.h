/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class WebGLDrawBuffers : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(WebGLDrawBuffers, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(WebGLDrawBuffers);

public:
    static GC::Ref<Bindings::Wrappable> create(GC::Ref<WebGLRenderingContextBase>);

    void draw_buffers_webgl(Vector<GLenum> buffers);

protected:
    void visit_edges(Visitor&) override;

private:
    WebGLDrawBuffers(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
