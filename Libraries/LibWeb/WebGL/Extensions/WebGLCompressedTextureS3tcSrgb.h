/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/WebGLCompressedTextureS3tcSrgb.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class WebGLCompressedTextureS3tcSrgb : public Bindings::Wrappable {
    WEB_WRAPPABLE(WebGLCompressedTextureS3tcSrgb, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(WebGLCompressedTextureS3tcSrgb);

public:
    static JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    WebGLCompressedTextureS3tcSrgb(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
