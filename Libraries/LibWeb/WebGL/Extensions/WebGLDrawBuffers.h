/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL::Extensions {

class WebGLDrawBuffers : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLDrawBuffers, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WebGLDrawBuffers);

public:
    static JS::ThrowCompletionOr<GC::Ptr<WebGLDrawBuffers>> create(JS::Realm&, GC::Ref<WebGLRenderingContext>);

    void draw_buffers_webgl(Vector<GLenum> buffers);

protected:
    void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

private:
    WebGLDrawBuffers(JS::Realm&, GC::Ref<WebGLRenderingContext>);

    GC::Ref<WebGLRenderingContext> m_context;
};

}
