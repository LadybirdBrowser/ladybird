/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Environment.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Filter.h>
#include <LibGfx/GradientInterpolation.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Path.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/CanvasPaintable.h>
#include <LibWeb/Painting/CheckBoxPaintable.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/DevicePixelConverter.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ImagePaint.h>
#include <LibWeb/Painting/ImagePaintable.h>
#include <LibWeb/Painting/InlinePaintable.h>
#include <LibWeb/Painting/NavigableContainerViewportPaintable.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/PaintingRustFFI.h>
#include <LibWeb/Painting/RadioButtonPaintable.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/SVGImagePaintable.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/ShadowData.h>
#include <LibWeb/Painting/VideoPaintable.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>

namespace Web::Painting {

static_assert(sizeof(RustFFI::IntPoint) == sizeof(Gfx::IntPoint));
static_assert(alignof(RustFFI::IntPoint) == alignof(Gfx::IntPoint));
static_assert(sizeof(RustFFI::FloatPoint) == sizeof(Gfx::FloatPoint));
static_assert(alignof(RustFFI::FloatPoint) == alignof(Gfx::FloatPoint));
static_assert(sizeof(RustFFI::IntSize) == sizeof(Gfx::IntSize));
static_assert(alignof(RustFFI::IntSize) == alignof(Gfx::IntSize));
static_assert(sizeof(RustFFI::FloatSize) == sizeof(Gfx::FloatSize));
static_assert(alignof(RustFFI::FloatSize) == alignof(Gfx::FloatSize));
static_assert(sizeof(RustFFI::IntRect) == sizeof(Gfx::IntRect));
static_assert(alignof(RustFFI::IntRect) == alignof(Gfx::IntRect));
static_assert(sizeof(RustFFI::FloatRect) == sizeof(Gfx::FloatRect));
static_assert(alignof(RustFFI::FloatRect) == alignof(Gfx::FloatRect));
static_assert(sizeof(RustFFI::Color) == sizeof(Gfx::Color));
static_assert(alignof(RustFFI::Color) == alignof(Gfx::Color));
static_assert(sizeof(RustFFI::AffineTransform) == sizeof(Gfx::AffineTransform));
static_assert(alignof(RustFFI::AffineTransform) == alignof(Gfx::AffineTransform));
static_assert(sizeof(RustFFI::FloatMatrix4x4) == sizeof(Gfx::FloatMatrix4x4));
static_assert(alignof(RustFFI::FloatMatrix4x4) == alignof(Gfx::FloatMatrix4x4));
static_assert(sizeof(RustFFI::CornerRadius) == sizeof(Gfx::CornerRadius));
static_assert(alignof(RustFFI::CornerRadius) == alignof(Gfx::CornerRadius));
static_assert(sizeof(RustFFI::CornerRadii) == sizeof(Gfx::CornerRadii));
static_assert(alignof(RustFFI::CornerRadii) == alignof(Gfx::CornerRadii));
static_assert(sizeof(RustFFI::GradientInterpolationMethod) == sizeof(Gfx::GradientInterpolationMethod));
static_assert(alignof(RustFFI::GradientInterpolationMethod) == alignof(Gfx::GradientInterpolationMethod));
static_assert(offsetof(RustFFI::CornerRadius, horizontal_radius) == offsetof(Gfx::CornerRadius, horizontal_radius));
static_assert(offsetof(RustFFI::CornerRadius, vertical_radius) == offsetof(Gfx::CornerRadius, vertical_radius));
static_assert(offsetof(RustFFI::CornerRadii, top_left) == offsetof(Gfx::CornerRadii, top_left));
static_assert(offsetof(RustFFI::CornerRadii, top_right) == offsetof(Gfx::CornerRadii, top_right));
static_assert(offsetof(RustFFI::CornerRadii, bottom_right) == offsetof(Gfx::CornerRadii, bottom_right));
static_assert(offsetof(RustFFI::CornerRadii, bottom_left) == offsetof(Gfx::CornerRadii, bottom_left));
static_assert(offsetof(RustFFI::GradientInterpolationMethod, interpolation_type) == offsetof(Gfx::GradientInterpolationMethod, type));
static_assert(offsetof(RustFFI::GradientInterpolationMethod, rectangular_color_space) == offsetof(Gfx::GradientInterpolationMethod, rectangular_color_space));
static_assert(offsetof(RustFFI::GradientInterpolationMethod, polar_color_space) == offsetof(Gfx::GradientInterpolationMethod, polar_color_space));
static_assert(offsetof(RustFFI::GradientInterpolationMethod, hue_interpolation_method) == offsetof(Gfx::GradientInterpolationMethod, hue_interpolation_method));
static_assert(sizeof(RustFFI::WindingRule) == sizeof(Gfx::WindingRule));
static_assert(to_underlying(RustFFI::WindingRule::Nonzero) == to_underlying(Gfx::WindingRule::Nonzero));
static_assert(to_underlying(RustFFI::WindingRule::EvenOdd) == to_underlying(Gfx::WindingRule::EvenOdd));
static_assert(sizeof(RustFFI::LineStyle) == sizeof(Gfx::LineStyle));
static_assert(to_underlying(RustFFI::LineStyle::Solid) == to_underlying(Gfx::LineStyle::Solid));
static_assert(to_underlying(RustFFI::LineStyle::Dotted) == to_underlying(Gfx::LineStyle::Dotted));
static_assert(to_underlying(RustFFI::LineStyle::Dashed) == to_underlying(Gfx::LineStyle::Dashed));
static_assert(sizeof(RustFFI::ScalingMode) == sizeof(Gfx::ScalingMode));
static_assert(to_underlying(RustFFI::ScalingMode::None) == to_underlying(Gfx::ScalingMode::None));
static_assert(to_underlying(RustFFI::ScalingMode::Bilinear) == to_underlying(Gfx::ScalingMode::Bilinear));
static_assert(to_underlying(RustFFI::ScalingMode::BilinearMipmap) == to_underlying(Gfx::ScalingMode::BilinearMipmap));
static_assert(to_underlying(RustFFI::ScalingMode::NearestNeighbor) == to_underlying(Gfx::ScalingMode::NearestNeighbor));
static_assert(sizeof(RustFFI::CompositingAndBlendingOperator) == sizeof(Gfx::CompositingAndBlendingOperator));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Normal) == to_underlying(Gfx::CompositingAndBlendingOperator::Normal));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Multiply) == to_underlying(Gfx::CompositingAndBlendingOperator::Multiply));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Screen) == to_underlying(Gfx::CompositingAndBlendingOperator::Screen));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Darken) == to_underlying(Gfx::CompositingAndBlendingOperator::Darken));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Lighten) == to_underlying(Gfx::CompositingAndBlendingOperator::Lighten));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Overlay) == to_underlying(Gfx::CompositingAndBlendingOperator::Overlay));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::ColorDodge) == to_underlying(Gfx::CompositingAndBlendingOperator::ColorDodge));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::ColorBurn) == to_underlying(Gfx::CompositingAndBlendingOperator::ColorBurn));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::HardLight) == to_underlying(Gfx::CompositingAndBlendingOperator::HardLight));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::SoftLight) == to_underlying(Gfx::CompositingAndBlendingOperator::SoftLight));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Difference) == to_underlying(Gfx::CompositingAndBlendingOperator::Difference));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Exclusion) == to_underlying(Gfx::CompositingAndBlendingOperator::Exclusion));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Hue) == to_underlying(Gfx::CompositingAndBlendingOperator::Hue));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Saturation) == to_underlying(Gfx::CompositingAndBlendingOperator::Saturation));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Color) == to_underlying(Gfx::CompositingAndBlendingOperator::Color));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Luminosity) == to_underlying(Gfx::CompositingAndBlendingOperator::Luminosity));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Clear) == to_underlying(Gfx::CompositingAndBlendingOperator::Clear));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Copy) == to_underlying(Gfx::CompositingAndBlendingOperator::Copy));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::SourceOver) == to_underlying(Gfx::CompositingAndBlendingOperator::SourceOver));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::DestinationOver) == to_underlying(Gfx::CompositingAndBlendingOperator::DestinationOver));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::SourceIn) == to_underlying(Gfx::CompositingAndBlendingOperator::SourceIn));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::DestinationIn) == to_underlying(Gfx::CompositingAndBlendingOperator::DestinationIn));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::SourceOut) == to_underlying(Gfx::CompositingAndBlendingOperator::SourceOut));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::DestinationOut) == to_underlying(Gfx::CompositingAndBlendingOperator::DestinationOut));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::SourceATop) == to_underlying(Gfx::CompositingAndBlendingOperator::SourceATop));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::DestinationATop) == to_underlying(Gfx::CompositingAndBlendingOperator::DestinationATop));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Xor) == to_underlying(Gfx::CompositingAndBlendingOperator::Xor));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::Lighter) == to_underlying(Gfx::CompositingAndBlendingOperator::Lighter));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::PlusDarker) == to_underlying(Gfx::CompositingAndBlendingOperator::PlusDarker));
static_assert(to_underlying(RustFFI::CompositingAndBlendingOperator::PlusLighter) == to_underlying(Gfx::CompositingAndBlendingOperator::PlusLighter));
static_assert(sizeof(RustFFI::MaskKind) == sizeof(Gfx::MaskKind));
static_assert(to_underlying(RustFFI::MaskKind::Alpha) == to_underlying(Gfx::MaskKind::Alpha));
static_assert(to_underlying(RustFFI::MaskKind::Luminance) == to_underlying(Gfx::MaskKind::Luminance));
static_assert(sizeof(RustFFI::Orientation) == sizeof(Gfx::Orientation));
static_assert(to_underlying(RustFFI::Orientation::Horizontal) == to_underlying(Gfx::Orientation::Horizontal));
static_assert(to_underlying(RustFFI::Orientation::Vertical) == to_underlying(Gfx::Orientation::Vertical));
static_assert(sizeof(RustFFI::ShouldAntiAlias) == sizeof(Gfx::ShouldAntiAlias));
static_assert(to_underlying(RustFFI::ShouldAntiAlias::Yes) == to_underlying(Gfx::ShouldAntiAlias::Yes));
static_assert(to_underlying(RustFFI::ShouldAntiAlias::No) == to_underlying(Gfx::ShouldAntiAlias::No));
static_assert(sizeof(RustFFI::CornerClip) == sizeof(Gfx::CornerClip));
static_assert(to_underlying(RustFFI::CornerClip::Outside) == to_underlying(Gfx::CornerClip::Outside));
static_assert(to_underlying(RustFFI::CornerClip::Inside) == to_underlying(Gfx::CornerClip::Inside));
static_assert(sizeof(RustFFI::InterpolationColorSpace) == sizeof(Gfx::InterpolationColorSpace));
static_assert(to_underlying(RustFFI::InterpolationColorSpace::LinearRGB) == to_underlying(Gfx::InterpolationColorSpace::LinearRGB));
static_assert(to_underlying(RustFFI::InterpolationColorSpace::SRGB) == to_underlying(Gfx::InterpolationColorSpace::SRGB));
static_assert(sizeof(RustFFI::RectangularColorSpace) == sizeof(Gfx::RectangularColorSpace));
static_assert(to_underlying(RustFFI::RectangularColorSpace::Srgb) == to_underlying(Gfx::RectangularColorSpace::Srgb));
static_assert(to_underlying(RustFFI::RectangularColorSpace::SrgbLinear) == to_underlying(Gfx::RectangularColorSpace::SrgbLinear));
static_assert(to_underlying(RustFFI::RectangularColorSpace::DisplayP3) == to_underlying(Gfx::RectangularColorSpace::DisplayP3));
static_assert(to_underlying(RustFFI::RectangularColorSpace::DisplayP3Linear) == to_underlying(Gfx::RectangularColorSpace::DisplayP3Linear));
static_assert(to_underlying(RustFFI::RectangularColorSpace::A98Rgb) == to_underlying(Gfx::RectangularColorSpace::A98Rgb));
static_assert(to_underlying(RustFFI::RectangularColorSpace::ProphotoRgb) == to_underlying(Gfx::RectangularColorSpace::ProphotoRgb));
static_assert(to_underlying(RustFFI::RectangularColorSpace::Rec2020) == to_underlying(Gfx::RectangularColorSpace::Rec2020));
static_assert(to_underlying(RustFFI::RectangularColorSpace::Lab) == to_underlying(Gfx::RectangularColorSpace::Lab));
static_assert(to_underlying(RustFFI::RectangularColorSpace::Oklab) == to_underlying(Gfx::RectangularColorSpace::Oklab));
static_assert(to_underlying(RustFFI::RectangularColorSpace::Xyz) == to_underlying(Gfx::RectangularColorSpace::Xyz));
static_assert(to_underlying(RustFFI::RectangularColorSpace::XyzD50) == to_underlying(Gfx::RectangularColorSpace::XyzD50));
static_assert(to_underlying(RustFFI::RectangularColorSpace::XyzD65) == to_underlying(Gfx::RectangularColorSpace::XyzD65));
static_assert(sizeof(RustFFI::PolarColorSpace) == sizeof(Gfx::PolarColorSpace));
static_assert(to_underlying(RustFFI::PolarColorSpace::Hsl) == to_underlying(Gfx::PolarColorSpace::Hsl));
static_assert(to_underlying(RustFFI::PolarColorSpace::Hwb) == to_underlying(Gfx::PolarColorSpace::Hwb));
static_assert(to_underlying(RustFFI::PolarColorSpace::Lch) == to_underlying(Gfx::PolarColorSpace::Lch));
static_assert(to_underlying(RustFFI::PolarColorSpace::Oklch) == to_underlying(Gfx::PolarColorSpace::Oklch));
static_assert(sizeof(RustFFI::HueInterpolationMethod) == sizeof(Gfx::HueInterpolationMethod));
static_assert(to_underlying(RustFFI::HueInterpolationMethod::Shorter) == to_underlying(Gfx::HueInterpolationMethod::Shorter));
static_assert(to_underlying(RustFFI::HueInterpolationMethod::Longer) == to_underlying(Gfx::HueInterpolationMethod::Longer));
static_assert(to_underlying(RustFFI::HueInterpolationMethod::Increasing) == to_underlying(Gfx::HueInterpolationMethod::Increasing));
static_assert(to_underlying(RustFFI::HueInterpolationMethod::Decreasing) == to_underlying(Gfx::HueInterpolationMethod::Decreasing));
static_assert(sizeof(RustFFI::GradientInterpolationType) == sizeof(Gfx::GradientInterpolationMethod::Type));
static_assert(to_underlying(RustFFI::GradientInterpolationType::Rectangular) == to_underlying(Gfx::GradientInterpolationMethod::Type::Rectangular));
static_assert(to_underlying(RustFFI::GradientInterpolationType::Polar) == to_underlying(Gfx::GradientInterpolationMethod::Type::Polar));
static_assert(sizeof(RustFFI::CapStyle) == sizeof(Gfx::Path::CapStyle));
static_assert(to_underlying(RustFFI::CapStyle::Butt) == to_underlying(Gfx::Path::CapStyle::Butt));
static_assert(to_underlying(RustFFI::CapStyle::Round) == to_underlying(Gfx::Path::CapStyle::Round));
static_assert(to_underlying(RustFFI::CapStyle::Square) == to_underlying(Gfx::Path::CapStyle::Square));
static_assert(sizeof(RustFFI::JoinStyle) == sizeof(Gfx::Path::JoinStyle));
static_assert(to_underlying(RustFFI::JoinStyle::Miter) == to_underlying(Gfx::Path::JoinStyle::Miter));
static_assert(to_underlying(RustFFI::JoinStyle::Round) == to_underlying(Gfx::Path::JoinStyle::Round));
static_assert(to_underlying(RustFFI::JoinStyle::Bevel) == to_underlying(Gfx::Path::JoinStyle::Bevel));
static_assert(sizeof(RustFFI::ColorFilterType) == sizeof(Gfx::ColorFilterType));
static_assert(to_underlying(RustFFI::ColorFilterType::Brightness) == to_underlying(Gfx::ColorFilterType::Brightness));
static_assert(to_underlying(RustFFI::ColorFilterType::Contrast) == to_underlying(Gfx::ColorFilterType::Contrast));
static_assert(to_underlying(RustFFI::ColorFilterType::Grayscale) == to_underlying(Gfx::ColorFilterType::Grayscale));
static_assert(to_underlying(RustFFI::ColorFilterType::Invert) == to_underlying(Gfx::ColorFilterType::Invert));
static_assert(to_underlying(RustFFI::ColorFilterType::Opacity) == to_underlying(Gfx::ColorFilterType::Opacity));
static_assert(to_underlying(RustFFI::ColorFilterType::Saturate) == to_underlying(Gfx::ColorFilterType::Saturate));
static_assert(to_underlying(RustFFI::ColorFilterType::Sepia) == to_underlying(Gfx::ColorFilterType::Sepia));

