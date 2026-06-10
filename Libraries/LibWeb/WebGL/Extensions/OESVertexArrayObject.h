/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Extensions/WebGLExtension.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class OESVertexArrayObject : public WebGLExtension {
    WEB_WRAPPABLE(OESVertexArrayObject, WebGLExtension);
    GC_DECLARE_ALLOCATOR(OESVertexArrayObject);

public:
    static GC::Ref<WebGLExtension> create(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLVertexArrayObjectOES> create_vertex_array_oes();
    void delete_vertex_array_oes(GC::Root<WebGLVertexArrayObjectOES> array_object);
    bool is_vertex_array_oes(GC::Root<WebGLVertexArrayObjectOES> array_object);
    void bind_vertex_array_oes(GC::Root<WebGLVertexArrayObjectOES> array_object);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    explicit OESVertexArrayObject(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
