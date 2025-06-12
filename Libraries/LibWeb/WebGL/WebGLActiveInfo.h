/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

typedef unsigned int GLenum;
typedef int GLsizei;

namespace Web::WebGL {

class WebGLActiveInfo : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLActiveInfo, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WebGLActiveInfo);

public:
    static GC::Ptr<WebGLActiveInfo> create(JS::Realm&, String name, GLenum type, GLsizei size);
    virtual ~WebGLActiveInfo();

    GLsizei size() const { return m_size; }
    GLenum type() const { return m_type; }
    String const& name() const { return m_name; }

protected:
    explicit WebGLActiveInfo(JS::Realm&, String name, GLenum type, GLsizei size);

private:
    virtual void initialize(JS::Realm&) override;

    String m_name;
    GLenum m_type { 0 };
    GLsizei m_size { 0 };
};

}
