/*
 * Copyright (c) 2020, Matthew Olsson <matthewcolsson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Bitmap.h>
#include <LibWeb/DOM/NonElementParentNode.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGTransform.h>
#include <LibWeb/SVG/SVGViewport.h>
#include <LibWeb/SVG/ViewBox.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::SVG {

class SVGSVGElement final : public SVGGraphicsElement
    , public SVGViewport
    // SVGSVGElement is not strictly a NonElementParentNode, but it implements the same get_element_by_id() method.
    , public DOM::NonElementParentNode<SVGSVGElement> {
    WEB_PLATFORM_OBJECT(SVGSVGElement, SVGGraphicsElement);
    JS_DECLARE_ALLOCATOR(SVGSVGElement);

public:
    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>) override;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;

    virtual bool requires_svg_container() const override { return false; }
    virtual bool is_svg_container() const override { return true; }

    virtual Optional<ViewBox> view_box() const override;
    virtual Optional<PreserveAspectRatio> preserve_aspect_ratio() const override { return m_preserve_aspect_ratio; }

    void set_fallback_view_box_for_svg_as_image(Optional<ViewBox>);

    JS::NonnullGCPtr<SVGAnimatedRect> view_box_for_bindings() { return *m_view_box_for_bindings; }

    JS::NonnullGCPtr<SVGAnimatedLength> x() const;
    JS::NonnullGCPtr<SVGAnimatedLength> y() const;
    JS::NonnullGCPtr<SVGAnimatedLength> width() const;
    JS::NonnullGCPtr<SVGAnimatedLength> height() const;

    float current_scale() const;
    void set_current_scale(float);

    JS::NonnullGCPtr<Geometry::DOMPointReadOnly> current_translate() const;

    JS::NonnullGCPtr<DOM::NodeList> get_intersection_list(JS::NonnullGCPtr<Geometry::DOMRectReadOnly> rect, JS::GCPtr<SVGElement> reference_element) const;
    JS::NonnullGCPtr<DOM::NodeList> get_enclosure_list(JS::NonnullGCPtr<Geometry::DOMRectReadOnly> rect, JS::GCPtr<SVGElement> reference_element) const;
    bool check_intersection(JS::NonnullGCPtr<SVGElement> element, JS::NonnullGCPtr<Geometry::DOMRectReadOnly> rect) const;
    bool check_enclosure(JS::NonnullGCPtr<SVGElement> element, JS::NonnullGCPtr<Geometry::DOMRectReadOnly> rect) const;

    void deselect_all() const;

    JS::NonnullGCPtr<SVGLength> create_svg_length() const;
    JS::NonnullGCPtr<Geometry::DOMPoint> create_svg_point() const;
    JS::NonnullGCPtr<Geometry::DOMMatrix> create_svg_matrix() const;
    JS::NonnullGCPtr<Geometry::DOMRect> create_svg_rect() const;
    JS::NonnullGCPtr<SVGTransform> create_svg_transform() const;

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

    [[nodiscard]] RefPtr<CSS::CSSStyleValue> width_style_value_from_attribute() const;
    [[nodiscard]] RefPtr<CSS::CSSStyleValue> height_style_value_from_attribute() const;

private:
    SVGSVGElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    virtual bool is_svg_svg_element() const override { return true; }

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    void update_fallback_view_box_for_svg_as_image();

    Optional<ViewBox> m_view_box;
    Optional<PreserveAspectRatio> m_preserve_aspect_ratio;

    Optional<ViewBox> m_fallback_view_box_for_svg_as_image;

    JS::GCPtr<SVGAnimatedRect> m_view_box_for_bindings;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGSVGElement>() const { return is_svg_svg_element(); }

}
