/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class OESVertexArrayObject : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(OESVertexArrayObject, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(OESVertexArrayObject);

public:
    static GC::Ref<Bindings::Wrappable> create(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLVertexArrayObjectOES> create_vertex_array_oes();
    void delete_vertex_array_oes(GC::Ptr<WebGLVertexArrayObjectOES> array_object);
    bool is_vertex_array_oes(GC::Ptr<WebGLVertexArrayObjectOES> array_object);
    void bind_vertex_array_oes(GC::Ptr<WebGLVertexArrayObjectOES> array_object);

protected:
    void visit_edges(Visitor&) override;

private:
    OESVertexArrayObject(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
