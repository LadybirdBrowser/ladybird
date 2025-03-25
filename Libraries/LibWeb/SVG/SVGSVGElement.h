/*
 * Copyright (c) 2020, Matthew Olsson <matthewcolsson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Bitmap.h>
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
    , public SVGViewport {
    WEB_PLATFORM_OBJECT(SVGSVGElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGSVGElement);

public:
    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;

    virtual bool requires_svg_container() const override { return false; }
    virtual bool is_svg_container() const override { return true; }

    virtual Optional<ViewBox> view_box() const override;
    virtual Optional<PreserveAspectRatio> preserve_aspect_ratio() const override { return m_preserve_aspect_ratio; }

    void set_fallback_view_box_for_svg_as_image(Optional<ViewBox>);

    GC::Ref<SVGAnimatedRect> view_box_for_bindings() { return *m_view_box_for_bindings; }

    GC::Ref<SVGAnimatedLength> x() const;
    GC::Ref<SVGAnimatedLength> y() const;
    GC::Ref<SVGAnimatedLength> width() const;
    GC::Ref<SVGAnimatedLength> height() const;

    float current_scale() const;
    void set_current_scale(float);

    GC::Ref<Geometry::DOMPointReadOnly> current_translate() const;

    GC::Ref<DOM::NodeList> get_intersection_list(GC::Ref<Geometry::DOMRectReadOnly> rect, GC::Ptr<SVGElement> reference_element) const;
    GC::Ref<DOM::NodeList> get_enclosure_list(GC::Ref<Geometry::DOMRectReadOnly> rect, GC::Ptr<SVGElement> reference_element) const;
    bool check_intersection(GC::Ref<SVGElement> element, GC::Ref<Geometry::DOMRectReadOnly> rect) const;
    bool check_enclosure(GC::Ref<SVGElement> element, GC::Ref<Geometry::DOMRectReadOnly> rect) const;

    void deselect_all() const;

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

    [[nodiscard]] RefPtr<CSS::CSSStyleValue> width_style_value_from_attribute() const;
    [[nodiscard]] RefPtr<CSS::CSSStyleValue> height_style_value_from_attribute() const;

    struct NaturalMetrics {
        Optional<CSSPixels> width;
        Optional<CSSPixels> height;
        Optional<CSSPixelFraction> aspect_ratio;
    };

    static NaturalMetrics negotiate_natural_metrics(SVGSVGElement const&);

private:
    SVGSVGElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    virtual bool is_svg_svg_element() const override { return true; }

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    void update_fallback_view_box_for_svg_as_image();

    Optional<ViewBox> m_view_box;
    Optional<PreserveAspectRatio> m_preserve_aspect_ratio;

    Optional<ViewBox> m_fallback_view_box_for_svg_as_image;

    GC::Ptr<SVGAnimatedRect> m_view_box_for_bindings;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGSVGElement>() const { return is_svg_svg_element(); }

}
