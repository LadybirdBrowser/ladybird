/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

struct PrerecordedMaskLayerDisplayList {
    MaskLayerOrigin origin;
    bool mask_layer_area_is_empty { false };
    Optional<DisplayListResource> resource;
};

struct PrerecordedNestedDisplayLists {
    HashMap<Paintable const*, Vector<PrerecordedMaskLayerDisplayList, 3>> mask_entries;
    HashMap<Paintable const*, Optional<PaintStyle>> pattern_paint_styles;
};

void prerecord_nested_display_lists(DisplayListRecordingContext&, ViewportPaintable&);

DisplayListResource record_nested_svg_display_list(DisplayListRecordingContext&, Paintable const&, TransformData root_transform, IncludeRootElementTransform, bool draw_svg_geometry_for_clip_path);

Optional<PaintStyle> prerecorded_pattern_paint_style(DisplayListRecordingContext&, SVG::SVGPatternElement const&, Layout::Node const& target_layout_node);

}
