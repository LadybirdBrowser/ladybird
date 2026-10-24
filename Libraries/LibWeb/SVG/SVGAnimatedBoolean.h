/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedBoolean
class SVGAnimatedBoolean final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGAnimatedBoolean, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGAnimatedBoolean);

public:
    [[nodiscard]] static GC::Ref<SVGAnimatedBoolean> create(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, bool initial_value);
    virtual ~SVGAnimatedBoolean() override;

    bool base_val() const { return base_or_anim_value(); }
    void set_base_val(bool);

    bool anim_val() const { return base_or_anim_value(); }

private:
    SVGAnimatedBoolean(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, bool initial_value);

    virtual void visit_edges(Cell::Visitor&) override;

    bool base_or_anim_value() const;

    GC::Ref<SVGElement> m_element;
    Utf16FlyString m_reflected_attribute;
    bool m_initial_value { false };
};

}
