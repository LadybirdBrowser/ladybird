/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/SVG/SVGLength.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG11/types.html#InterfaceSVGAnimatedLength
class SVGAnimatedLength final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGAnimatedLength, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGAnimatedLength);

public:
    [[nodiscard]] static GC::Ref<SVGAnimatedLength> create(JS::Realm&, GC::Ref<SVGLength> base_val, GC::Ref<SVGLength> anim_val);
    virtual ~SVGAnimatedLength() override;

    GC::Ref<SVGLength> base_val() const { return m_base_val; }
    GC::Ref<SVGLength> anim_val() const { return m_anim_val; }

private:
    SVGAnimatedLength(JS::Realm&, GC::Ref<SVGLength> base_val, GC::Ref<SVGLength> anim_val);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<SVGLength> m_base_val;
    GC::Ref<SVGLength> m_anim_val;
};

}
