/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class TextPaintable final : public Paintable {
public:
    static NonnullRefPtr<TextPaintable> create(Layout::TextNode const&);
    virtual StringView class_name() const override { return "TextPaintable"sv; }

    Layout::TextNode const& layout_node() const { return static_cast<Layout::TextNode const&>(Paintable::layout_node()); }

protected:
    virtual void paint_inspector_overlay_internal(DisplayListRecordingContext&) const override;

private:
    virtual bool is_text_paintable() const override { return true; }

    TextPaintable(Layout::TextNode const&);
};

}
