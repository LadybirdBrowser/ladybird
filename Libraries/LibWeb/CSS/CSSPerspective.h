/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSKeywordValue.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/CSSTransformComponent.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#typedefdef-cssperspectivevalue
// NB: CSSKeywordish is flattened here, because our bindings generator flattens nested variants.
using CSSPerspectiveValue = Variant<GC::Root<CSSNumericValue>, String, GC::Root<CSSKeywordValue>>;
using CSSPerspectiveValueInternal = Variant<GC::Ref<CSSNumericValue>, GC::Ref<CSSKeywordValue>>;

// https://drafts.css-houdini.org/css-typed-om-1/#cssperspective
class CSSPerspective final : public CSSTransformComponent {
    WEB_PLATFORM_OBJECT(CSSPerspective, CSSTransformComponent);
    GC_DECLARE_ALLOCATOR(CSSPerspective);

public:
    [[nodiscard]] static GC::Ref<CSSPerspective> create(JS::Realm&, CSSPerspectiveValueInternal);
    static WebIDL::ExceptionOr<GC::Ref<CSSPerspective>> construct_impl(JS::Realm&, CSSPerspectiveValue);

    virtual ~CSSPerspective() override;

    virtual WebIDL::ExceptionOr<Utf16String> to_string() const override;

    virtual WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> to_matrix() const override;

    CSSPerspectiveValue length() const;
    WebIDL::ExceptionOr<void> set_length(CSSPerspectiveValue value);

    virtual void set_is_2d(bool value) override;

private:
    explicit CSSPerspective(JS::Realm&, CSSPerspectiveValueInternal);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    CSSPerspectiveValueInternal m_length;
};

}

class CSSPerspective {
};
