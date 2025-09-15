/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSTransformComponent.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssskewx
class CSSSkewX final : public CSSTransformComponent {
    WEB_PLATFORM_OBJECT(CSSSkewX, CSSTransformComponent);
    GC_DECLARE_ALLOCATOR(CSSSkewX);

public:
    [[nodiscard]] static GC::Ref<CSSSkewX> create(JS::Realm&, GC::Ref<CSSNumericValue> ax);
    static WebIDL::ExceptionOr<GC::Ref<CSSSkewX>> construct_impl(JS::Realm&, GC::Ref<CSSNumericValue> ax);

    virtual ~CSSSkewX() override;

    virtual WebIDL::ExceptionOr<Utf16String> to_string() const override;

    virtual WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> to_matrix() const override;

    GC::Ref<CSSNumericValue> ax() const { return m_ax; }
    WebIDL::ExceptionOr<void> set_ax(GC::Ref<CSSNumericValue> value);

    virtual void set_is_2d(bool value) override;

private:
    explicit CSSSkewX(JS::Realm&, GC::Ref<CSSNumericValue> ax);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<CSSNumericValue> m_ax;
};

}
