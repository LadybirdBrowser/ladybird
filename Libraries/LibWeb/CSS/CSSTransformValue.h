/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#csstransformvalue
class CSSTransformValue final : public CSSStyleValue {
    WEB_WRAPPABLE(CSSTransformValue, CSSStyleValue);
    GC_DECLARE_ALLOCATOR(CSSTransformValue);

public:
    [[nodiscard]] static GC::Ref<CSSTransformValue> create(ReadonlySpan<GC::Ref<CSSTransformComponent>>);
    static WebIDL::ExceptionOr<GC::Ref<CSSTransformValue>> create_for_constructor(ReadonlySpan<GC::Ref<CSSTransformComponent>>);

    virtual ~CSSTransformValue() override;

    WebIDL::UnsignedLong length() const;
    GC::Ptr<CSSTransformComponent> component_at(size_t index) const;
    WebIDL::ExceptionOr<void> set_value_of_existing_indexed_property(u32, GC::Ref<CSSTransformComponent>);
    WebIDL::ExceptionOr<void> set_value_of_new_indexed_property(u32, GC::Ref<CSSTransformComponent>);

    bool is_2d() const;
    WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> to_matrix() const;

    virtual WebIDL::ExceptionOr<String> to_string() const override;

    virtual WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> create_an_internal_representation(PropertyNameAndID const&, PerformTypeCheck) const override;

private:
    explicit CSSTransformValue(Vector<GC::Ref<CSSTransformComponent>>);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    Vector<GC::Ref<CSSTransformComponent>> m_transforms;
};

}
