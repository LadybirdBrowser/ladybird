/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/ImageProvider.h>

namespace Web::Layout {

void ImageProvider::did_update_alt_text(ImageBox& layout_node)
{
    layout_node.dom_node_did_update_alt_text({});
}

Optional<CSSPixelSize> ImageProvider::intrinsic_size() const
{
    auto width = intrinsic_width();
    auto height = intrinsic_height();
    if (!width.has_value() || !height.has_value())
        return {};

    return CSSPixelSize { *width, *height };
}

RefPtr<Gfx::ImmutableBitmap> ImageProvider::current_image_bitmap() const
{
    return current_image_bitmap_sized(intrinsic_size().value_or({}).to_type<int>());
}

RefPtr<Gfx::ImmutableBitmap> ImageProvider::default_image_bitmap() const
{
    return default_image_bitmap_sized(intrinsic_size().value_or({}).to_type<int>());
}

RefPtr<Gfx::ImmutableBitmap> ImageProvider::default_image_bitmap_sized(Gfx::IntSize size) const
{
    // Defer to the current image by default.
    return current_image_bitmap_sized(size);
}

}
