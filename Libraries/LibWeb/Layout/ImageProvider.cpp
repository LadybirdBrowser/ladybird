/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintFacts.h>

namespace Web::Layout {

Optional<CSSPixels> ImageProvider::intrinsic_width() const
{
    if (auto const& data = decoded_image_data())
        return data->intrinsic_width();
    return {};
}

Optional<CSSPixels> ImageProvider::intrinsic_height() const
{
    if (auto const& data = decoded_image_data())
        return data->intrinsic_height();
    return {};
}

Optional<CSSPixelFraction> ImageProvider::intrinsic_aspect_ratio() const
{
    if (auto const& data = decoded_image_data())
        return data->intrinsic_aspect_ratio();
    return {};
}

Optional<CSSPixelSize> ImageProvider::intrinsic_size() const
{
    auto width = intrinsic_width();
    auto height = intrinsic_height();
    if (!width.has_value() || !height.has_value())
        return {};

    return CSSPixelSize { *width, *height };
}

Optional<Gfx::DecodedImageFrame> ImageProvider::current_image_frame(Optional<Gfx::IntSize> size) const
{
    if (auto const& data = decoded_image_data())
        return data->current_frame(size.value_or(intrinsic_size().value_or({}).to_type<int>()));
    return {};
}

Optional<Gfx::DecodedImageFrame> ImageProvider::default_image_frame(Optional<Gfx::IntSize> size) const
{
    if (auto const& data = decoded_image_data())
        return data->default_frame(size.value_or(intrinsic_size().value_or({}).to_type<int>()));
    return {};
}

void ImageProvider::image_provider_contents_changed() const
{
    auto const* layout_node = image_provider_layout_node();
    if (!layout_node)
        return;
    if (layout_node->kind() == RustFFI::NodeKind::ImageBox) {
        auto const& image_box = static_cast<Box const&>(*layout_node);
        // A box that owns its provider is handed it once the layout update that built the box is over.
        if (RustFFI::layout_arena_image_box_awaits_owned_provider(image_box.arena_handle(), Node::slot_id(&image_box)) || &image_box.image_provider() != this)
            return;
    }
    Painting::push_replaced_image_paint_facts(*layout_node);
}

}
