/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class ANGLEInstancedArrays : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ANGLEInstancedArrays, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ANGLEInstancedArrays);

public:
    static JS::ThrowCompletionOr<GC::Ptr<ANGLEInstancedArrays>> create(JS::Realm&, GC::Ref<WebGLRenderingContext>);

    void draw_arrays_instanced_angle(GLenum mode, GLint first, GLsizei count, GLsizei primcount);
    void draw_elements_instanced_angle(GLenum mode, GLsizei count, GLenum type, GLintptr offset, GLsizei primcount);
    void vertex_attrib_divisor_angle(GLuint index, GLuint divisor);

protected:
    void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

private:
    ANGLEInstancedArrays(JS::Realm&, GC::Ref<WebGLRenderingContext>);

    GC::Ref<WebGLRenderingContext> m_context;
};

}
