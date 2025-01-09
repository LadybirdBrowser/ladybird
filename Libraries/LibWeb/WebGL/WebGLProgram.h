/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLProgram final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLProgram, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLProgram);

public:
    static GC::Ref<WebGLProgram> create(JS::Realm& realm, WebGLRenderingContextBase&, GLuint handle);

    virtual ~WebGLProgram();

    GC::Ptr<WebGLShader> attached_vertex_shader() const { return m_attached_fragment_shader; }
    void set_attached_vertex_shader(GC::Ptr<WebGLShader> shader) { m_attached_vertex_shader = shader; }

    GC::Ptr<WebGLShader> attached_fragment_shader() const { return m_attached_fragment_shader; }
    void set_attached_fragment_shader(GC::Ptr<WebGLShader> shader) { m_attached_fragment_shader = shader; }

protected:
    explicit WebGLProgram(JS::Realm&, WebGLRenderingContextBase&, GLuint handle);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    GC::Ptr<WebGLShader> m_attached_vertex_shader;
    GC::Ptr<WebGLShader> m_attached_fragment_shader;
};

}