static_assert(sizeof(RustFFI::VisualContextIndex) == sizeof(VisualContextIndex));
static_assert(sizeof(RustFFI::FontResourceId) == sizeof(FontResourceId));
static_assert(sizeof(RustFFI::ImageFrameResourceId) == sizeof(ImageFrameResourceId));
static_assert(sizeof(RustFFI::VideoSinkResourceId) == sizeof(VideoSinkResourceId));
static_assert(sizeof(RustFFI::DisplayListResourceId) == sizeof(DisplayListResourceId));
static_assert(sizeof(RustFFI::CanvasId) == sizeof(CanvasId));
static_assert(sizeof(RustFFI::CompositorContextId) == sizeof(Web::Compositor::CompositorContextId));
static_assert(sizeof(RustFFI::UniqueNodeId) == sizeof(UniqueNodeID));

static_assert(sizeof(RustFFI::OptionalFloatRect) == sizeof(Optional<Gfx::FloatRect>));
static_assert(alignof(RustFFI::OptionalFloatRect) == alignof(Optional<Gfx::FloatRect>));
static_assert(sizeof(RustFFI::OptionalColor) == sizeof(Optional<Gfx::Color>));
static_assert(alignof(RustFFI::OptionalColor) == alignof(Optional<Gfx::Color>));
static_assert(sizeof(RustFFI::OptionalU32) == sizeof(Optional<u32>));
static_assert(alignof(RustFFI::OptionalU32) == alignof(Optional<u32>));
static_assert(sizeof(RustFFI::OptionalF32) == sizeof(Optional<float>));
static_assert(alignof(RustFFI::OptionalF32) == alignof(Optional<float>));
static_assert(sizeof(RustFFI::OptionalAffineTransform) == sizeof(Optional<Gfx::AffineTransform>));
static_assert(alignof(RustFFI::OptionalAffineTransform) == alignof(Optional<Gfx::AffineTransform>));

