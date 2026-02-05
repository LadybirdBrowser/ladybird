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

class WebGLCompressedTextureS3tcSrgb : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLCompressedTextureS3tcSrgb, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WebGLCompressedTextureS3tcSrgb);

public:
    static JS::ThrowCompletionOr<GC::Ptr<WebGLCompressedTextureS3tcSrgb>> create(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

protected:
    void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

private:
    WebGLCompressedTextureS3tcSrgb(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
