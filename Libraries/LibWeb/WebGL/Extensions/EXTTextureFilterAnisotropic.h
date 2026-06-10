/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Extensions/WebGLExtension.h>

namespace Web::WebGL {

class EXTTextureFilterAnisotropic : public WebGLExtension {
    WEB_WRAPPABLE(EXTTextureFilterAnisotropic, WebGLExtension);
    GC_DECLARE_ALLOCATOR(EXTTextureFilterAnisotropic);

public:
    static GC::Ref<WebGLExtension> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    EXTTextureFilterAnisotropic(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
