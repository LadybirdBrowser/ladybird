/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ANGLEInstancedArrays.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class ANGLEInstancedArrays : public Bindings::Wrappable {
    WEB_WRAPPABLE(ANGLEInstancedArrays, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(ANGLEInstancedArrays);

public:
    static JS::ThrowCompletionOr<GC::Ref<Bindings::Wrappable>> create(GC::Ref<WebGLRenderingContextBase>);

    void draw_arrays_instanced_angle(GLenum mode, GLint first, GLsizei count, GLsizei primcount);
    void draw_elements_instanced_angle(GLenum mode, GLsizei count, GLenum type, GLintptr offset, GLsizei primcount);
    void vertex_attrib_divisor_angle(GLuint index, GLuint divisor);

protected:
    void visit_edges(GC::Cell::Visitor&) override;

private:
    ANGLEInstancedArrays(GC::Ref<WebGLRenderingContextBase>);

    GC::Ref<WebGLRenderingContextBase> m_context;
};

}
