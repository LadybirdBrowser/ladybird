/*
 * Copyright (c) 2020, Matthew Olsson <matthewcolsson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGFitToViewBox.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGTransform.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::SVG {

class SVGSVGElement final : public SVGGraphicsElement
    , public SVGFitToViewBox {
    WEB_WRAPPABLE(SVGSVGElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGSVGElement);

public:
    virtual RefPtr<Layout::Node> create_layout_node(CSS::LayoutStyle) override;

    virtual bool requires_svg_container() const override { return false; }
    virtual bool is_svg_container() const override { return true; }

    virtual Optional<ViewBox> active_view_box() const override;

    void set_active_view_element(GC::Ptr<SVGViewElement> view_element) { m_active_view_element = view_element; }

    void set_fallback_view_box_for_svg_as_image(Optional<ViewBox>);

    // AD-HOC: The spec states that the x, y, width and height IDL attributes reflect the respective computed values and their
    //         corresponding presentation attributes but other browsers reflect the attribute values instead - see
    //         https://github.com/w3c/svgwg/issues/1153

    // https://w3c.github.io/svgwg/svg2-draft/struct.html#__svg__SVGSVGElement__x
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/struct.html#__svg__SVGSVGElement__y
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y, Vertical, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/struct.html#__svg__SVGSVGElement__width
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(width, Horizontal, SVGLengthValue::percentage(100));

    // https://w3c.github.io/svgwg/svg2-draft/struct.html#__svg__SVGSVGElement__height
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(height, Vertical, SVGLengthValue::percentage(100));

    float current_scale() const;
    void set_current_scale(float);

    GC::Ref<Geometry::DOMPointReadOnly> current_translate() const;

    GC::Ref<DOM::NodeList> get_intersection_list(GC::Ref<Geometry::DOMRectReadOnly> rect, GC::Ptr<SVGElement> reference_element) const;
    GC::Ref<DOM::NodeList> get_enclosure_list(GC::Ref<Geometry::DOMRectReadOnly> rect, GC::Ptr<SVGElement> reference_element) const;
    bool check_intersection(GC::Ref<SVGElement> element, GC::Ref<Geometry::DOMRectReadOnly> rect) const;
    bool check_enclosure(GC::Ref<SVGElement> element, GC::Ref<Geometry::DOMRectReadOnly> rect) const;

    void deselect_all() const;

    GC::Ref<SVGNumber> create_svg_number() const;
    GC::Ref<SVGLength> create_svg_length() const;
    GC::Ref<Geometry::DOMPoint> create_svg_point() const;
    GC::Ref<Geometry::DOMMatrix> create_svg_matrix() const;
    GC::Ref<Geometry::DOMRect> create_svg_rect() const;
    GC::Ref<SVGTransform> create_svg_transform() const;

    // Deprecated methods that have no effect when called, but which are kept for compatibility reasons.
    WebIDL::UnsignedLong suspend_redraw(WebIDL::UnsignedLong max_wait_milliseconds) const
    {
        (void)max_wait_milliseconds;
        // When the suspendRedraw method is called, it must return 1.
        return 1;
    }
    void unsuspend_redraw(WebIDL::UnsignedLong suspend_handle_id) const
    {
        (void)suspend_handle_id;
    }
    void unsuspend_redraw_all() const { }
    void force_redraw() const { }

    // The width/height attributes parsed as the CSS width/height properties, when that yields a
    // plain <length> - the only case the natural size negotiation can use.
    [[nodiscard]] Optional<CSS::Length> width_attribute_length() const;
    [[nodiscard]] Optional<CSS::Length> height_attribute_length() const;

    static CSS::SizeWithAspectRatio negotiate_natural_metrics(SVGSVGElement const&, CSS::Length::ResolutionContext const&);

private:
    SVGSVGElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize_element() override;
    virtual void visit_edges(Visitor&) override;

    virtual bool is_svg_svg_element() const override { return true; }

    GC::Ptr<SVGViewElement> active_view_element() const { return m_active_view_element; }

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
    virtual void children_changed(ChildrenChangedMetadata const&) override;

    void update_fallback_view_box_for_svg_as_image();

    Optional<ViewBox> m_fallback_view_box_for_svg_as_image;

    mutable Optional<Optional<CSS::Length>> m_cached_width_attribute_length;
    mutable Optional<Optional<CSS::Length>> m_cached_height_attribute_length;

    GC::Ptr<SVGViewElement> m_active_view_element;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGSVGElement>() const { return is_svg_svg_element(); }

}