namespace {

static Gfx::FloatMatrix4x4 matrix_from_export(float const (&values)[16])
{
    Gfx::FloatMatrix4x4 matrix;
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            matrix.elements()[row][column] = values[row * 4 + column];
    return matrix;
}

static TransformData transform_data_from_export(Layout::RustFFI::FfiVisualContextNodeExport const& node)
{
    return TransformData {
        .matrix = matrix_from_export(node.matrix),
        .origin = { node.origin[0], node.origin[1] },
        .flattens_inherited_transform = node.flattens_inherited_transform,
        .role = static_cast<TransformDataRole>(node.transform_role),
    };
}

static VisualContextData visual_context_data_from_export(Layout::RustFFI::FfiVisualContextNodeExport const& node)
{
    auto rect = [&] { return DevicePixelRect { node.rect[0], node.rect[1], node.rect[2], node.rect[3] }; };
    switch (node.kind) {
    case Layout::RustFFI::FfiVisualContextNodeKind::Scroll:
        return ScrollData { .is_sticky = node.is_sticky, .state_slot = static_cast<ScrollStateSlot>(node.state_slot) };
    case Layout::RustFFI::FfiVisualContextNodeKind::Clip:
        return ClipData { rect(), Gfx::CornerRadii { { node.corner_radii[0], node.corner_radii[1] }, { node.corner_radii[2], node.corner_radii[3] }, { node.corner_radii[4], node.corner_radii[5] }, { node.corner_radii[6], node.corner_radii[7] } } };
    case Layout::RustFFI::FfiVisualContextNodeKind::Transform:
        return transform_data_from_export(node);
    case Layout::RustFFI::FfiVisualContextNodeKind::Perspective:
        return PerspectiveData { .matrix = matrix_from_export(node.matrix), .flattens_inherited_transform = node.flattens_inherited_transform };
    case Layout::RustFFI::FfiVisualContextNodeKind::BackfaceVisibility:
        return BackfaceVisibilityData { .plane_root_index = VisualContextIndex { node.index_value }, .flattens_inherited_transform = node.flattens_inherited_transform };
    case Layout::RustFFI::FfiVisualContextNodeKind::ClipPath:
        return ClipPathData { .path = *static_cast<Gfx::Path const*>(node.path), .bounding_rect = rect(), .fill_rule = static_cast<Gfx::WindingRule>(node.winding_rule) };
    case Layout::RustFFI::FfiVisualContextNodeKind::Effects: {
        EffectsData effects { .opacity = node.opacity, .blend_mode = static_cast<Gfx::CompositingAndBlendingOperator>(node.blend_mode), .gfx_filter = {} };
        if (node.filter)
            effects.gfx_filter = *static_cast<Gfx::Filter const*>(node.filter);
        return effects;
    }
    case Layout::RustFFI::FfiVisualContextNodeKind::ScrollCompensation:
        return ScrollCompensation { .scroll_node_index = VisualContextIndex { node.index_value } };
    case Layout::RustFFI::FfiVisualContextNodeKind::AnchorScrollShift:
        return AnchorScrollShift { .scroll_node_index = VisualContextIndex { node.index_value }, .negate = node.negate, .compensate_horizontal_scroll = node.compensate_horizontal_scroll, .compensate_vertical_scroll = node.compensate_vertical_scroll };
    case Layout::RustFFI::FfiVisualContextNodeKind::Mask:
        return MaskData { .rect = rect(), .kind = static_cast<Gfx::MaskKind>(node.mask_kind), .origin = static_cast<MaskLayerOrigin>(node.mask_origin) };
    }
    VERIFY_NOT_REACHED();
}

static AccumulatedVisualContextTree materialize_rust_visual_context_tree(void const* tree)
{
    auto node_count = Layout::RustFFI::layout_arena_visual_context_tree_node_count(tree);
    VERIFY(node_count > 0);
    auto root = Layout::RustFFI::layout_arena_visual_context_tree_node(tree, 0);
    VERIFY(root.kind == Layout::RustFFI::FfiVisualContextNodeKind::Transform);
    auto visual_context_tree = Layout::RustFFI::layout_arena_visual_context_tree_root_is_visual_viewport(tree)
        ? AccumulatedVisualContextTree::create(transform_data_from_export(root))
        : AccumulatedVisualContextTree::create_with_content_root(transform_data_from_export(root));
    for (size_t index = 1; index < node_count; ++index) {
        auto node = Layout::RustFFI::layout_arena_visual_context_tree_node(tree, index);
        visual_context_tree.append(visual_context_data_from_export(node), VisualContextIndex { node.parent_index });
    }
    return visual_context_tree;
}

extern "C" void* ladybird_web_svg_path_from_path_data_ascii(u8 const*, size_t);
extern "C" void* ladybird_web_svg_path_from_path_data_utf16(char16_t const*, size_t);

extern "C" void* ladybird_web_svg_path_from_path_data_ascii(u8 const* bytes, size_t length)
{
    auto path_data = SVG::AttributeParser::parse_path_data(Utf16View { StringView { bytes, length } });
    return new Gfx::Path(path_data.to_gfx_path());
}

extern "C" void* ladybird_web_svg_path_from_path_data_utf16(char16_t const* units, size_t length)
{
    auto path_data = SVG::AttributeParser::parse_path_data(Utf16View { units, length });
    return new Gfx::Path(path_data.to_gfx_path());
}

static Layout::RustFFI::FfiRootBackgroundSource rust_root_background_source(DOM::Document const& document)
{
    Layout::RustFFI::FfiRootBackgroundSource source {};
    source.body_layout_node = Layout::RustFFI::NodeSlotId { Layout::RustFFI::INVALID_NODE_SLOT_INDEX };
    auto const* html_element = document.html_element();
    source.use_body_background_properties = html_element && html_element->unsafe_layout_node() && html_element->should_use_body_background_properties();
    if (auto const* body = document.body(); body && body->unsafe_layout_node())
        source.body_layout_node = Layout::Node::slot_id(body->unsafe_layout_node());
    return source;
}

Layout::RustFFI::FfiVisualContextHostCallbacks visual_context_host_callbacks(ViewportPaintable& viewport_paintable)
{
    return {
        .context = &viewport_paintable,
        .tree_inputs = [](void* context) -> Layout::RustFFI::FfiVisualContextTreeInputs {
            auto& viewport_paintable = *static_cast<ViewportPaintable*>(context);
            Layout::RustFFI::FfiVisualContextTreeInputs inputs {};
            inputs.device_pixels_per_css_pixel = viewport_paintable.document().page().client().device_pixels_per_css_pixel();
            auto const& visual_viewport = *viewport_paintable.document().visual_viewport();
            auto offset = visual_viewport.offset().to_type<double>();
            inputs.visual_viewport_offset_x = offset.x();
            inputs.visual_viewport_offset_y = offset.y();
            inputs.visual_viewport_scale = visual_viewport.scale();
            return inputs;
        },
        .scroll_offset = [](void*, void* paintable_shell) -> Layout::RustFFI::FfiCssPixelPoint {
            auto offset = static_cast<Paintable*>(paintable_shell)->scroll_offset();
            return { offset.x().raw_value(), offset.y().raw_value() };
        },
        .svg_transform_view_box_rect = [](void*, void* paintable_shell, Layout::RustFFI::FfiCssPixelRect* out_rect) -> bool {
            auto& paintable = *static_cast<Paintable*>(paintable_shell);
            auto const* viewport_paintable = nearest_svg_viewport_paintable_of(paintable.layout_node());
            if (!viewport_paintable)
                return false;
            auto rect = svg_viewport_user_rect(*viewport_paintable).to_type<CSSPixels>();
            *out_rect = to_ffi_css_pixel_rect(rect);
            return true;
        },
        .svg_additional_element_transform = [](void*, void* paintable_shell, float* out_values) -> bool {
            auto& paintable = *static_cast<Paintable*>(paintable_shell);
            auto const* graphics_element = as_if<SVG::SVGGraphicsElement>(paintable.layout_node().dom_node());
            if (!graphics_element)
                return false;
            auto transform = graphics_element->additional_element_transform();
            out_values[0] = transform.a();
            out_values[1] = transform.b();
            out_values[2] = transform.c();
            out_values[3] = transform.d();
            out_values[4] = transform.e();
            out_values[5] = transform.f();
            return true;
        },
        .root_background_source = [](void* context) -> Layout::RustFFI::FfiRootBackgroundSource {
            auto& viewport_paintable = *static_cast<ViewportPaintable*>(context);
            return rust_root_background_source(viewport_paintable.document());
        },
        .svg_mask_facts = [](void*, void* paintable_shell) -> Layout::RustFFI::FfiSvgMaskFacts {
            auto& paintable = *static_cast<Paintable*>(paintable_shell);
            Layout::RustFFI::FfiSvgMaskFacts facts {};
            if (auto mask_area = paintable.get_mask_area(); mask_area.has_value()) {
                facts.has_mask_area = true;
                facts.mask_area = to_ffi_css_pixel_rect(*mask_area);
                facts.mask_kind = to_underlying(paintable.get_mask_type().value_or(Gfx::MaskKind::Alpha));
            }
            if (auto clip_area = paintable.get_clip_area(); clip_area.has_value()) {
                facts.has_clip_area = true;
                facts.clip_area = to_ffi_css_pixel_rect(*clip_area);
            }
            return facts;
        },
        .resolve_effects_filter = [](void* context, void* paintable_shell) -> void* {
            auto& viewport_paintable = *static_cast<ViewportPaintable*>(context);
            auto& box = *static_cast<Paintable*>(paintable_shell);
            auto const& style_source = box.layout_node();
            if (style_source.filter().has_filters())
                box.set_filter(resolve_css_filter(style_source.filter(), box));
            else if (box.filter().has_filters() || box.filter().svg_filter_bounds.has_value())
                box.set_filter({});
            if (!box.filter().has_filters())
                return nullptr;
            auto pixel_ratio = viewport_paintable.document().page().client().device_pixels_per_css_pixel();
            auto gfx_filter = to_gfx_filter(box.filter(), pixel_ratio);
            if (!gfx_filter.has_value())
                return nullptr;
            return new Gfx::Filter(move(*gfx_filter));
        },
        .default_scroll_shift_anchor = [](void*, void* paintable_shell) -> Layout::RustFFI::NodeSlotId {
            auto& paintable_box = *static_cast<Paintable*>(paintable_shell);
            if (auto const* box = as_if<Layout::Box>(&paintable_box.layout_node())) {
                if (auto const* anchor_box = as_if<Layout::Box>(box->default_scroll_shift_anchor()))
                    return Layout::Node::slot_id(anchor_box);
            }
            return Layout::RustFFI::NodeSlotId { Layout::RustFFI::INVALID_NODE_SLOT_INDEX };
        },
    };
}

}

