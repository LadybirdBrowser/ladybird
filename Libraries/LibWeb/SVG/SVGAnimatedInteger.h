/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedInteger
class SVGAnimatedInteger final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGAnimatedInteger, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGAnimatedInteger);

public:
    enum class SupportsSecondValue : u8 {
        Yes,
        No,
    };
    enum class ValueRepresented : u8 {
        First,
        Second,
    };

    [[nodiscard]] static GC::Ref<SVGAnimatedInteger> create(JS::Realm&, GC::Ref<SVGElement>,
        DOM::QualifiedName reflected_attribute, WebIDL::Long initial_value, SupportsSecondValue = SupportsSecondValue::No,
        ValueRepresented = ValueRepresented::First);
    virtual ~SVGAnimatedInteger() override;

    WebIDL::Long base_val() const;
    void set_base_val(WebIDL::Long);

    WebIDL::Long anim_val() const;

private:
    SVGAnimatedInteger(JS::Realm&, GC::Ref<SVGElement>, DOM::QualifiedName, WebIDL::Long, SupportsSecondValue, ValueRepresented);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    WebIDL::Long parse_value_or_initial(StringView) const;
    WebIDL::Long get_base_or_anim_value() const;

    GC::Ref<SVGElement> m_element;
    DOM::QualifiedName m_reflected_attribute;
    WebIDL::Long m_initial_value;
    SupportsSecondValue m_supports_second_value;
    ValueRepresented m_value_represented;
};

}
