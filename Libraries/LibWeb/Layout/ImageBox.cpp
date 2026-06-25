/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Painting/ImagePaintable.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Layout {

static ImageProvider const& image_provider_for_element(DOM::Element const& element)
{
    if (auto const* image = as_if<HTML::HTMLImageElement>(element))
        return *image;
    if (auto const* input = as_if<HTML::HTMLInputElement>(element))
        return *input;
    if (auto const* object = as_if<HTML::HTMLObjectElement>(element))
        return *object;

    VERIFY_NOT_REACHED();
}

ImageBox::ImageBox(DOM::Document& document, GC::Ptr<DOM::Element> element, CSS::ComputedProperties const& style, ImageProvider const& image_provider)
    : ReplacedBox(document, element, style)
{
    VERIFY(element);
    VERIFY(&image_provider == &image_provider_for_element(*element));
}

ImageBox::ImageBox(DOM::Document& document, GC::Ptr<DOM::Element> element, CSS::ComputedProperties const& style, NonnullOwnPtr<ImageProvider> image_provider)
    : ReplacedBox(document, element, style)
    , m_owned_image_provider(move(image_provider))
{
}

ImageProvider const& ImageBox::image_provider() const
{
    if (m_owned_image_provider)
        return *m_owned_image_provider;

    auto element = dom_node();
    VERIFY(element);

    return image_provider_for_element(*element);
}

ImageBox::~ImageBox() = default;

CSS::SizeWithAspectRatio ImageBox::natural_size() const
{
    auto const& image_provider = this->image_provider();
    if (image_provider.is_image_available()) {
        return {
            .width = image_provider.intrinsic_width(),
            .height = image_provider.intrinsic_height(),
            .aspect_ratio = image_provider.intrinsic_aspect_ratio()
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
    return !image_provider().is_image_available();
}

RefPtr<Painting::Paintable> ImageBox::create_paintable() const
{
    return Painting::ImagePaintable::create(*this);
}

}
