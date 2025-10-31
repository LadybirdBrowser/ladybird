/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedNumber
class SVGAnimatedNumber final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGAnimatedNumber, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGAnimatedNumber);

public:
    enum class SupportsSecondValue : u8 {
        Yes,
        No,
    };
    enum class ValueRepresented : u8 {
        First,
        Second,
    };

    [[nodiscard]] static GC::Ref<SVGAnimatedNumber> create(JS::Realm&, GC::Ref<SVGElement>,
        DOM::QualifiedName reflected_attribute, float initial_value, SupportsSecondValue = SupportsSecondValue::No,
        ValueRepresented = ValueRepresented::First);
    virtual ~SVGAnimatedNumber() override;

    float base_val() const;
    void set_base_val(float);

    float anim_val() const;

private:
    SVGAnimatedNumber(JS::Realm&, GC::Ref<SVGElement>, DOM::QualifiedName, float, SupportsSecondValue, ValueRepresented);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    float parse_value_or_initial(StringView) const;
    float get_base_or_anim_value() const;

    GC::Ref<SVGElement> m_element;
    DOM::QualifiedName m_reflected_attribute;
    float m_initial_value;
    SupportsSecondValue m_supports_second_value;
    ValueRepresented m_value_represented;
};

}
