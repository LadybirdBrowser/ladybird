/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/ImageProvider.h>

namespace Web::Layout {

void ImageProvider::did_update_alt_text(ImageBox& layout_node)
{
    layout_node.dom_node_did_update_alt_text({});
}

RefPtr<Gfx::ImmutableBitmap> ImageProvider::current_image_bitmap() const
{
    int w = intrinsic_width().value_or({}).to_int();
    int h = intrinsic_height().value_or({}).to_int();
    return current_image_bitmap_sized({ w, h });
}

}
