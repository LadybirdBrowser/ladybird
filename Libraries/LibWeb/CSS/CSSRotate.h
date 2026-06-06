/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/CSSTransformComponent.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssrotate
class CSSRotate final : public CSSTransformComponent {
    WEB_WRAPPABLE(CSSRotate, CSSTransformComponent);
    GC_DECLARE_ALLOCATOR(CSSRotate);

public:
    [[nodiscard]] static GC::Ref<CSSRotate> create(Is2D, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z, GC::Ref<CSSNumericValue> angle);
    static WebIDL::ExceptionOr<GC::Ref<CSSRotate>> construct_impl(GC::Ref<CSSNumericValue> angle);
    static WebIDL::ExceptionOr<GC::Ref<CSSRotate>> construct_impl(CSSNumberish x, CSSNumberish y, CSSNumberish z, GC::Ref<CSSNumericValue> angle);

    virtual ~CSSRotate() override;

    virtual WebIDL::ExceptionOr<Utf16String> to_string() const override;

    virtual WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> to_matrix(JS::Realm&) const override;

    CSSNumberish x() const { return m_x; }
    CSSNumberish y() const { return m_y; }
    CSSNumberish z() const { return m_z; }
    GC::Ref<CSSNumericValue> angle() const { return m_angle; }
    WebIDL::ExceptionOr<void> set_x(CSSNumberish value);
    WebIDL::ExceptionOr<void> set_y(CSSNumberish value);
    WebIDL::ExceptionOr<void> set_z(CSSNumberish value);
    WebIDL::ExceptionOr<void> set_angle(GC::Ref<CSSNumericValue> value);

    virtual WebIDL::ExceptionOr<NonnullRefPtr<TransformationStyleValue const>> create_style_value(PropertyNameAndID const&) const override;

private:
    explicit CSSRotate(Is2D, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z, GC::Ref<CSSNumericValue> angle);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<CSSNumericValue> m_x;
    GC::Ref<CSSNumericValue> m_y;
    GC::Ref<CSSNumericValue> m_z;
    GC::Ref<CSSNumericValue> m_angle;
};

}
