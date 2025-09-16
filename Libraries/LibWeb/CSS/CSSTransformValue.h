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
    WEB_PLATFORM_OBJECT(CSSTransformValue, CSSStyleValue);
    GC_DECLARE_ALLOCATOR(CSSTransformValue);

public:
    [[nodiscard]] static GC::Ref<CSSTransformValue> create(JS::Realm&, Vector<GC::Ref<CSSTransformComponent>>);
    static WebIDL::ExceptionOr<GC::Ref<CSSTransformValue>> construct_impl(JS::Realm&, GC::RootVector<GC::Root<CSSTransformComponent>>);

    virtual ~CSSTransformValue() override;

    WebIDL::UnsignedLong length() const;
    virtual Optional<JS::Value> item_value(size_t index) const override;
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_indexed_property(u32, JS::Value) override;
    virtual WebIDL::ExceptionOr<void> set_value_of_new_indexed_property(u32, JS::Value) override;

    bool is_2d() const;
    WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> to_matrix() const;

    virtual WebIDL::ExceptionOr<String> to_string() const override;

private:
    explicit CSSTransformValue(JS::Realm&, Vector<GC::Ref<CSSTransformComponent>>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    Vector<GC::Ref<CSSTransformComponent>> m_transforms;
};

}
