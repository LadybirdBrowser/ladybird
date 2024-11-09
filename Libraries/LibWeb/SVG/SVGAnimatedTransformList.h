/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/SVG/SVGTransformList.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/single-page.html#coords-InterfaceSVGTransformList
class SVGAnimatedTransformList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGAnimatedTransformList, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(SVGAnimatedTransformList);

public:
    [[nodiscard]] static JS::NonnullGCPtr<SVGAnimatedTransformList> create(JS::Realm& realm, JS::NonnullGCPtr<SVGTransformList> base_val, JS::NonnullGCPtr<SVGTransformList> anim_val);
    virtual ~SVGAnimatedTransformList() override;

    JS::NonnullGCPtr<SVGTransformList> base_val() const
    {
        return m_anim_val;
    }

    JS::NonnullGCPtr<SVGTransformList> anim_val() const
    {
        return m_anim_val;
    }

private:
    SVGAnimatedTransformList(JS::Realm& realm, JS::NonnullGCPtr<SVGTransformList> base_val, JS::NonnullGCPtr<SVGTransformList> anim_val);

    virtual void initialize(JS::Realm& realm) override;
    virtual void visit_edges(Cell::Visitor& visitor) override;

    JS::NonnullGCPtr<SVGTransformList> m_base_val;
    JS::NonnullGCPtr<SVGTransformList> m_anim_val;
};

}
