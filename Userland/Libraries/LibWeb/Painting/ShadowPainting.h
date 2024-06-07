/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>
#include <LibWeb/Painting/PaintContext.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/Painting/ShadowData.h>

namespace Web::Painting {

void paint_inner_box_shadow(Gfx::Painter&, PaintBoxShadowParams params);

Gfx::IntRect get_outer_box_shadow_bounding_rect(PaintBoxShadowParams params);
void paint_outer_box_shadow(Gfx::Painter& painter, PaintBoxShadowParams params);

void paint_box_shadow(
    PaintContext&,
    CSSPixelRect const& bordered_content_rect,
    CSSPixelRect const& borderless_content_rect,
    BordersData const& borders_data,
    BorderRadiiData const&,
    Vector<ShadowData> const&);
void paint_text_shadow(PaintContext&, PaintableFragment const&, Vector<ShadowData> const&);

}
