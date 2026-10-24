/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGPointList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGPointList);

GC::Ref<SVGPointList> SVGPointList::create(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGPointList>(element, move(reflected_attribute), read_only);
}

SVGPointList::SVGPointList(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, ReadOnlyList read_only)
    : SVGList(read_only)
    , m_element(element)
    , m_reflected_attribute(move(reflected_attribute))
{
    synchronize();
}

void SVGPointList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
    visitor.visit(m_element);
}

GC::Ref<Geometry::DOMPoint> SVGPointList::copy_if_not_detached(GC::Ref<Geometry::DOMPoint> point)
{
    // FIXME: A DOMPoint does not know which list it is an element of, so only a point that is an element of this list
    //        is known not to be detached. Also copy a point that is an element of another list.
    if (!items().contains_slow(point))
        return point;

    // NB: The value of a point in a list is its x and y coordinates.
    return Geometry::DOMPoint::create(point->x(), point->y());
}

// https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
void SVGPointList::synchronize()
{
    // NB: The list does not need to be synchronized with a value that it has just reserialized itself.
    if (m_is_reserializing)
        return;

    // 1. Let value be the base value of the reflected content attribute (using the attribute's initial value if it is
    //    not present or invalid).
    auto value = parse_points(m_element->get_attribute_value(m_reflected_attribute));

    // FIXME: DOMPoint objects are not associated with the list they are an element of, so modifying one does not
    //        reserialize the reflected attribute, and a point of a list that is read only can still be modified.
    synchronize_items(value, [] { return Geometry::DOMPoint::create(); }, [](GC::Ref<Geometry::DOMPoint> const& item, Gfx::FloatPoint const& point) { item->set_x_and_y(point.x(), point.y()); });
}

// https://svgwg.org/svg2-draft/types.html#TermReserialize
void SVGPointList::reserialize_reflected_attribute()
{
    // 1. Let elements be the list of values in value.
    // 2. Let serialized elements be a list of strings, where each string is formed based on the corresponding value in
    //    elements and its type:
    //    a DOMPoint object: The string value is computed as follows:
    //    1. Let string be an empty string.
    //    2. Let x and y be the values of the DOMPoint object's x and y coordinates, respectively.
    //    3. Append to string an implementation specific string that, if parsed as <number> using CSS syntax, would
    //       return the number value closest to x, given the implementation's supported real number precision.
    //    4. Append a single U+002C COMMA character to string.
    //    5. Append to string an implementation specific string that, if parsed as <number> using CSS syntax, would
    //       return the number value closest to y, given the implementation's supported real number precision.
    //    6. The string is string.
    // 3. Set the content attribute to a string consisting of the strings in serialized elements joined and separated by
    //    single U+0020 SPACE characters.
    // FIXME: The spec does not say how to serialize a coordinate that is not finite.
    Utf16StringBuilder builder;
    for (size_t index = 0; index < items().size(); ++index) {
        if (index > 0)
            builder.append_code_unit(' ');
        CSS::serialize_a_number(builder, items()[index]->x());
        builder.append_code_unit(',');
        CSS::serialize_a_number(builder, items()[index]->y());
    }

    TemporaryChange change { m_is_reserializing, true };
    m_element->set_attribute_value(m_reflected_attribute, builder.to_string());
}

// https://svgwg.org/svg2-draft/shapes.html#__svg__SVGAnimatedPoints__points
GC::Ref<SVGPointList> SVGAnimatedPoints::points_for_bindings()
{
    // On getting points, an SVGPointList object is returned that reflects the base value of the reflected attribute.
    if (!m_points_list)
        m_points_list = SVGPointList::create(svg_animated_points_element(), AttributeNames::points, ReadOnlyList::No);
    return *m_points_list;
}

// https://svgwg.org/svg2-draft/shapes.html#__svg__SVGAnimatedPoints__animatedPoints
GC::Ref<SVGPointList> SVGAnimatedPoints::animated_points()
{
    // On getting animatedPoints, an SVGPointList object is returned that reflects the animated value of the reflected
    // attribute.
    // NB: List interface objects reflected through the animVal IDL attribute are always read only.
    // FIXME: Reflect the animated value once SMIL animation can affect it.
    if (!m_animated_points_list)
        m_animated_points_list = SVGPointList::create(svg_animated_points_element(), AttributeNames::points, ReadOnlyList::Yes);
    return *m_animated_points_list;
}

void SVGAnimatedPoints::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_points_list);
    visitor.visit(m_animated_points_list);
}

void SVGAnimatedPoints::attribute_changed(Utf16FlyString const& name, Optional<Utf16FlyString> const& namespace_)
{
    // https://svgwg.org/svg2-draft/types.html#SynchronizingReflectedValues
    if (namespace_.has_value() || name != AttributeNames::points)
        return;
    if (m_points_list)
        m_points_list->synchronize();
    if (m_animated_points_list)
        m_animated_points_list->synchronize();
}

}
