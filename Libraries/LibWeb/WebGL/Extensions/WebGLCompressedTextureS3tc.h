/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Extensions/WebGLExtension.h>

namespace Web::WebGL {

class WebGLCompressedTextureS3tc : public WebGLExtension {
    WEB_WRAPPABLE(WebGLCompressedTextureS3tc, WebGLExtension);
    GC_DECLARE_ALLOCATOR(WebGLCompressedTextureS3tc);

public:
    static GC::Ref<WebGLExtension> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    WebGLCompressedTextureS3tc(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
