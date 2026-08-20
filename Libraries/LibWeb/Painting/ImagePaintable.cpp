/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Painting/ImagePaintable.h>

namespace Web::Painting {

NonnullRefPtr<ImagePaintable> ImagePaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new ImagePaintable(layout_box));
}

ImagePaintable::ImagePaintable(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

Layout::ImageProvider const& ImagePaintable::image_provider() const
{
    return static_cast<Layout::Box const&>(layout_node()).image_provider();
}

}
