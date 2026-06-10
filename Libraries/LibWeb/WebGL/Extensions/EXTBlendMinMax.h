/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Extensions/WebGLExtension.h>

namespace Web::WebGL {

class EXTBlendMinMax : public WebGLExtension {
    WEB_WRAPPABLE(EXTBlendMinMax, WebGLExtension);
    GC_DECLARE_ALLOCATOR(EXTBlendMinMax);

public:
    static GC::Ref<WebGLExtension> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    EXTBlendMinMax(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
