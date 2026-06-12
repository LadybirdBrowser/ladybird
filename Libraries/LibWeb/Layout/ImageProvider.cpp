/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/ImageProvider.h>

namespace Web::Layout {

void ImageProvider::did_update_alt_text(ImageBox& layout_node)
{
    layout_node.dom_node_did_update_alt_text({});
}

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

Optional<Gfx::DecodedImageFrame> ImageProvider::current_image_frame() const
{
    return current_image_frame_sized(intrinsic_size().value_or({}).to_type<int>());
}

Optional<Gfx::DecodedImageFrame> ImageProvider::current_image_frame_sized(Gfx::IntSize size) const
{
    if (auto const& data = decoded_image_data())
        return data->frame(current_frame_index(), size);
    return {};
}

Optional<Gfx::DecodedImageFrame> ImageProvider::default_image_frame_sized(Gfx::IntSize size) const
{
    if (auto const& data = decoded_image_data())
        return data->frame(0, size);
    return {};
}

}
