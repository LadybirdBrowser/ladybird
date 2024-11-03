/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
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
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    JS::NonnullGCPtr<SVG::SVGAnimatedLength> x();
    JS::NonnullGCPtr<SVG::SVGAnimatedLength> y();
    JS::NonnullGCPtr<SVG::SVGAnimatedLength> width();
    JS::NonnullGCPtr<SVG::SVGAnimatedLength> height();

    Gfx::Rect<CSSPixels> bounding_box() const;

    // FIXME: This is a hack for images used as CanvasImageSource. Do something more elegant.
    RefPtr<Gfx::Bitmap> bitmap() const
    {
        auto bitmap = current_image_bitmap();
        if (!bitmap)
            return nullptr;
        return bitmap->bitmap();
    }

    // ^Layout::ImageProvider
    virtual bool is_image_available() const override;
    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;
    virtual RefPtr<Gfx::ImmutableBitmap> current_image_bitmap(Gfx::IntSize = {}) const override;
    virtual void set_visible_in_viewport(bool) override { }
    virtual JS::NonnullGCPtr<DOM::Element const> to_html_element() const override { return *this; }

protected:
    SVGImageElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void process_the_url(Optional<String> const& href);
    void fetch_the_document(URL::URL const& url);

private:
    virtual JS::GCPtr<Layout::Node> create_layout_node(CSS::StyleProperties) override;
    void animate();

    JS::GCPtr<SVG::SVGAnimatedLength> m_x;
    JS::GCPtr<SVG::SVGAnimatedLength> m_y;
    JS::GCPtr<SVG::SVGAnimatedLength> m_width;
    JS::GCPtr<SVG::SVGAnimatedLength> m_height;

    RefPtr<Core::Timer> m_animation_timer;
    size_t m_current_frame_index { 0 };
    size_t m_loops_completed { 0 };

    URL::URL m_href;

    JS::GCPtr<HTML::SharedResourceRequest> m_resource_request;
    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;
};

}
