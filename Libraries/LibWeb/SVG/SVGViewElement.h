/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGFitToViewBox.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGViewElement final : public SVGGraphicsElement
    , public SVGFitToViewBox {
    WEB_PLATFORM_OBJECT(SVGViewElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGViewElement);

public:
    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;

private:
    SVGViewElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    virtual bool is_svg_view_element() const override { return true; }

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGViewElement>() const { return is_svg_view_element(); }

}
