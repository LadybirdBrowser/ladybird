/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class EXTTextureNorm16 : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(EXTTextureNorm16, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(EXTTextureNorm16);

public:
    static GC::Ref<Bindings::Wrappable> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(Visitor&) override;

private:
    EXTTextureNorm16(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