bool rust_assign_accumulated_visual_contexts(ViewportPaintable& viewport_paintable, bool forced_incompatible_rebuild)
{
    return Layout::RustFFI::layout_arena_assign_accumulated_visual_contexts(viewport_paintable.rust_arena().handle(), viewport_paintable.rust_slot(), visual_context_host_callbacks(viewport_paintable), forced_incompatible_rebuild);
}

AccumulatedVisualContextTree materialize_rust_main_visual_context_tree(ViewportPaintable& viewport_paintable)
{
    auto const* tree = Layout::RustFFI::layout_arena_main_visual_context_tree(viewport_paintable.rust_arena().handle());
    VERIFY(tree);
    return materialize_rust_visual_context_tree(tree);
}

void patch_rust_visual_context_nodes(ViewportPaintable& viewport_paintable, AccumulatedVisualContextTree& visual_context_tree, size_t begin, size_t end)
{
    auto const* tree = Layout::RustFFI::layout_arena_main_visual_context_tree(viewport_paintable.rust_arena().handle());
    VERIFY(tree);
    VERIFY(end <= visual_context_tree.nodes().size());
    for (size_t index = begin; index < end; ++index)
        visual_context_tree.node_at(VisualContextIndex { index }).data = visual_context_data_from_export(Layout::RustFFI::layout_arena_visual_context_tree_node(tree, index));
}

