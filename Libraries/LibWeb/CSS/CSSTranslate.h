/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSTransformComponent.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#csstranslate
class CSSTranslate final : public CSSTransformComponent {
    WEB_PLATFORM_OBJECT(CSSTranslate, CSSTransformComponent);
    GC_DECLARE_ALLOCATOR(CSSTranslate);

public:
    [[nodiscard]] static GC::Ref<CSSTranslate> create(JS::Realm&, Is2D, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z);
    static WebIDL::ExceptionOr<GC::Ref<CSSTranslate>> construct_impl(JS::Realm&, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ptr<CSSNumericValue> z = {});

    virtual ~CSSTranslate() override;

    virtual WebIDL::ExceptionOr<Utf16String> to_string() const override;

    virtual WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> to_matrix() const override;

    GC::Ref<CSSNumericValue> x() const { return m_x; }
    GC::Ref<CSSNumericValue> y() const { return m_y; }
    GC::Ref<CSSNumericValue> z() const { return m_z; }
    WebIDL::ExceptionOr<void> set_x(GC::Ref<CSSNumericValue> value);
    WebIDL::ExceptionOr<void> set_y(GC::Ref<CSSNumericValue> value);
    WebIDL::ExceptionOr<void> set_z(GC::Ref<CSSNumericValue> value);

private:
    explicit CSSTranslate(JS::Realm&, Is2D, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<CSSNumericValue> m_x;
    GC::Ref<CSSNumericValue> m_y;
    GC::Ref<CSSNumericValue> m_z;
};

}
