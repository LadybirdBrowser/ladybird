/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/BorderPainting.h>
#include <LibWeb/Painting/PaintContext.h>

namespace Web::Painting {

struct ResolvedBackgroundLayerData {
    RefPtr<CSS::AbstractImageStyleValue const> background_image;
    CSS::BackgroundAttachment attachment;
    CSS::BackgroundBox clip;
    CSS::PositionEdge position_edge_x;
    CSS::PositionEdge position_edge_y;
    CSSPixels offset_x;
    CSSPixels offset_y;
    CSSPixelRect background_positioning_area;
    CSSPixelRect image_rect;
    CSS::Repeat repeat_x;
    CSS::Repeat repeat_y;
    CSS::MixBlendMode blend_mode;
};

struct BackgroundBox {
    CSSPixelRect rect;
    BorderRadiiData radii;

    inline void shrink(CSSPixels top, CSSPixels right, CSSPixels bottom, CSSPixels left)
    {
        rect.shrink(top, right, bottom, left);
        radii.shrink(top, right, bottom, left);
    }
};

struct ResolvedBackground {
    BackgroundBox color_box;
    Vector<ResolvedBackgroundLayerData> layers;
    bool needs_text_clip { false };
    CSSPixelRect background_rect {};
    Color color {};
};

ResolvedBackground resolve_background_layers(Vector<CSS::BackgroundLayerData> const& layers, PaintableBox const& paintable_box, Color background_color, CSSPixelRect const& border_rect, BorderRadiiData const& border_radii);

void paint_background(PaintContext&, PaintableBox const&, CSS::ImageRendering, ResolvedBackground resolved_background, BorderRadiiData const&);

}