bool rust_update_accumulated_visual_context_values(ViewportPaintable& viewport_paintable, Paintable& paintable_box)
{
    return Layout::RustFFI::layout_arena_update_visual_context_values(viewport_paintable.rust_arena().handle(), paintable_box.rust_slot(), visual_context_host_callbacks(viewport_paintable));
}

Optional<TransformData> rust_compute_css_transform(Paintable const& paintable_box, double pixel_ratio)
{
    auto viewport_paintable = const_cast<DOM::Document&>(paintable_box.document()).unsafe_paintable();
    if (!viewport_paintable)
        return {};
    float matrix_values[16];
    float origin_values[2];
    if (!Layout::RustFFI::layout_arena_compute_css_transform(viewport_paintable->rust_arena().handle(), paintable_box.rust_slot(), visual_context_host_callbacks(*viewport_paintable), pixel_ratio, matrix_values, origin_values))
        return {};
    return TransformData {
        Gfx::FloatMatrix4x4(
            matrix_values[0], matrix_values[1], matrix_values[2], matrix_values[3],
            matrix_values[4], matrix_values[5], matrix_values[6], matrix_values[7],
            matrix_values[8], matrix_values[9], matrix_values[10], matrix_values[11],
            matrix_values[12], matrix_values[13], matrix_values[14], matrix_values[15]),
        { origin_values[0], origin_values[1] },
    };
}

