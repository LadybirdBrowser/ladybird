/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListCommand.h>

namespace Web::Painting {

static StringView scaling_mode_name(Gfx::ScalingMode scaling_mode)
{
    switch (scaling_mode) {
    case Gfx::ScalingMode::None:
        return "None"sv;
    case Gfx::ScalingMode::Bilinear:
        return "Bilinear"sv;
    case Gfx::ScalingMode::BilinearMipmap:
        return "BilinearMipmap"sv;
    case Gfx::ScalingMode::NearestNeighbor:
        return "NearestNeighbor"sv;
    }
    VERIFY_NOT_REACHED();
}

Gfx::IntRect PaintOuterBoxShadow::bounding_rect() const
{
    auto rect = shadow_rect;
    rect.inflate(blur_radius * 2, blur_radius * 2, blur_radius * 2, blur_radius * 2);
    return rect;
}

Gfx::IntRect PaintInnerBoxShadow::bounding_rect() const
{
    return device_content_rect;
}

void DrawGlyphRun::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={} translation={} color={}", rect, translation, color);
}

void FillRect::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={} color={}", rect, color);
}

void DrawScaledDecodedImageFrame::dump(StringBuilder& builder) const
{
    builder.appendff(" dst_rect={}", dst_rect);
    if (src_rect.has_value())
        builder.appendff(" src_rect={}", src_rect.value());
}

void DrawRepeatedDecodedImageFrame::dump(StringBuilder& builder) const
{
    builder.appendff(" dst_rect={} clip_rect={}", dst_rect, clip_rect);
}

void DrawRepeatedDisplayList::dump(StringBuilder& builder) const
{
    builder.appendff(" dst_rect={} clip_rect={} scaling_mode={}", dst_rect, clip_rect, scaling_mode_name(scaling_mode));
}

void DrawTiledDecodedImageFrame::dump(StringBuilder& builder) const
{
    builder.appendff(" tile_rect={} clip_rect={} src_rect={} tile_step={}", tile_rect, clip_rect, src_rect, tile_step);
    if (tile_count_x.has_value())
        builder.appendff(" tile_count_x={}", tile_count_x.value());
    else
        builder.appendff(" tile_count_x=repeat");
    if (tile_count_y.has_value())
        builder.appendff(" tile_count_y={}", tile_count_y.value());
    else
        builder.appendff(" tile_count_y=repeat");
}

void DrawCompositedContext::dump(StringBuilder& builder) const
{
    builder.appendff(" dst_rect={}", dst_rect);
}

void DrawCanvas::dump(StringBuilder& builder) const
{
    builder.appendff(" dst_rect={}", dst_rect);
}

void DrawVideoFrame::dump(StringBuilder& builder) const
{
    builder.appendff(" dst_rect={}", dst_rect);
}

void Save::dump(StringBuilder&) const
{
}

void SaveLayer::dump(StringBuilder&) const
{
}

void Restore::dump(StringBuilder&) const
{
}

void Translate::dump(StringBuilder& builder) const
{
    builder.appendff(" delta={}", delta);
}

void AddClipRect::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={}", rect);
}

void PaintLinearGradient::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={}", gradient_rect);
}

void PaintRadialGradient::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={} center={} size={}", rect, center, size);
}

void PaintConicGradient::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={} position={} angle={}", rect, position, start_angle);
}

void PaintOuterBoxShadow::dump(StringBuilder& builder) const
{
    builder.appendff(" content_rect={} shadow_rect={} blur_radius={} color={}", device_content_rect, shadow_rect, blur_radius, color);
}

void PaintInnerBoxShadow::dump(StringBuilder& builder) const
{
    builder.appendff(" content_rect={} outer_shadow_rect={} inner_shadow_rect={} blur_radius={} color={}", device_content_rect, outer_shadow_rect, inner_shadow_rect, blur_radius, color);
}

void PaintTextShadow::dump(StringBuilder& builder) const
{
    builder.appendff(" shadow_rect={} text_rect={} draw_location={} blur_radius={} color={}", shadow_bounding_rect, text_rect, draw_location, blur_radius, color);
}

void FillRectWithRoundedCorners::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={} color={}", rect, color);
}

