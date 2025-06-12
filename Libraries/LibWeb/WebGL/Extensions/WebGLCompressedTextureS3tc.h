/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL::Extensions {

class WebGLCompressedTextureS3tc : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLCompressedTextureS3tc, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WebGLCompressedTextureS3tc);

public:
    static JS::ThrowCompletionOr<GC::Ptr<WebGLCompressedTextureS3tc>> create(JS::Realm&, WebGLRenderingContextBase*);

protected:
    void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

private:
    WebGLCompressedTextureS3tc(JS::Realm&, WebGLRenderingContextBase*);

    // FIXME: It should be GC::Ptr instead of raw pointer, but we need to make WebGLRenderingContextBase inherit from PlatformObject first.
    WebGLRenderingContextBase* m_context;
};

}
