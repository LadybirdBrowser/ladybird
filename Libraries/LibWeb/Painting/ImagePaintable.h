/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class ImagePaintable final : public Paintable {
public:
    static NonnullRefPtr<ImagePaintable> create(Layout::Box const& layout_box, Layout::ImageProvider const& image_provider);
    virtual StringView class_name() const override { return "ImagePaintable"sv; }

    Layout::ImageProvider const& image_provider() const { return m_image_provider; }

private:
    ImagePaintable(Layout::Box const& layout_box, Layout::ImageProvider const& image_provider);

    Layout::ImageProvider const& m_image_provider;
};

}