void rust_update_visual_viewport_transform(ViewportPaintable& viewport_paintable)
{
    Layout::RustFFI::layout_arena_update_visual_viewport_transform(viewport_paintable.rust_arena().handle(), visual_context_host_callbacks(viewport_paintable));
}

void rust_refresh_scroll_state(ViewportPaintable& viewport_paintable)
{
    Layout::RustFFI::layout_arena_refresh_scroll_state(viewport_paintable.rust_arena().handle(), visual_context_host_callbacks(viewport_paintable));
}

ScrollStateSnapshot rust_scroll_state_snapshot(ViewportPaintable& viewport_paintable)
{
    auto* arena = viewport_paintable.rust_arena().handle();
    auto count = Layout::RustFFI::layout_arena_scroll_state_snapshot(arena, nullptr, 0);
    Vector<float> values;
    values.resize(count);
    if (count > 0)
        Layout::RustFFI::layout_arena_scroll_state_snapshot(arena, values.data(), values.size());
    ScrollStateSnapshot snapshot;
    for (size_t index = 0; index * 2 + 1 < values.size(); ++index)
        snapshot.set_device_offset_for_index(VisualContextIndex { index }, { values[index * 2], values[index * 2 + 1] });
    return snapshot;
}

CSSPixelPoint rust_cumulative_scroll_offset_for_node(ViewportPaintable const& viewport_paintable, VisualContextIndex scroll_node_index)
{
    i32 raw[2] = { 0, 0 };
    Layout::RustFFI::layout_arena_cumulative_scroll_offset_for_node(viewport_paintable.rust_arena().handle(), scroll_node_index.value(), raw);
    return { CSSPixels::from_raw(raw[0]), CSSPixels::from_raw(raw[1]) };
}

