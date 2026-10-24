/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedEnumeration
class SVGAnimatedEnumeration final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGAnimatedEnumeration, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGAnimatedEnumeration);

public:
    // A keyword of the reflected attribute, and the numeric type value it maps to.
    struct Keyword {
        StringView keyword;
        u16 value;
    };

    [[nodiscard]] static GC::Ref<SVGAnimatedEnumeration> create(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, ReadonlySpan<Keyword> keywords, u16 initial_value);
    virtual ~SVGAnimatedEnumeration() override;

    u16 base_val() const { return base_or_anim_value(); }
    WebIDL::ExceptionOr<void> set_base_val(u16);

    u16 anim_val() const { return base_or_anim_value(); }

private:
    SVGAnimatedEnumeration(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, ReadonlySpan<Keyword> keywords, u16 initial_value);

    virtual void visit_edges(Cell::Visitor&) override;

    u16 base_or_anim_value() const;

    GC::Ref<SVGElement> m_element;
    Utf16FlyString m_reflected_attribute;
    ReadonlySpan<Keyword> m_keywords;
    u16 m_initial_value { 0 };
};

}
