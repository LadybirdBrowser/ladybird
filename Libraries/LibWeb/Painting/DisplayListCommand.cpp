/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

void DrawGlyphRun::translate_by(Gfx::IntPoint const& offset)
{
    rect.translate_by(offset);
    translation.translate_by(offset.to_type<float>());
    bounding_rectangle.translate_by(offset);
}

Gfx::IntRect PaintOuterBoxShadow::bounding_rect() const
{
    auto shadow_rect = box_shadow_params.device_content_rect;
    auto spread = box_shadow_params.blur_radius * 2 + box_shadow_params.spread_distance;
    shadow_rect.inflate(spread, spread, spread, spread);
    auto offset_x = box_shadow_params.offset_x;
    auto offset_y = box_shadow_params.offset_y;
    shadow_rect.translate_by(offset_x, offset_y);
    return shadow_rect;
}

Gfx::IntRect PaintInnerBoxShadow::bounding_rect() const
{
    return box_shadow_params.device_content_rect;
}

void PaintOuterBoxShadow::translate_by(Gfx::IntPoint const& offset)
{
    box_shadow_params.device_content_rect.translate_by(offset);
}

void PaintInnerBoxShadow::translate_by(Gfx::IntPoint const& offset)
{
    box_shadow_params.device_content_rect.translate_by(offset);
}

void DrawGlyphRun::dump(StringBuilder& builder) const
{
    builder.appendff("DrawGlyphRun rect={} translation={} color={} scale={}", rect, translation, color, scale);
}

void FillRect::dump(StringBuilder& builder) const
{
    builder.appendff("FillRect rect={} color={}", rect, color);
}

void DrawPaintingSurface::dump(StringBuilder& builder) const
{
    builder.appendff("DrawPaintingSurface dst_rect={} src_rect={}", dst_rect, src_rect);
}

void DrawScaledImmutableBitmap::dump(StringBuilder& builder) const
{
    builder.appendff("DrawScaledImmutableBitmap dst_rect={} clip_rect={}", dst_rect, clip_rect);
}

void DrawRepeatedImmutableBitmap::dump(StringBuilder& builder) const
{
    builder.appendff("DrawRepeatedImmutableBitmap dst_rect={} clip_rect={}", dst_rect, clip_rect);
}

void Save::dump(StringBuilder& builder) const
{
    builder.appendff("Save");
}

void SaveLayer::dump(StringBuilder& builder) const
{
    builder.appendff("SaveLayer");
}

void Restore::dump(StringBuilder& builder) const
{
    builder.appendff("Restore");
}

void Translate::dump(StringBuilder& builder) const
{
    builder.appendff("Translate delta={}", delta);
}

void AddClipRect::dump(StringBuilder& builder) const
{
    builder.appendff("AddClipRect rect={}", rect);
}

void PushStackingContext::dump(StringBuilder& builder) const
{
    auto affine_transform = extract_2d_affine_transform(transform.matrix);
    builder.appendff("PushStackingContext opacity={} isolate={} has_clip_path={} transform={} bounding_rect={}", opacity, isolate, clip_path.has_value(), affine_transform, bounding_rect);
}

void PopStackingContext::dump(StringBuilder& builder) const
{
    builder.appendff("PopStackingContext");
}

void PaintLinearGradient::dump(StringBuilder& builder) const
{
    builder.appendff("PaintLinearGradient rect={}", gradient_rect);
}

void PaintRadialGradient::dump(StringBuilder& builder) const
{
    builder.appendff("PaintRadialGradient rect={} center={} size={}", rect, center, size);
}

void PaintConicGradient::dump(StringBuilder& builder) const
{
    builder.appendff("PaintConicGradient rect={} position={} angle={}", rect, position, conic_gradient_data.start_angle);
}

void PaintOuterBoxShadow::dump(StringBuilder& builder) const
{
    builder.appendff("PaintOuterBoxShadow content_rect={} offset=({},{}) blur_radius={} spread_distance={} color={}", box_shadow_params.device_content_rect, box_shadow_params.offset_x, box_shadow_params.offset_y, box_shadow_params.blur_radius, box_shadow_params.spread_distance, box_shadow_params.color);
}

void PaintInnerBoxShadow::dump(StringBuilder& builder) const
{
    builder.appendff("PaintInnerBoxShadow content_rect={} offset=({},{}) blur_radius={} spread_distance={} color={}", box_shadow_params.device_content_rect, box_shadow_params.offset_x, box_shadow_params.offset_y, box_shadow_params.blur_radius, box_shadow_params.spread_distance, box_shadow_params.color);
}

void PaintTextShadow::dump(StringBuilder& builder) const
{
    builder.appendff("PaintTextShadow shadow_rect={} text_rect={} draw_location={} blur_radius={} color={} scale={}", shadow_bounding_rect, text_rect, draw_location, blur_radius, color, glyph_run_scale);
}

void FillRectWithRoundedCorners::dump(StringBuilder& builder) const
{
    builder.appendff("FillRectWithRoundedCorners rect={} color={}", rect, color);
}

void FillPath::dump(StringBuilder& builder) const
{
    builder.appendff("FillPath path_bounding_rect={}", path_bounding_rect);
}

void StrokePath::dump(StringBuilder& builder) const
{
    builder.appendff("StrokePath");
}

void DrawEllipse::dump(StringBuilder& builder) const
{
    builder.appendff("DrawEllipse rect={} color={} thickness={}", rect, color, thickness);
}

void FillEllipse::dump(StringBuilder& builder) const
{
    builder.appendff("FillEllipse rect={} color={}", rect, color);
}

void DrawLine::dump(StringBuilder& builder) const
{
    builder.appendff("DrawLine from={} to={} color={} thickness={}", from, to, color, thickness);
}

void ApplyBackdropFilter::dump(StringBuilder& builder) const
{
    builder.appendff("ApplyBackdropFilter backdrop_region={}", backdrop_region);
}

void DrawRect::dump(StringBuilder& builder) const
{
    builder.appendff("DrawRect rect={} color={} rough={}", rect, color, rough);
}

void AddRoundedRectClip::dump(StringBuilder& builder) const
{
    builder.appendff("AddRoundedRectClip rect={}", border_rect);
}

void AddMask::dump(StringBuilder& builder) const
{
    builder.appendff("AddMask rect={}", rect);
}

void PaintNestedDisplayList::dump(StringBuilder& builder) const
{
    builder.appendff("PaintNestedDisplayList rect={}", rect);
}

void PaintScrollBar::dump(StringBuilder& builder) const
{
    builder.appendff("PaintScrollBar");
}

void ApplyOpacity::dump(StringBuilder& builder) const
{
    builder.appendff("ApplyOpacity opacity={}", opacity);
}

void ApplyCompositeAndBlendingOperator::dump(StringBuilder& builder) const
{
    builder.appendff("ApplyCompositeAndBlendingOperator");
}

void ApplyFilter::dump(StringBuilder& builder) const
{
    builder.appendff("ApplyFilter");
}

void ApplyTransform::dump(StringBuilder& builder) const
{
    auto affine_transform = extract_2d_affine_transform(matrix);
    builder.appendff("ApplyTransform matrix={}", affine_transform);
}

void ApplyMaskBitmap::dump(StringBuilder& builder) const
{
    builder.appendff("ApplyMaskBitmap");
}

}
