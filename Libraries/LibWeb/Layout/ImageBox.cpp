/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Painting/ImagePaintable.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(ImageBox);

ImageBox::ImageBox(DOM::Document& document, GC::Ptr<DOM::Element> element, GC::Ref<CSS::ComputedProperties> style, ImageProvider const& image_provider)
    : ReplacedBox(document, element, move(style))
    , m_image_provider(image_provider)
{
}

ImageBox::~ImageBox() = default;

void ImageBox::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_image_provider.image_provider_visit_edges(visitor);
}

CSS::SizeWithAspectRatio ImageBox::natural_size() const
{
    if (m_image_provider.is_image_available()) {
        return {
            .width = m_image_provider.intrinsic_width(),
            .height = m_image_provider.intrinsic_height(),
            .aspect_ratio = m_image_provider.intrinsic_aspect_ratio()
        };
    }

    String alt;
    if (auto element = dom_node())
        alt = element->get_attribute_value(HTML::AttributeNames::alt);
    if (alt.is_empty())
        return { 0, 0, {} };

    auto font = Platform::FontPlugin::the().default_font(12);
    CSSPixels alt_text_width = m_cached_alt_text_width.ensure([&] {
        return CSSPixels::nearest_value_for(font->width(Utf16String::from_utf8(alt)));
    });
    auto width = alt_text_width + 16;
    auto height = CSSPixels::nearest_value_for(font->pixel_size()) + 16;

    Optional<CSSPixelFraction> aspect_ratio;
    if (height > 0)
        aspect_ratio = CSSPixelFraction(width, height);

    return { width, height, aspect_ratio };
}

void ImageBox::dom_node_did_update_alt_text(Badge<ImageProvider>)
{
    m_cached_alt_text_width = {};
}

bool ImageBox::renders_as_alt_text() const
{
    return !m_image_provider.is_image_available();
}

GC::Ptr<Painting::Paintable> ImageBox::create_paintable() const
{
    return Painting::ImagePaintable::create(*this);
}

}
