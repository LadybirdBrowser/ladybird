/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/SVGImageBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class ImagePaintable final : public PaintableBox {
public:
    static NonnullRefPtr<ImagePaintable> create(Layout::ImageBox const& layout_box);
    static NonnullRefPtr<ImagePaintable> create(Layout::SVGImageBox const& layout_box);
    virtual StringView class_name() const override { return "ImagePaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;
    virtual void reset_for_relayout() override;

private:
    ImagePaintable(Layout::Box const& layout_box, Layout::ImageProvider const& image_provider, bool renders_as_alt_text, String alt_text, bool is_svg_image);

    bool m_renders_as_alt_text { false };
    String m_alt_text;

    Layout::ImageProvider const& m_image_provider;

    bool m_is_svg_image { false };
};

}
