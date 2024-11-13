/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::WebGL {

class WebGLObject : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLObject, Bindings::PlatformObject);

public:
    virtual ~WebGLObject();

    String label() const { return m_label; }
    void set_label(String const& label) { m_label = label; }

protected:
    explicit WebGLObject(JS::Realm&);

    bool invalidated() const { return m_invalidated; }

private:
    bool m_invalidated { false };
    String m_label;
};

}
