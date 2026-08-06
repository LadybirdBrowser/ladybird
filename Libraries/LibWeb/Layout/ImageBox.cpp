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

ImageBox::ImageBox(DOM::Document& document, GC::Ptr<DOM::Element> element, NonnullRefPtr<CSS::ComputedValues const> style, ImageProvider const& image_provider)
    : ReplacedBox(document, element, style)
{
    VERIFY(element);
    VERIFY(&image_provider == &image_provider_for_element(*element));
}

ImageBox::ImageBox(DOM::Document& document, GC::Ptr<DOM::Element> element, NonnullRefPtr<CSS::ComputedValues const> style, NonnullOwnPtr<ImageProvider> image_provider)
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

    return { 0, 0, {} };
}

RefPtr<Painting::Paintable> ImageBox::create_paintable() const
{
    return Painting::ImagePaintable::create(*this);
}

}
