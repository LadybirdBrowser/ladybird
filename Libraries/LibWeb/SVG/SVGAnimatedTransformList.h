/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/SVGAnimatedTransformList.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGTransformList.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/single-page.html#coords-InterfaceSVGTransformList
class SVGAnimatedTransformList final : public Bindings::Wrappable {
    WEB_WRAPPABLE(SVGAnimatedTransformList, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SVGAnimatedTransformList);

public:
    [[nodiscard]] static GC::Ref<SVGAnimatedTransformList> create(GC::Ref<SVGTransformList> base_val, GC::Ref<SVGTransformList> anim_val);
    virtual ~SVGAnimatedTransformList() override = default;

    GC::Ref<SVGTransformList> base_val() const
    {
        return m_base_val;
    }

    GC::Ref<SVGTransformList> anim_val() const
    {
        return m_anim_val;
    }

private:
    SVGAnimatedTransformList(GC::Ref<SVGTransformList> base_val, GC::Ref<SVGTransformList> anim_val);

    virtual void visit_edges(Cell::Visitor& visitor) override;

    GC::Ref<SVGTransformList> m_base_val;
    GC::Ref<SVGTransformList> m_anim_val;
};

}
