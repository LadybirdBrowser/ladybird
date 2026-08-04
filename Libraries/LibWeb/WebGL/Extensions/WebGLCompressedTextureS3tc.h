/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class WebGLCompressedTextureS3tc : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(WebGLCompressedTextureS3tc, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(WebGLCompressedTextureS3tc);

public:
    static GC::Ref<Bindings::Wrappable> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(Visitor&) override;

private:
    WebGLCompressedTextureS3tc(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