void FillPath::dump(StringBuilder& builder) const
{
    builder.appendff(" path_bounding_rect={}", path_bounding_rect);
}

void StrokePath::dump(StringBuilder&) const
{
}

void DrawEllipse::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={} color={} thickness={}", rect, color, thickness);
}

void FillEllipse::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={} color={}", rect, color);
}

void DrawLine::dump(StringBuilder& builder) const
{
    builder.appendff(" from={} to={} color={} thickness={}", from, to, color, thickness);
}

void ApplyBackdropFilter::dump(StringBuilder& builder) const
{
    builder.appendff(" backdrop_region={}", backdrop_region);
}

void DrawRect::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={} color={} rough={}", rect, color, rough);
}

void AddRoundedRectClip::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={}", border_rect);
}

void PaintNestedDisplayList::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={}", rect);
}

void CompositorScrollNode::dump(StringBuilder& builder) const
{
    builder.appendff(" scroll_frame_index={} parent_scroll_frame_index={} scrollport_rect={} max_scroll_offset={} is_viewport={}",
        scroll_frame_index, parent_scroll_frame_index, scrollport_rect, max_scroll_offset, is_viewport);
}

static void dump_optional_float(StringBuilder& builder, Optional<float> value)
{
    if (value.has_value())
        builder.appendff("{}", *value);
    else
        builder.append("none"sv);
}

void CompositorStickyArea::dump(StringBuilder& builder) const
{
    builder.appendff(" scroll_frame_index={} parent_scroll_frame_index={} nearest_scrolling_ancestor_index={} position_relative_to_scroll_ancestor={} border_box_size={} scrollport_size={} containing_block_region={} needs_parent_offset_adjustment={} inset_top=",
        scroll_frame_index, parent_scroll_frame_index, nearest_scrolling_ancestor_index, position_relative_to_scroll_ancestor, border_box_size, scrollport_size, containing_block_region, needs_parent_offset_adjustment);
    dump_optional_float(builder, inset_top);
    builder.append(" inset_right="sv);
    dump_optional_float(builder, inset_right);
    builder.append(" inset_bottom="sv);
    dump_optional_float(builder, inset_bottom);
    builder.append(" inset_left="sv);
    dump_optional_float(builder, inset_left);
}

void CompositorBlockingWheelEventRegion::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={}", rect);
}

void CompositorWheelHitTestTarget::dump(StringBuilder& builder) const
{
    builder.appendff(" target_scroll_frame_index={} rect={}", target_scroll_frame_index, rect);
}

void CompositorWheelHitTestTargetWithCornerRadii::dump(StringBuilder& builder) const
{
    builder.appendff(" target_scroll_frame_index={} rect={}", target_scroll_frame_index, rect);
    if (corner_radii.has_any_radius()) {
        builder.appendff(" corner_radii=[{}x{},{}x{},{}x{},{}x{}]",
            corner_radii.top_left.horizontal_radius, corner_radii.top_left.vertical_radius,
            corner_radii.top_right.horizontal_radius, corner_radii.top_right.vertical_radius,
            corner_radii.bottom_right.horizontal_radius, corner_radii.bottom_right.vertical_radius,
            corner_radii.bottom_left.horizontal_radius, corner_radii.bottom_left.vertical_radius);
    }
}

void CompositorMainThreadWheelEventRegion::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={}", rect);
}

void CompositorViewportScrollbar::dump(StringBuilder& builder) const
{
    builder.appendff(" scroll_frame_index={} gutter_rect={} thumb_rect={} expanded_gutter_rect={} expanded_thumb_rect={} scroll_size={} expanded_scroll_size={} max_scroll_offset={} thumb_color={} track_color={} vertical={}",
        scroll_frame_index, gutter_rect, thumb_rect, expanded_gutter_rect, expanded_thumb_rect, scroll_size, expanded_scroll_size, max_scroll_offset, thumb_color, track_color, vertical);
}

void PaintScrollBar::dump(StringBuilder&) const
{
}

void ApplyEffects::dump(StringBuilder& builder) const
{
    builder.appendff(" opacity={} has_filter={}", opacity, has_filter);
}

}
