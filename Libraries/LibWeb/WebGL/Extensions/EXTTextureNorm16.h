/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Extensions/WebGLExtension.h>

namespace Web::WebGL {

class EXTTextureNorm16 : public WebGLExtension {
    WEB_WRAPPABLE(EXTTextureNorm16, WebGLExtension);
    GC_DECLARE_ALLOCATOR(EXTTextureNorm16);

public:
    static GC::Ref<WebGLExtension> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    EXTTextureNorm16(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
