/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/Painting/ShadowData.h>

namespace Web::Painting {

WEB_API void paint_box_shadow(
    DisplayListRecordingContext&,
    CSSPixelRect const& bordered_content_rect,
    CSSPixelRect const& borderless_content_rect,
    BordersData const& borders_data,
    BorderRadiiData const&,
    Vector<ShadowData> const&);
WEB_API void paint_text_shadow(DisplayListRecordingContext&, PaintableFragment::FragmentSpan const&);

}
