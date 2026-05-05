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

Gfx::IntRect DrawGlyphRun::bounding_rect() const
{
    return glyph_run->cached_blob_bounds().translated(translation).to_rounded<int>();
}

void DrawGlyphRun::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={} translation={} color={}", rect, translation, color);
}

void FillRect::dump(StringBuilder& builder) const
{
    builder.appendff(" rect={} color={}", rect, color);
}

void DrawScaledImmutableBitmap::dump(StringBuilder& builder) const
{
    builder.appendff(" dst_rect={} clip_rect={}", dst_rect, clip_rect);
}

void DrawRepeatedImmutableBitmap::dump(StringBuilder& builder) const
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
    builder.appendff(" rect={} position={} angle={}", rect, position, conic_gradient_data.start_angle);
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

void PaintScrollBar::dump(StringBuilder&) const
{
}

void ApplyEffects::dump(StringBuilder& builder) const
{
    builder.appendff(" opacity={} has_filter={}", opacity, filter.has_value());
}

}
