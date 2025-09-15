/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSTransformComponent.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssskewy
class CSSSkewY final : public CSSTransformComponent {
    WEB_PLATFORM_OBJECT(CSSSkewY, CSSTransformComponent);
    GC_DECLARE_ALLOCATOR(CSSSkewY);

public:
    [[nodiscard]] static GC::Ref<CSSSkewY> create(JS::Realm&, GC::Ref<CSSNumericValue> ay);
    static WebIDL::ExceptionOr<GC::Ref<CSSSkewY>> construct_impl(JS::Realm&, GC::Ref<CSSNumericValue> ay);

    virtual ~CSSSkewY() override;

    virtual WebIDL::ExceptionOr<Utf16String> to_string() const override;

    virtual WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> to_matrix() const override;

    GC::Ref<CSSNumericValue> ay() const { return m_ay; }
    WebIDL::ExceptionOr<void> set_ay(GC::Ref<CSSNumericValue> value);

    virtual void set_is_2d(bool value) override;

private:
    explicit CSSSkewY(JS::Realm&, GC::Ref<CSSNumericValue> ay);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<CSSNumericValue> m_ay;
};

}
