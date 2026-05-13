/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListCommand.h>

namespace Web::Painting {

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
    builder.appendff(" dst_rect={} clip_rect={}", dst_rect, clip_rect);
}

void DrawRepeatedDecodedImageFrame::dump(StringBuilder& builder) const
{
    builder.appendff(" dst_rect={} clip_rect={}", dst_rect, clip_rect);
}

void DrawExternalContent::dump(StringBuilder& builder) const
{
    builder.appendff(" dst_rect={}", dst_rect);
}

void DrawVideoFrameSource::dump(StringBuilder& builder) const
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
