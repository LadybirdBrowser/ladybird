/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGImageElement final
    : public SVGGraphicsElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes>
    , public Layout::ImageProvider
    , public HTML::DecodedImageData::Client {
    WEB_PLATFORM_OBJECT(SVGImageElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGImageElement);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~SVGImageElement() override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    GC::Ref<SVG::SVGAnimatedLength> x();
    GC::Ref<SVG::SVGAnimatedLength> y();
    GC::Ref<SVG::SVGAnimatedLength> width();
    GC::Ref<SVG::SVGAnimatedLength> height();

    Gfx::FloatRect bounding_box() const;

    // ^Layout::ImageProvider
    virtual GC::Ptr<HTML::DecodedImageData> decoded_image_data() const override;

protected:
    SVGImageElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void adopted_from(DOM::Document&) override;

    void process_the_url(Optional<String> const& href);
    void fetch_the_document(URL::URL const& url);

private:
    virtual void finalize() override;

    virtual RefPtr<Layout::Node> create_layout_node(CSS::ComputedProperties const&) override;
    virtual void decoded_image_data_did_update() override { set_needs_repaint(); }

    GC::Ptr<SVG::SVGAnimatedLength> m_x;
    GC::Ptr<SVG::SVGAnimatedLength> m_y;
    GC::Ptr<SVG::SVGAnimatedLength> m_width;
    GC::Ptr<SVG::SVGAnimatedLength> m_height;

    Optional<URL::URL> m_href;

    GC::Ptr<HTML::SharedResourceRequest> m_resource_request;
    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;
};

}
