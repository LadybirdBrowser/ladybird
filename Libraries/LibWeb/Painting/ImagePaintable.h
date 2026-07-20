/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class ImagePaintable final : public Paintable {
public:
    static NonnullRefPtr<ImagePaintable> create(Layout::ImageBox const& layout_box);
    virtual StringView class_name() const override { return "ImagePaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;
    virtual void reset_for_relayout() override;

private:
    ImagePaintable(Layout::Box const& layout_box, Layout::ImageProvider const& image_provider, Utf16String alt_text);

    Utf16String m_alt_text;

    Layout::ImageProvider const& m_image_provider;
};

}
