/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Geometry/DOMRect.h>

namespace Web::SVG {

class SVGAnimatedRect final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGAnimatedRect, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGAnimatedRect);

public:
    virtual ~SVGAnimatedRect();

    GC::Ptr<Geometry::DOMRect> base_val() const;
    GC::Ptr<Geometry::DOMRect> anim_val() const;

    void set_base_val(Gfx::DoubleRect const&);
    void set_anim_val(Gfx::DoubleRect const&);

    void set_nulled(bool);

private:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    explicit SVGAnimatedRect(JS::Realm&);

    GC::Ptr<Geometry::DOMRect> m_base_val;
    GC::Ptr<Geometry::DOMRect> m_anim_val;

    bool m_nulled { true };
};

}
