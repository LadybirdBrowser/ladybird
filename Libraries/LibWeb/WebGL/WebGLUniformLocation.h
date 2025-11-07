/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLProgram.h>

namespace Web::WebGL {

class WebGLUniformLocation final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLUniformLocation, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WebGLUniformLocation);

public:
    static GC::Ref<WebGLUniformLocation> create(JS::Realm& realm, GLuint handle, GC::Ptr<WebGLProgram> parent_shader);

    virtual ~WebGLUniformLocation();

    ErrorOr<GLuint> handle(GC::Ptr<WebGLProgram> current_shader) const;

protected:
    explicit WebGLUniformLocation(JS::Realm&, GLuint handle, GC::Ptr<WebGLProgram> parent_shader);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    GLuint m_handle { 0 };
    GC::Ptr<WebGLProgram> m_parent_shader;
};

}
