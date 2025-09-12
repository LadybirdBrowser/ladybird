/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#csstransformcomponent
class CSSTransformComponent : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CSSTransformComponent, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CSSTransformComponent);

public:
    enum class Is2D : u8 {
        No,
        Yes,
    };

    virtual void initialize(JS::Realm&) override;
    virtual ~CSSTransformComponent() override;

    virtual WebIDL::ExceptionOr<Utf16String> to_string() const = 0;

    bool is_2d() const { return m_is_2d; }
    virtual void set_is_2d(bool value) { m_is_2d = value; }

    virtual WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> to_matrix() const = 0;

protected:
    explicit CSSTransformComponent(JS::Realm&, Is2D is_2d);

private:
    bool m_is_2d;
};

}
