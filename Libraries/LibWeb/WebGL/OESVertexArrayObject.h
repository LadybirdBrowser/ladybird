/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLVertexArrayObjectOES.h>

namespace Web::WebGL {

class OESVertexArrayObject : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(OESVertexArrayObject, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(OESVertexArrayObject);

public:
    static JS::ThrowCompletionOr<GC::Ptr<OESVertexArrayObject>> create(JS::Realm&, GC::Ref<WebGLRenderingContext>);

    GC::Ref<WebGLVertexArrayObjectOES> create_vertex_array_oes();
    void delete_vertex_array_oes(GC::Root<WebGLVertexArrayObjectOES> array_object);
    bool is_vertex_array_oes(GC::Root<WebGLVertexArrayObjectOES> array_object);
    void bind_vertex_array_oes(GC::Root<WebGLVertexArrayObjectOES> array_object);

protected:
    void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

private:
    OESVertexArrayObject(JS::Realm&, GC::Ref<WebGLRenderingContext>);

    GC::Ref<WebGLRenderingContext> m_context;
};

}
