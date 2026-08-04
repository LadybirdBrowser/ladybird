/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class WebGLCompressedTextureS3tcSrgb : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(WebGLCompressedTextureS3tcSrgb, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(WebGLCompressedTextureS3tcSrgb);

public:
    static GC::Ref<Bindings::Wrappable> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(Visitor&) override;

private:
    WebGLCompressedTextureS3tcSrgb(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
