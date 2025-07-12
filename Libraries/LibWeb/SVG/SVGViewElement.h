/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGViewport.h>
#include <LibWeb/SVG/ViewBox.h>

namespace Web::SVG {

class SVGViewElement final : public SVGGraphicsElement
    , public SVGViewport {
    WEB_PLATFORM_OBJECT(SVGViewElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGViewElement);

public:
    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;

    virtual Optional<ViewBox> view_box() const override { return m_view_box; }
    virtual Optional<PreserveAspectRatio> preserve_aspect_ratio() const override { return m_preserve_aspect_ratio; }

    GC::Ref<SVGAnimatedRect> view_box_for_bindings() { return *m_view_box_for_bindings; }

private:
    SVGViewElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    virtual bool is_svg_view_element() const override { return true; }

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    Optional<ViewBox> m_view_box;
    Optional<PreserveAspectRatio> m_preserve_aspect_ratio;
    GC::Ptr<SVGAnimatedRect> m_view_box_for_bindings;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGViewElement>() const { return is_svg_view_element(); }

}
