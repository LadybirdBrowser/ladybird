/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGImageElement
    : public SVGGraphicsElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes>
    , public Layout::ImageProvider {
    WEB_PLATFORM_OBJECT(SVGImageElement, SVGGraphicsElement);

public:
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    GC::Ref<SVG::SVGAnimatedLength> x();
    GC::Ref<SVG::SVGAnimatedLength> y();
    GC::Ref<SVG::SVGAnimatedLength> width();
    GC::Ref<SVG::SVGAnimatedLength> height();

    Gfx::Rect<CSSPixels> bounding_box() const;

    // ^Layout::ImageProvider
    virtual bool is_image_available() const override;
    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;
    virtual RefPtr<Gfx::ImmutableBitmap> current_image_bitmap(Gfx::IntSize = {}) const override;
    virtual void set_visible_in_viewport(bool) override { }
    virtual GC::Ref<DOM::Element const> to_html_element() const override { return *this; }

protected:
    SVGImageElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void process_the_url(Optional<String> const& href);
    void fetch_the_document(URL::URL const& url);

private:
    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;
    void animate();

    GC::Ptr<SVG::SVGAnimatedLength> m_x;
    GC::Ptr<SVG::SVGAnimatedLength> m_y;
    GC::Ptr<SVG::SVGAnimatedLength> m_width;
    GC::Ptr<SVG::SVGAnimatedLength> m_height;

    RefPtr<Core::Timer> m_animation_timer;
    size_t m_current_frame_index { 0 };
    size_t m_loops_completed { 0 };

    Optional<URL::URL> m_href;

    GC::Ptr<HTML::SharedResourceRequest> m_resource_request;
    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;
};

}
