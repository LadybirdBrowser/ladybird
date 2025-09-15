/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/CSSTransformComponent.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssscale
class CSSScale final : public CSSTransformComponent {
    WEB_PLATFORM_OBJECT(CSSScale, CSSTransformComponent);
    GC_DECLARE_ALLOCATOR(CSSScale);

public:
    [[nodiscard]] static GC::Ref<CSSScale> create(JS::Realm&, Is2D, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z);
    static WebIDL::ExceptionOr<GC::Ref<CSSScale>> construct_impl(JS::Realm&, CSSNumberish x, CSSNumberish y, Optional<CSSNumberish> z = {});

    virtual ~CSSScale() override;

    virtual WebIDL::ExceptionOr<Utf16String> to_string() const override;

    virtual WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> to_matrix() const override;

    CSSNumberish x() const { return GC::Root { m_x }; }
    CSSNumberish y() const { return GC::Root { m_y }; }
    CSSNumberish z() const { return GC::Root { m_z }; }
    WebIDL::ExceptionOr<void> set_x(CSSNumberish value);
    WebIDL::ExceptionOr<void> set_y(CSSNumberish value);
    WebIDL::ExceptionOr<void> set_z(CSSNumberish value);

private:
    explicit CSSScale(JS::Realm&, Is2D, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<CSSNumericValue> m_x;
    GC::Ref<CSSNumericValue> m_y;
    GC::Ref<CSSNumericValue> m_z;
};

}
