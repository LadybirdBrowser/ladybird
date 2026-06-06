/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

class WEB_API WebGLObject : public Bindings::Wrappable {
    WEB_WRAPPABLE(WebGLObject, Bindings::Wrappable);

public:
    virtual ~WebGLObject();

    String label() const { return m_label; }
    void set_label(String const& label) { m_label = label; }

    ErrorOr<GLuint> handle(WebGLRenderingContextBase const* context) const;

protected:
    explicit WebGLObject(JS::Realm&, GC::Ref<WebGLRenderingContextBase>, GLuint handle);
    void visit_edges(GC::Cell::Visitor&) override;

    bool invalidated() const { return m_invalidated; }

    GC::Ref<WebGLRenderingContextBase> m_context;

private:
    GLuint m_handle { 0 };

    bool m_invalidated { false };
    String m_label;
};

}
