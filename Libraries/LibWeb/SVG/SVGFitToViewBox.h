/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedString.h>

namespace Web::SVG {

class SVGFitToViewBox {
public:
    virtual ~SVGFitToViewBox() = default;

    GC::Ref<SVGAnimatedRect> view_box_for_bindings() const { return *m_view_box_for_bindings; }
    Optional<ViewBox> view_box() const { return m_view_box; }
    Optional<PreserveAspectRatio> preserve_aspect_ratio() const { return m_preserve_aspect_ratio; }

protected:
    void initialize(JS::Realm&);
    void visit_edges(JS::Cell::Visitor&);
    void attribute_changed(DOM::Element& element, FlyString const& name, Optional<String> const& value);

private:
    Optional<ViewBox> m_view_box;
    GC::Ptr<SVGAnimatedRect> m_view_box_for_bindings;
    Optional<PreserveAspectRatio> m_preserve_aspect_ratio;
};

}
