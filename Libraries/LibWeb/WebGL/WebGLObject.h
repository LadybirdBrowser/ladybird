/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

class WEB_API WebGLObject : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLObject, Bindings::PlatformObject);

public:
    virtual ~WebGLObject();

    String label() const { return m_label; }
    void set_label(String const& label) { m_label = label; }

    ErrorOr<GLuint> handle(WebGLRenderingContextBase const* context) const;

protected:
    explicit WebGLObject(JS::Realm&, WebGLRenderingContextBase&, GLuint handle);

    void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

    bool invalidated() const { return m_invalidated; }

private:
    // FIXME: It should be GC::Ptr instead of raw pointer, but we need to make WebGLRenderingContextBase inherit from PlatformObject first.
    WebGLRenderingContextBase* m_context;
    GLuint m_handle { 0 };

    bool m_invalidated { false };
    String m_label;
};

}
