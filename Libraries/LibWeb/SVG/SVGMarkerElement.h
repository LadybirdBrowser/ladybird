/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedAngle.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFitToViewBox.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/painting.html#InterfaceSVGMarkerElement
class SVGMarkerElement final
    : public SVGElement
    , public SVGFitToViewBox {
    WEB_WRAPPABLE(SVGMarkerElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGMarkerElement);

public:
    static constexpr u16 SVG_MARKERUNITS_UNKNOWN = 0;
    static constexpr u16 SVG_MARKERUNITS_USERSPACEONUSE = 1;
    static constexpr u16 SVG_MARKERUNITS_STROKEWIDTH = 2;

    static constexpr u16 SVG_MARKER_ORIENT_UNKNOWN = 0;
    static constexpr u16 SVG_MARKER_ORIENT_AUTO = 1;
    static constexpr u16 SVG_MARKER_ORIENT_ANGLE = 2;
    static constexpr u16 SVG_MARKER_ORIENT_AUTO_START_REVERSE = 3;

    virtual ~SVGMarkerElement() override = default;

    // https://svgwg.org/svg2-draft/painting.html#__svg__SVGMarkerElement__refX
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE_WITH_GETTER(refX, ref_x, Horizontal, SVGLengthValue::number(0));

    // https://svgwg.org/svg2-draft/painting.html#__svg__SVGMarkerElement__refY
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE_WITH_GETTER(refY, ref_y, Vertical, SVGLengthValue::number(0));

    // https://svgwg.org/svg2-draft/painting.html#__svg__SVGMarkerElement__markerWidth
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE_WITH_GETTER(markerWidth, marker_width, Horizontal, SVGLengthValue::number(3));

    // https://svgwg.org/svg2-draft/painting.html#__svg__SVGMarkerElement__markerHeight
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE_WITH_GETTER(markerHeight, marker_height, Vertical, SVGLengthValue::number(3));

    GC::Ref<SVGAnimatedEnumeration> marker_units();
    GC::Ref<SVGAnimatedEnumeration> orient_type();
    GC::Ref<SVGAnimatedAngle> orient_angle();

    void set_orient_to_auto();
    void set_orient_to_angle(GC::Ref<SVGAngle>);

private:
    SVGMarkerElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize_element() override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    GC::Ptr<SVGAnimatedEnumeration> m_orient_type;
    GC::Ptr<SVGAnimatedAngle> m_orient_angle;
};

}
