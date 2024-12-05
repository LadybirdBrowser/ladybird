/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class WebGLObject : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLObject, Bindings::PlatformObject);

public:
    virtual ~WebGLObject();

    String label() const { return m_label; }
    void set_label(String const& label) { m_label = label; }

    GLuint handle() const { return m_handle; }

protected:
    explicit WebGLObject(JS::Realm&, GLuint handle);

    virtual void initialize(JS::Realm&) override;

    bool invalidated() const { return m_invalidated; }

private:
    bool m_invalidated { false };
    String m_label;
    GLuint m_handle { 0 };
};

}