ScrollState materialize_rust_scroll_state(ViewportPaintable& viewport_paintable, bool& has_non_viewport_wheel_scroll_target_candidate)
{
    auto* arena = viewport_paintable.rust_arena().handle();
    ScrollState scroll_state;
    auto slot_count = Layout::RustFFI::layout_arena_scroll_state_slot_count(arena);
    for (size_t slot = 0; slot < slot_count; ++slot) {
        auto exported = Layout::RustFFI::layout_arena_scroll_state_slot_export(arena, slot);
        auto* paintable = static_cast<Paintable*>(exported.paintable_shell);
        VERIFY(paintable);
        auto registered_slot = exported.is_sticky
            ? scroll_state.register_sticky_node(VisualContextIndex { exported.node_index }, *paintable, ScrollStateSlot { exported.parent_slot })
            : scroll_state.register_scroll_node(VisualContextIndex { exported.node_index }, *paintable, ScrollStateSlot { exported.parent_slot });
        VERIFY(registered_slot.value() == slot);
        auto& state = scroll_state.state_at_slot(registered_slot);
        state.set_own_offset({ CSSPixels::from_raw(exported.own_offset.x), CSSPixels::from_raw(exported.own_offset.y) });
        if (exported.has_sticky_constraints) {
            auto side = [](i32 raw, bool present) -> Optional<CSSPixels> {
                if (!present)
                    return {};
                return CSSPixels::from_raw(raw);
            };
            state.set_sticky_constraints({
                .position_relative_to_scroll_ancestor = { CSSPixels::from_raw(exported.position_relative_to_scroll_ancestor.x), CSSPixels::from_raw(exported.position_relative_to_scroll_ancestor.y) },
                .border_box_size = { CSSPixels::from_raw(exported.border_box_size.width), CSSPixels::from_raw(exported.border_box_size.height) },
                .scrollport_size = { CSSPixels::from_raw(exported.scrollport_size.width), CSSPixels::from_raw(exported.scrollport_size.height) },
                .containing_block_region = from_ffi_css_pixel_rect(exported.containing_block_region),
                .needs_parent_offset_adjustment = exported.needs_parent_offset_adjustment,
                .insets = {
                    side(exported.sticky_insets.top, exported.sticky_insets.has_top),
                    side(exported.sticky_insets.right, exported.sticky_insets.has_right),
                    side(exported.sticky_insets.bottom, exported.sticky_insets.has_bottom),
                    side(exported.sticky_insets.left, exported.sticky_insets.has_left),
                },
            });
        }
    }
    has_non_viewport_wheel_scroll_target_candidate = Layout::RustFFI::layout_arena_scroll_state_has_non_viewport_wheel_scroll_target_candidate(arena);
    return scroll_state;
}

void mirror_rust_refresh_sticky_constraints(ViewportPaintable& viewport_paintable)
{
    Layout::RustFFI::layout_arena_refresh_sticky_constraints(viewport_paintable.rust_arena().handle());
}

void mirror_rust_clear_scroll_state(ViewportPaintable& viewport_paintable)
{
    Layout::RustFFI::layout_arena_clear_scroll_state(viewport_paintable.rust_arena().handle());
}

void mirror_rust_set_needs_to_refresh_scroll_state(ViewportPaintable& viewport_paintable, bool value)
{
    Layout::RustFFI::layout_arena_set_needs_to_refresh_scroll_state(viewport_paintable.rust_arena().handle(), value);
}

void mirror_rust_clear_visual_context_tree(ViewportPaintable& viewport_paintable)
{
    Layout::RustFFI::layout_arena_clear_visual_context_tree(viewport_paintable.rust_arena().handle());
}

void mirror_rust_reset_visual_context_state(ViewportPaintable& viewport_paintable)
{
    Layout::RustFFI::layout_arena_reset_visual_context_state(viewport_paintable.rust_arena().handle());
}

}
