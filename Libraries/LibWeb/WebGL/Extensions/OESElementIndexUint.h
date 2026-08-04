/*
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

class OESElementIndexUint : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(OESElementIndexUint, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(OESElementIndexUint);

public:
    static GC::Ref<Bindings::Wrappable> create(GC::Ref<WebGLRenderingContextBase>);

protected:
    void visit_edges(Visitor&) override;

private:
    OESElementIndexUint(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
