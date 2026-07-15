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

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    GC::Ref<SVG::SVGAnimatedLength> x();
    GC::Ref<SVG::SVGAnimatedLength> y();
    GC::Ref<SVG::SVGAnimatedLength> width();
    GC::Ref<SVG::SVGAnimatedLength> height();

    Gfx::FloatRect bounding_box(CSSPixelSize viewport_size) const;

    // ^Layout::ImageProvider
    virtual GC::Ptr<HTML::DecodedImageData> decoded_image_data() const override;

protected:
    SVGImageElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void adopted_from(DOM::Document&) override;

    void process_the_url(Optional<Utf16String> const& href);
    void fetch_the_document(URL::URL const& url);

private:
    virtual void finalize() override;

    virtual bool is_svg_image_element() const override { return true; }

    virtual RefPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::ComputedValues const>) override;
    virtual void decoded_image_data_did_update() override { set_needs_repaint(); }

    Optional<NumberPercentage> m_x;
    Optional<NumberPercentage> m_y;
    Optional<NumberPercentage> m_width;
    Optional<NumberPercentage> m_height;

    Optional<URL::URL> m_href;

    GC::Ptr<HTML::SharedResourceRequest> m_resource_request;
    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGImageElement>() const { return is_svg_image_element(); }

}

namespace JS {

template<>
inline bool Object::fast_is<Web::SVG::SVGImageElement>() const
{
    return is_dom_node() && static_cast<Web::DOM::Node const&>(*this).is_svg_image_element();
}

}
