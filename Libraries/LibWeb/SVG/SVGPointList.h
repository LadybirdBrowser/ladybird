/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/SVG/SVGList.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/shapes.html#InterfaceSVGPointList
class SVGPointList final
    : public Bindings::GCAllocatedWrappable
    , public SVGList<GC::Ref<Geometry::DOMPoint>> {
    WEB_WRAPPABLE(SVGPointList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGPointList);

public:
    [[nodiscard]] static GC::Ref<SVGPointList> create(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, ReadOnlyList);
    virtual ~SVGPointList() override = default;

    // https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
    void synchronize();

    // https://svgwg.org/svg2-draft/types.html#TermReserialize
    virtual void reserialize_reflected_attribute() override;

private:
    SVGPointList(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, ReadOnlyList);

    virtual void visit_edges(Visitor&) override;

    virtual GC::Ref<Geometry::DOMPoint> copy_if_not_detached(GC::Ref<Geometry::DOMPoint>) override;

    GC::Ref<SVGElement> m_element;
    Utf16FlyString m_reflected_attribute;
    bool m_is_reserializing { false };
};

// https://svgwg.org/svg2-draft/shapes.html#InterfaceSVGAnimatedPoints
class SVGAnimatedPoints {
public:
    virtual ~SVGAnimatedPoints() = default;

    GC::Ref<SVGPointList> points_for_bindings();
    GC::Ref<SVGPointList> animated_points();

protected:
    void visit_edges(GC::Cell::Visitor&);
    void attribute_changed(Utf16FlyString const& name, Optional<Utf16FlyString> const& namespace_);

    virtual SVGElement& svg_animated_points_element() = 0;

private:
    GC::Ptr<SVGPointList> m_points_list;
    GC::Ptr<SVGPointList> m_animated_points_list;
};

}
