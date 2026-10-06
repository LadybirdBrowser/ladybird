/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AbstractImageStyleValue.h"
#include <LibWeb/CSS/CSSImageValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#reify-stylevalue
GC::Ref<CSSStyleValue> AbstractImageStyleValue::reify(Utf16FlyString const&) const
{
    // AD-HOC: There's no spec description of how to reify as a CSSImageValue.
    return CSSImageValue::create(*this);
}

void AbstractImageStyleValue::load_any_resources(Layout::NodeWithStyle const& layout_node)
{
    load_any_resources(const_cast<DOM::Document&>(layout_node.document()));
}

ImageStyleValue const* AbstractImageStyleValue::selected_image_style_value() const
{
    if (is_image())
        return &as_image();
    if (is_image_set()) {
        if (auto const* selected_image = as_image_set().selected_image(); selected_image && selected_image->is_image())
            return &selected_image->as_image();
    }
    return nullptr;
}

SizeWithAspectRatio AbstractImageStyleValue::natural_size(HTML::DecodedImageData const& decoded_image_data) const
{
    return {
        .width = decoded_image_data.intrinsic_width(),
        .height = decoded_image_data.intrinsic_height(),
        .aspect_ratio = decoded_image_data.intrinsic_aspect_ratio(),
    };
}

Optional<Painting::ImagePaint> AbstractImageStyleValue::image_paint(Painting::ImagePaintRequest const&) const
{
    if (is_linear_gradient() || is_radial_gradient() || is_conic_gradient())
        return Painting::ImagePaint { Painting::ImagePaint::Gradient { *this } };
    return {};
}

}
