/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
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
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ImagePaint.h>
#include <LibWeb/Painting/InlinePaintable.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/PaintingRustFFI.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/ShadowData.h>
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

void write_matrix(Gfx::FloatMatrix4x4 const& matrix, float (&out)[16])
{
    auto const& elements = matrix.elements();
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column)
            out[row * 4 + column] = elements[row][column];
    }
}

void write_rect(Gfx::IntRect const& rect, i32 (&out)[4])
{
    out[0] = rect.x();
    out[1] = rect.y();
    out[2] = rect.width();
    out[3] = rect.height();
}

void write_rect(DevicePixelRect const& rect, i32 (&out)[4])
{
    write_rect(rect.to_type<int>(), out);
}

static bool rust_painting_timing_enabled()
{
    static bool enabled = [] {
        auto value = Core::Environment::get("LADYBIRD_RUST_PAINTING_TIMING"sv);
        return value.has_value() && !value->is_empty() && *value != "0"sv;
    }();
    return enabled;
}

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
        .sorting_context_root_index = node.has_sorting_context_root ? Optional<VisualContextIndex> { VisualContextIndex { node.index_value } } : OptionalNone {},
        .flattens_inherited_transform = node.flattens_inherited_transform,
        .role = static_cast<TransformDataRole>(node.transform_role),
        .synthetic_plane = node.synthetic_plane,
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

static CSS::AbstractImageStyleValue const* layer_image_for(Paintable const& paintable, Layout::RustFFI::FfiLayerImageList list, u32 computed_index)
{
    auto const& layout_node = paintable.layout_node();
    switch (list) {
    case Layout::RustFFI::FfiLayerImageList::Background: {
        auto const& layers = layout_node.background_layers();
        return computed_index < layers.size() ? layers[computed_index].background_image.ptr() : nullptr;
    }
    case Layout::RustFFI::FfiLayerImageList::Mask: {
        auto const& layers = layout_node.mask_layers();
        return computed_index < layers.size() ? layers[computed_index].background_image.ptr() : nullptr;
    }
    case Layout::RustFFI::FfiLayerImageList::BorderImageSource:
        return layout_node.border_image().source.ptr();
    case Layout::RustFFI::FfiLayerImageList::DocumentBackground: {
        auto const* layers = paintable.document().background_layers();
        return layers && computed_index < layers->size() ? (*layers)[computed_index].background_image.ptr() : nullptr;
    }
    default:
        return nullptr;
    }
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
            inputs.may_have_default_scroll_shift_anchor = viewport_paintable.document().may_have_default_scroll_shift_anchor();
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

Layout::RustFFI::FfiPhysicalOverflowDirections rust_physical_overflow_directions(Paintable const& paintable_box)
{
    return Layout::RustFFI::layout_arena_physical_overflow_directions(paintable_box.rust_arena().handle(), paintable_box.rust_slot());
}

void rust_measure_scrollable_overflow(Paintable const& box_paintable)
{
    auto viewport_paintable = const_cast<DOM::Document&>(box_paintable.document()).unsafe_paintable();
    if (!viewport_paintable)
        return;
    Layout::RustFFI::FfiScrollableOverflowHostCallbacks overflow_callbacks {
        .context = nullptr,
        .layout_node_is_in_focused_text_control = [](void*, void* layout_node_shell) -> bool {
            auto const& layout_node = *static_cast<Layout::Node const*>(layout_node_shell);
            auto const* dom_node = layout_node.dom_node();
            if (!dom_node)
                return false;
            auto shadow_root = dom_node->containing_shadow_root();
            return shadow_root
                && shadow_root->is_user_agent_internal()
                && is<HTML::FormAssociatedTextControlElement>(shadow_root->host())
                && shadow_root->host()->is_focused();
        },
    };
    Layout::RustFFI::layout_arena_measure_scrollable_overflow(box_paintable.rust_arena().handle(), box_paintable.rust_slot(), visual_context_host_callbacks(*viewport_paintable), overflow_callbacks);
}

CSS::ResolvedImage rust_resolve_gradient_for_size(CSS::StyleValue const& gradient_style_value, Layout::NodeWithStyle const& layout_node, CSSPixelSize size)
{
    void const* current_color_value = nullptr;
    if (auto* dom_node = layout_node.dom_node()) {
        if (auto* element = as_if<DOM::Element>(*dom_node)) {
            if (auto const* values = element->style_group<CSS::ComputedValues::InheritedTextValues>(); values && values->color_style_value.pointer)
                current_color_value = values->color_style_value.pointer;
        }
    }

    Layout::RustFFI::FfiResolvedGradientPaint resolved {};
    ColorStopData color_stops;
    auto append_stop = [](void* stop_context, u32 color, float position) {
        auto& color_stops = *static_cast<ColorStopData*>(stop_context);
        color_stops.colors.append(Color::from_bgra(color));
        color_stops.positions.append(position);
    };
    Layout::RustFFI::layout_arena_resolve_gradient_paint_for_size(
        gradient_style_value.rust_style_value_data(),
        layout_node.color().value(),
        current_color_value,
        static_cast<u8>(to_underlying(layout_node.color_scheme())),
        size.width().raw_value(), size.height().raw_value(),
        &resolved, &color_stops, append_stop);
    color_stops.repeating = resolved.color_stops_repeating;

    Gfx::GradientInterpolationMethod interpolation_method {
        .type = static_cast<Gfx::GradientInterpolationMethod::Type>(resolved.interpolation_method[0]),
        .rectangular_color_space = static_cast<Gfx::RectangularColorSpace>(resolved.interpolation_method[1]),
        .polar_color_space = static_cast<Gfx::PolarColorSpace>(resolved.interpolation_method[2]),
        .hue_interpolation_method = static_cast<Gfx::HueInterpolationMethod>(resolved.interpolation_method[3]),
    };
    switch (resolved.kind) {
    case Layout::RustFFI::FfiResolvedGradientPaintKind::None:
        break;
    case Layout::RustFFI::FfiResolvedGradientPaintKind::Linear:
        return LinearGradientData { resolved.gradient_angle, resolved.first_stop_position, resolved.repeat_length, move(color_stops), interpolation_method };
    case Layout::RustFFI::FfiResolvedGradientPaintKind::Radial:
        return ResolvedRadialGradient {
            RadialGradientData { move(color_stops), interpolation_method },
            { CSSPixels::from_raw(resolved.size.width), CSSPixels::from_raw(resolved.size.height) },
            { CSSPixels::from_raw(resolved.center.x), CSSPixels::from_raw(resolved.center.y) },
        };
    case Layout::RustFFI::FfiResolvedGradientPaintKind::Conic:
        return ResolvedConicGradient {
            ConicGradientData { resolved.gradient_angle, move(color_stops), interpolation_method },
            { CSSPixels::from_raw(resolved.center.x), CSSPixels::from_raw(resolved.center.y) },
        };
    }
    VERIFY_NOT_REACHED();
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

void mirror_rust_reset_visual_context_state(ViewportPaintable& viewport_paintable)
{
    Layout::RustFFI::layout_arena_reset_visual_context_state(viewport_paintable.rust_arena().handle());
}

void mirror_rust_invalidate_paint_cache(Paintable const& paintable)
{
    Layout::RustFFI::layout_arena_paintable_invalidate_paint_cache(paintable.rust_arena().handle(), paintable.rust_slot(), false);
}

void rust_invalidate_propagated_text_decoration_caches(Paintable const& paintable)
{
    Layout::RustFFI::layout_arena_paintable_invalidate_paint_cache(paintable.rust_arena().handle(), paintable.rust_slot(), true);
}

void rust_build_stacking_context_tree(ViewportPaintable& viewport_paintable)
{
    Layout::RustFFI::layout_arena_build_stacking_context_tree(viewport_paintable.rust_arena().handle(), viewport_paintable.rust_slot());
}

static void dump_stacking_context_node(StringBuilder& builder, void* arena, size_t index, int indent)
{
    auto node = Layout::RustFFI::layout_arena_stacking_context_tree_node(arena, index);
    for (int i = 0; i < indent; ++i)
        builder.append(' ');
    if (auto const* paintable = static_cast<Paintable const*>(node.paintable_shell)) {
        builder.appendff("SC for {} {} (z-index: ", paintable->layout_node().debug_description(), paintable->absolute_rect());
        if (node.has_effective_z_index)
            builder.appendff("{}", node.effective_z_index);
        else
            builder.append("auto"sv);
        builder.append(')');
        if (paintable->has_css_transform())
            builder.append(", has_transform"sv);
    } else {
        builder.append("SC for (gone)"sv);
    }
    builder.append('\n');
    for (size_t child = 0; child < node.child_count; ++child)
        dump_stacking_context_node(builder, arena, Layout::RustFFI::layout_arena_stacking_context_tree_child(arena, index, child), indent + 1);
}

void dump_stacking_context_tree(StringBuilder& builder, ViewportPaintable const& viewport_paintable)
{
    auto* arena = viewport_paintable.rust_arena().handle();
    if (Layout::RustFFI::layout_arena_stacking_context_tree_node_count(arena) == 0)
        return;
    dump_stacking_context_node(builder, arena, 0, 0);
}

namespace {

Layout::RustFFI::FfiHitTestHostCallbacks hit_test_host_callbacks()
{
    return {
        .context = nullptr,
        .paintable_facts = [](void*, void* paintable_shell) -> Layout::RustFFI::FfiHitTestPaintableFacts {
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            auto const& layout_node = paintable.layout_node();
            Layout::RustFFI::FfiHitTestPaintableFacts facts {};
            facts.opacity_is_zero = layout_node.opacity() == 0;
            facts.visible_for_hit_testing = paintable.visible_for_hit_testing();
            auto dom_node = paintable.dom_node();
            facts.dom_node_has_parent = dom_node && dom_node->parent();
            facts.is_editable_or_editing_host = dom_node && dom_node->is_editable_or_editing_host();
            facts.has_resizer = paintable.has_resizer();
            facts.could_be_scrolled_horizontally = paintable.could_be_scrolled_by_wheel_event(Paintable::ScrollDirection::Horizontal);
            facts.could_be_scrolled_vertically = paintable.could_be_scrolled_by_wheel_event(Paintable::ScrollDirection::Vertical);
            if (paintable.is_svg_path_paintable()) {
                auto const& graphics_element = as<SVG::SVGGraphicsElement>(*paintable.dom_node());
                facts.svg_path_has_fill = graphics_element.fill_color().has_value();
                facts.svg_path_winding_rule = graphics_element.fill_rule().value_or(SVG::FillRule::Nonzero) == SVG::FillRule::Evenodd
                    ? to_underlying(Gfx::WindingRule::EvenOdd)
                    : to_underlying(Gfx::WindingRule::Nonzero);
            }
            if (auto const* graphics_element = as_if<SVG::SVGGraphicsElement>(paintable.dom_node().ptr()); graphics_element && graphics_element->unsafe_layout_node()) {
                for (auto child = graphics_element->unsafe_layout_node()->first_child(); child; child = child->next_sibling()) {
                    if (child->kind() == Layout::RustFFI::NodeKind::SVGMaskBox)
                        facts.svg_mask_content_units_object_bbox = as<SVG::SVGMaskElement>(*child->dom_node()).mask_content_units() == SVG::MaskContentUnits::ObjectBoundingBox;
                    else if (child->kind() == Layout::RustFFI::NodeKind::SVGClipBox)
                        facts.svg_clip_path_units_object_bbox = as<SVG::SVGClipPathElement>(*child->dom_node()).clip_path_units() == SVG::ClipPathUnits::ObjectBoundingBox;
                }
            }
            if (auto const& bounds = paintable.filter().svg_filter_bounds; bounds.has_value()) {
                facts.has_svg_filter_bounds = true;
                facts.svg_filter_bounds = to_ffi_css_pixel_rect(*bounds);
            }
            return facts;
        },
        .text_node_facts = [](void*, void* node_shell) -> Layout::RustFFI::FfiHitTestTextNodeFacts {
            auto const& text_node = *static_cast<Layout::TextNode const*>(node_shell);
            auto const& style_source = *text_node.parent();
            auto const* dom_text = text_node.dom_text();
            return {
                .parent_opacity_is_zero = style_source.opacity() == 0,
                .parent_pointer_events_none = style_source.pointer_events() == CSS::PointerEvents::None,
                .is_inert = dom_text && dom_text->is_inert(),
            };
        },
        .line_break_caret_targets = [](void*, void* paintable_shell, void* sink) {
            auto const& paintable = as<PaintableWithLines>(*static_cast<Paintable const*>(paintable_shell));
            auto* dom_node = paintable.layout_node().dom_node();
            if (!dom_node)
                return;
            for (auto* child = dom_node->first_child(); child; child = child->next_sibling()) {
                auto* br = as_if<HTML::HTMLBRElement>(*child);
                if (!br || !br->represents_empty_line())
                    continue;
                Layout::RustFFI::FfiLineBreakCaretTarget target {};
                target.caret_offset = br->index();
                target.rect = to_ffi_css_pixel_rect(paintable.caret_rect_for_child_offset(br->index()));
                Layout::RustFFI::layout_arena_hit_test_push_line_break_caret_target(sink, target);
            } },
    };
}

}

namespace {

void write_css_rect(CSSPixelRect const& rect, Layout::RustFFI::FfiCssPixelRect& out)
{
    out = to_ffi_css_pixel_rect(rect);
}

struct PaintHostContext {
    DisplayListResourceStorage& resource_storage;
    ViewportPaintable const& viewport_paintable;
    u64 paint_generation_id { 0 };
    double device_pixels_per_css_pixel { 1 };
};

static void write_image_paint_facts(ImagePaint const& paint, PaintHostContext& context, Layout::RustFFI::FfiImagePaintFacts& facts)
{
    paint.value.visit(
        [&](ImagePaint::DecodedFrame const& decoded_frame) {
            facts.image_paint_kind = Layout::RustFFI::FfiImagePaintKind::DecodedFrame;
            facts.frame_id = context.resource_storage.add_image_frame(decoded_frame.frame).value();
            facts.natural_width = decoded_frame.natural_size.width();
            facts.natural_height = decoded_frame.natural_size.height();
        },
        [&](ImagePaint::NestedDisplayList const& nested) {
            facts.image_paint_kind = Layout::RustFFI::FfiImagePaintKind::NestedDisplayList;
            facts.nested_display_list_id = context.resource_storage.add_display_list(nested.resource.display_list, nested.resource.visual_context_tree).value();
            facts.list_width = nested.list_size.width();
            facts.list_height = nested.list_size.height();
        },
        [](auto const&) { VERIFY_NOT_REACHED(); });
}

Layout::RustFFI::FfiPaintHostCallbacks paint_host_callbacks(PaintHostContext& context)
{
    return {
        .context = &context,
        .async_scroll_facts = [](void*, void* paintable_shell) -> Layout::RustFFI::FfiAsyncScrollFacts {
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            auto const& layout_node = paintable.layout_node();
            Layout::RustFFI::FfiAsyncScrollFacts facts {};
            auto dom_node = paintable.dom_node();
            facts.is_nested_navigable_container = dom_node && dom_node->is_navigable_container() && as<HTML::NavigableContainer const>(*dom_node).content_navigable();
            if (paintable.is_viewport_paintable()) {
                facts.scroll_node_kind = Layout::RustFFI::FfiScrollNodeKind::Viewport;
                facts.scrollable_node_id = paintable.document().unique_id().value();
            } else if (layout_node.generated_for_pseudo_element().has_value()) {
                facts.scroll_node_kind = Layout::RustFFI::FfiScrollNodeKind::PseudoElement;
                facts.scrollable_node_id = layout_node.pseudo_element_generator()->unique_id().value();
            } else if (dom_node && is<DOM::Element>(*dom_node)) {
                facts.scroll_node_kind = Layout::RustFFI::FfiScrollNodeKind::Element;
                facts.scrollable_node_id = dom_node->unique_id().value();
            }
            facts.pseudo_element_type = layout_node.generated_for_pseudo_element().has_value() ? static_cast<u8>(to_underlying(*layout_node.generated_for_pseudo_element())) : 0;
            facts.inside_blocking_wheel_event_handler = dom_node && dom_node->inside_blocking_wheel_event_handler();
            facts.records_viewport_scrollbars = paintable.is_viewport_paintable()
                && paintable.document().page().async_scrolling_enabled()
                && should_paint_viewport_scrollbars()
                && layout_node.scrollbar_width() != CSS::ScrollbarWidth::None;
            if (facts.records_viewport_scrollbars) {
                auto scrollbar_colors = scrollbar_colors_for_paint(paintable);
                auto const& metrics = paintable.document().page().chrome_metrics();
                size_t index = 0;
                for (auto direction : { Paintable::ScrollDirection::Vertical, Paintable::ScrollDirection::Horizontal }) {
                    auto& out = facts.viewport_scrollbars[index++];
                    auto scrollbar_data = paintable.compute_scrollbar_data(direction, metrics, nullptr, Paintable::ScrollbarSizing::Regular);
                    if (!scrollbar_data.has_value())
                        continue;
                    auto expanded_scrollbar_data = paintable.compute_scrollbar_data(direction, metrics, nullptr, Paintable::ScrollbarSizing::Enlarged);
                    VERIFY(expanded_scrollbar_data.has_value());
                    out.present = true;
                    write_css_rect(scrollbar_data->gutter_rect, out.gutter_rect);
                    write_css_rect(scrollbar_data->thumb_rect, out.thumb_rect);
                    write_css_rect(expanded_scrollbar_data->gutter_rect, out.expanded_gutter_rect);
                    write_css_rect(expanded_scrollbar_data->thumb_rect, out.expanded_thumb_rect);
                    out.scroll_size = scrollbar_data->thumb_travel_to_scroll_ratio.to_double();
                    out.expanded_scroll_size = expanded_scrollbar_data->thumb_travel_to_scroll_ratio.to_double();
                    out.thumb_color = scrollbar_colors.thumb_color.value();
                    out.track_color = scrollbar_colors.track_color.value();
                }
            }
            return facts;
        },
        .overlay_facts = [](void*, void* paintable_shell) -> Layout::RustFFI::FfiOverlayFacts {
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            auto const& layout_node = paintable.layout_node();
            auto const& metrics = paintable.document().page().chrome_metrics();
            Layout::RustFFI::FfiOverlayFacts facts {};
            facts.paints_scrollbars = ((should_paint_viewport_scrollbars() && !paintable.document().page().async_scrolling_enabled()) || !paintable.is_viewport_paintable())
                && layout_node.scrollbar_width() != CSS::ScrollbarWidth::None;
            if (facts.paints_scrollbars) {
                auto scrollbar_colors = scrollbar_colors_for_paint(paintable);
                facts.thumb_color = scrollbar_colors.thumb_color.value();
                facts.track_color = scrollbar_colors.track_color.value();
                size_t index = 0;
                for (auto direction : { Paintable::ScrollDirection::Vertical, Paintable::ScrollDirection::Horizontal }) {
                    auto& out = facts.scrollbars[index++];
                    auto scrollbar_data = paintable.compute_scrollbar_data(direction, metrics);
                    if (!scrollbar_data.has_value())
                        continue;
                    out.present = true;
                    write_css_rect(scrollbar_data->gutter_rect, out.gutter_rect);
                    write_css_rect(scrollbar_data->thumb_rect, out.thumb_rect);
                    write_css_rect(scrollbar_data->track_rect, out.track_rect);
                    out.thumb_travel_to_scroll_ratio = scrollbar_data->thumb_travel_to_scroll_ratio.to_double();
                }
            }
            if (auto resizer_rect = paintable.absolute_resizer_rect(metrics); resizer_rect.has_value()) {
                facts.has_resizer_rect = true;
                write_css_rect(*resizer_rect, facts.resizer_rect);
                facts.resize_gripper_padding = metrics.resize_gripper_padding.raw_value();
            }
            if (paintable.is_viewport_paintable()) {
                if (auto navigable = paintable.document().navigable()) {
                    if (auto handler = navigable->event_handler().middle_button_scroll_handler(); handler.has_value()) {
                        facts.middle_button_scroll_active = true;
                        facts.middle_button_scroll_origin.x = handler->origin().x().raw_value();
                        facts.middle_button_scroll_origin.y = handler->origin().y().raw_value();
                    }
                }
            }
            return facts;
        },
        .outline_facts = [](void*, void* paintable_shell) -> Layout::RustFFI::FfiOutlineFacts {
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            Layout::RustFFI::FfiOutlineFacts facts {};
            if (auto const* area_element = as_if<HTML::HTMLAreaElement>(paintable.document().focused_area().ptr())) {
                auto const* map_element = area_element->first_ancestor_of_type<HTML::HTMLMapElement>();
                auto const* image_element = as_if<HTML::HTMLImageElement>(paintable.dom_node().ptr());
                if (map_element && image_element && map_element->first_painted_image_with_focusable_shapes().ptr() == image_element) {
                    if (auto area_computed_values = area_element->computed_style(); area_computed_values && area_computed_values->outline_style() == CSS::OutlineStyle::Auto) {
                        if (auto outline_data = paintable.outline_data(*area_computed_values); outline_data.has_value()) {
                            if (auto path = area_element->shape_path(paintable.absolute_rect().size()); path.has_value()) {
                                facts.paints_focused_area_outline = true;
                                facts.focused_area_path = new Gfx::Path(path.release_value());
                                facts.focused_area_color = outline_data->color.value();
                                facts.focused_area_width = outline_data->width.raw_value();
                            }
                        }
                    }
                }
            }
            return facts;
        },
        .image_intrinsic_facts = [](void*, void* paintable_shell, Layout::RustFFI::FfiLayerImageList list, u32 computed_index) -> Layout::RustFFI::FfiImageIntrinsicFacts {
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            auto const& document = paintable.document();
            Layout::RustFFI::FfiImageIntrinsicFacts facts {};
            auto const* image = layer_image_for(paintable, list, computed_index);
            if (!image)
                return facts;
            facts.is_paintable = image->is_paintable(document);
            if (auto width = image->natural_width(document); width.has_value()) {
                facts.has_natural_width = true;
                facts.natural_width = width->raw_value();
            }
            if (auto height = image->natural_height(document); height.has_value()) {
                facts.has_natural_height = true;
                facts.natural_height = height->raw_value();
            }
            if (auto aspect_ratio = image->natural_aspect_ratio(document); aspect_ratio.has_value()) {
                facts.has_natural_aspect_ratio = true;
                facts.natural_aspect_ratio_numerator = aspect_ratio->numerator().raw_value();
                facts.natural_aspect_ratio_denominator = aspect_ratio->denominator().raw_value();
            }
            if (auto const* image_set = as_if<CSS::ImageSetStyleValue>(*image)) {
                if (auto const* selected_image = image_set->selected_image()) {
                    facts.has_selected_image_value = true;
                    facts.selected_image_value = selected_image->rust_style_value_data();
                }
            }
            return facts;
        },
        .root_background_source = [](void* context_pointer) -> Layout::RustFFI::FfiRootBackgroundSource {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            return rust_root_background_source(context.viewport_paintable.document());
        },
        .text_control_selection = [](void*, void* layout_node_shell) -> Layout::RustFFI::FfiTextControlSelection {
            Layout::RustFFI::FfiTextControlSelection result {};
            auto const* layout_node = static_cast<Layout::Node const*>(layout_node_shell);
            if (!layout_node)
                return result;
            auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(layout_node->document().focused_area().ptr());
            if (!text_control)
                return result;
            if (GC::Ptr { layout_node->dom_node() } != text_control->form_associated_element_to_text_node())
                return result;
            auto selection_start = text_control->selection_start();
            auto selection_end = text_control->selection_end();
            if (selection_start == selection_end)
                return result;
            result.has_selection = true;
            result.start = selection_start;
            result.end = selection_end;
            return result;
        },
        .selection_style_facts = [](void*, void* layout_node_shell, void* shadow_sink) -> Layout::RustFFI::FfiSelectionStyleFacts {
            Layout::RustFFI::FfiSelectionStyleFacts facts {};
            auto const* text_node = as_if<Layout::TextNode>(static_cast<Layout::Node const*>(layout_node_shell));
            if (!text_node)
                return facts;
            auto style = Paintable::selection_style_for_node(*text_node, text_node->dom_text());
            facts.background_color = style.background_color.value();
            if (style.text_color.has_value()) {
                facts.has_text_color = true;
                facts.text_color = style.text_color->value();
            }
            if (style.text_shadow.has_value()) {
                facts.has_text_shadow = true;
                for (auto const& layer : *style.text_shadow)
                    Layout::RustFFI::layout_arena_paint_push_selection_shadow(shadow_sink, layer.color.value(), layer.offset_x.raw_value(), layer.offset_y.raw_value(), layer.blur_radius.raw_value());
            }
            if (style.text_decoration.has_value()) {
                facts.has_text_decoration = true;
                facts.text_decoration_line_count = min(style.text_decoration->line.size(), array_size(facts.text_decoration_lines));
                for (size_t i = 0; i < facts.text_decoration_line_count; ++i)
                    facts.text_decoration_lines[i] = to_underlying(style.text_decoration->line[i]);
                facts.text_decoration_style = to_underlying(style.text_decoration->style);
                facts.text_decoration_color = style.text_decoration->color.value();
            }
            return facts;
        },
        .register_font = [](void* context_pointer, void const* font) -> u64 {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            return context.resource_storage.add_font(*static_cast<Gfx::Font const*>(font)).value();
        },
        .cursor_facts = [](void*, void* paintable_shell, void* owner_shell) -> Layout::RustFFI::FfiCursorFacts {
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            Layout::RustFFI::FfiCursorFacts facts {};
            if (!paintable.document().cursor_position())
                return facts;
            auto caret = is<InlinePaintable>(paintable)
                ? static_cast<InlinePaintable const&>(paintable).resolve_empty_editable_caret_paint()
                : as<PaintableWithLines>(paintable).resolve_caret_paint(static_cast<InlinePaintable const*>(owner_shell));
            if (!caret.has_value())
                return facts;
            facts.paints = true;
            write_css_rect(caret->rect, facts.rect);
            facts.color = caret->color.value();
            return facts;
        },
        .layer_image_prepare = [](void*, void* paintable_shell, Layout::RustFFI::FfiLayerImageList list, u32 computed_index) -> Layout::RustFFI::FfiLayerImagePrepareFacts {
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            Layout::RustFFI::FfiLayerImagePrepareFacts facts {};
            auto const* image = layer_image_for(paintable, list, computed_index);
            if (!image)
                return facts;
            facts.is_image_style_value = is<CSS::ImageStyleValue>(*image);
            if (auto color = image->color_if_single_pixel_bitmap(paintable.document()); color.has_value()) {
                facts.has_single_pixel_color = true;
                facts.single_pixel_color = color->value();
            }
            return facts;
        },
        .layer_image_nested_display_list = [](void* context_pointer, void* paintable_shell, Layout::RustFFI::FfiLayerImageList list, u32 computed_index, i32 const* dest) -> Layout::RustFFI::FfiLayerImageNestedDisplayListFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            Layout::RustFFI::FfiLayerImageNestedDisplayListFacts facts {};
            auto const* image = layer_image_for(paintable, list, computed_index);
            if (!image || !is<CSS::ImageStyleValue>(*image))
                return facts;
            DevicePixelRect dest_rect { DevicePixels(dest[0]), DevicePixels(dest[1]), DevicePixels(dest[2]), DevicePixels(dest[3]) };
            auto color_scheme = paintable.layout_node().color_scheme();
            if (auto display_list = static_cast<CSS::ImageStyleValue const&>(*image).record_display_list(context.resource_storage, paintable.document(), dest_rect, color_scheme); display_list.has_value()) {
                facts.has_nested_display_list = true;
                facts.nested_display_list_id = context.resource_storage.add_display_list(display_list->display_list, display_list->visual_context_tree).value();
            }
            return facts;
        },
        .layer_image_current_frame = [](void* context_pointer, void* paintable_shell, Layout::RustFFI::FfiLayerImageList list, u32 computed_index, i32 const* dest) -> Layout::RustFFI::FfiLayerImageFrameFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            Layout::RustFFI::FfiLayerImageFrameFacts facts {};
            auto const* image = layer_image_for(paintable, list, computed_index);
            if (!image || !is<CSS::ImageStyleValue>(*image))
                return facts;
            DevicePixelRect dest_rect { DevicePixels(dest[0]), DevicePixels(dest[1]), DevicePixels(dest[2]), DevicePixels(dest[3]) };
            if (auto frame = static_cast<CSS::ImageStyleValue const&>(*image).current_frame(paintable.document(), dest_rect); frame.has_value()) {
                facts.has_frame = true;
                facts.frame_id = context.resource_storage.add_image_frame(*frame).value();
                facts.frame_width = frame->size().width();
                facts.frame_height = frame->size().height();
            }
            return facts;
        },
        .layer_image_paint = [](void* context_pointer, void* paintable_shell, Layout::RustFFI::FfiLayerImageList list, u32 computed_index, float const* dest, i32 const* css_size_raw, u8 image_rendering_raw, float const* accumulated_scale_raw) -> Layout::RustFFI::FfiImagePaintFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            auto const& layout_node = paintable.layout_node();
            Layout::RustFFI::FfiImagePaintFacts facts {};
            auto const* image_pointer = layer_image_for(paintable, list, computed_index);
            if (!image_pointer)
                return facts;
            auto const& image = *image_pointer;
            Gfx::FloatRect dest_rect { dest[0], dest[1], dest[2], dest[3] };
            Gfx::FloatSize accumulated_scale { accumulated_scale_raw[0], accumulated_scale_raw[1] };
            ImagePaintRequest request {
                .document = paintable.document(),
                .dest_rect = dest_rect,
                .image_rendering = static_cast<CSS::ImageRendering>(image_rendering_raw),
                .color_scheme = layout_node.color_scheme(),
                .accumulated_scale = accumulated_scale,
                .resource_storage = context.resource_storage,
            };
            CSSPixelSize css_size { CSSPixels::from_raw(css_size_raw[0]), CSSPixels::from_raw(css_size_raw[1]) };
            auto paint = image.image_paint(request, image.resolve_for_size(layout_node, css_size));
            if (paint.has_value())
                write_image_paint_facts(*paint, context, facts);
            return facts;
        },
        .replaced_paint_facts = [](void* context_pointer, void* paintable_shell) -> Layout::RustFFI::FfiReplacedPaintFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            auto const& layout_node = paintable.layout_node();
            Layout::RustFFI::FfiReplacedPaintFacts facts {};
            if (paintable.kind() == Layout::RustFFI::PaintableKind::ImagePaintable) {
                auto const& image_provider = static_cast<Layout::Box const&>(layout_node).image_provider();
                facts.has_decoded_image_data = image_provider.decoded_image_data() != nullptr;
                if (auto width = image_provider.intrinsic_width(); width.has_value()) {
                    facts.has_natural_width = true;
                    facts.natural_width = width->raw_value();
                }
                if (auto height = image_provider.intrinsic_height(); height.has_value()) {
                    facts.has_natural_height = true;
                    facts.natural_height = height->raw_value();
                }
                if (auto aspect_ratio = image_provider.intrinsic_aspect_ratio(); aspect_ratio.has_value()) {
                    facts.has_natural_aspect_ratio = true;
                    facts.natural_aspect_ratio_numerator = aspect_ratio->numerator().raw_value();
                    facts.natural_aspect_ratio_denominator = aspect_ratio->denominator().raw_value();
                }
                if (paintable.selection_state() != Paintable::SelectionState::None)
                    facts.selection_background_color = paintable.selection_style().background_color.value();
            } else if (paintable.kind() == Layout::RustFFI::PaintableKind::CanvasPaintable) {
                auto& canvas_element = as<HTML::HTMLCanvasElement>(*paintable.dom_node());
                if (auto content_size = canvas_element.canvas_surface_content_size(); content_size.has_value()) {
                    facts.has_canvas_content = true;
                    facts.canvas_content_width = content_size->width();
                    facts.canvas_content_height = content_size->height();
                    facts.canvas_id = canvas_element.canvas_id().value().value();
                    facts.canvas_content_generation = canvas_element.content_generation();
                }
            } else if (paintable.kind() == Layout::RustFFI::PaintableKind::VideoPaintable) {
                auto const& video_element = as<HTML::HTMLVideoElement>(*paintable.dom_node());
                switch (video_element.current_representation()) {
                case HTML::HTMLVideoElement::Representation::FirstVideoFrame:
                case HTML::HTMLVideoElement::Representation::VideoFrame: {
                    facts.video_representation = Layout::RustFFI::FfiVideoRepresentation::VideoFrame;
                    auto sink_handle = video_element.video_sink_handle();
                    if (sink_handle.has_value() && video_element.natural_media_size().has_value()) {
                        facts.has_video_frame = true;
                        auto src_size = video_element.natural_media_size()->to_type<int>();
                        facts.video_src_width = src_size.width();
                        facts.video_src_height = src_size.height();
                        facts.video_sink_storage_id = context.resource_storage.add_video_sink(video_element.video_sink_resource_id().value(), *sink_handle).value();
                    }
                    break;
                }
                case HTML::HTMLVideoElement::Representation::PosterFrame: {
                    facts.video_representation = Layout::RustFFI::FfiVideoRepresentation::PosterFrame;
                    if (auto const& poster_frame = video_element.poster_frame()) {
                        facts.has_poster_frame = true;
                        auto frame = Gfx::DecodedImageFrame { *poster_frame };
                        facts.poster_width = frame.size().width();
                        facts.poster_height = frame.size().height();
                        facts.poster_frame_id = context.resource_storage.add_image_frame(move(frame)).value();
                    }
                    break;
                }
                case HTML::HTMLVideoElement::Representation::TransparentBlack:
                    facts.video_representation = Layout::RustFFI::FfiVideoRepresentation::TransparentBlack;
                    break;
                }
            } else if (paintable.is_navigable_container_viewport_paintable()) {
                auto const& navigable_container = as<HTML::NavigableContainer>(*paintable.dom_node());
                auto content_navigable = navigable_container.content_navigable();
                VERIFY(content_navigable);
                auto& local_navigable = as<HTML::LocalNavigable>(*content_navigable);
                if (!local_navigable.has_been_destroyed()) {
                    auto context_id = paintable.document().page().client().compositor_context_id_for_remote_child_frame(content_navigable->id());
                    if (!context_id.has_value() && local_navigable.has_compositor_context()) {
                        auto* hosted_document = const_cast<DOM::Document*>(navigable_container.content_document_without_origin_check());
                        if (!hosted_document || !hosted_document->is_render_blocked())
                            context_id = local_navigable.compositor_context().id();
                    }
                    if (context_id.has_value()) {
                        facts.has_composited_context = true;
                        facts.composited_context_id = context_id->value();
                    }
                }
            } else if (paintable.kind() == Layout::RustFFI::PaintableKind::CheckBoxPaintable || paintable.kind() == Layout::RustFFI::PaintableKind::RadioButtonPaintable) {
                auto const& input = as<HTML::HTMLInputElement const>(*paintable.dom_node());
                facts.enabled = input.enabled();
                facts.checked = input.checked();
                facts.indeterminate = input.indeterminate();
                facts.being_activated = input.is_being_activated();
                auto color_scheme = layout_node.color_scheme();
                facts.canvas_color = CSS::SystemColor::canvas(color_scheme).value();
                facts.canvas_text_color = CSS::SystemColor::canvas_text(color_scheme).value();
                facts.accent_color = layout_node.accent_color().value_or(CSS::SystemColor::accent_color(color_scheme)).value();
            }
            return facts;
        },
        .replaced_image_paint = [](void* context_pointer, void* paintable_shell, float const* dest, float const* accumulated_scale_raw) -> Layout::RustFFI::FfiImagePaintFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            auto const& layout_node = paintable.layout_node();
            Layout::RustFFI::FfiImagePaintFacts facts {};
            GC::Ptr<HTML::DecodedImageData> decoded_image_data;
            if (paintable.kind() == Layout::RustFFI::PaintableKind::ImagePaintable)
                decoded_image_data = static_cast<Layout::Box const&>(layout_node).image_provider().decoded_image_data();
            else if (paintable.kind() == Layout::RustFFI::PaintableKind::SVGImagePaintable) {
                auto const& image_provider = as<SVG::SVGImageElement>(*paintable.layout_node().dom_node());
                decoded_image_data = image_provider.decoded_image_data();
            }
            if (!decoded_image_data)
                return facts;
            Gfx::FloatRect dest_rect { dest[0], dest[1], dest[2], dest[3] };
            Gfx::FloatSize accumulated_scale { accumulated_scale_raw[0], accumulated_scale_raw[1] };
            ImagePaintRequest request {
                .document = paintable.document(),
                .dest_rect = dest_rect,
                .image_rendering = layout_node.image_rendering(),
                .color_scheme = layout_node.color_scheme(),
                .accumulated_scale = accumulated_scale,
                .resource_storage = context.resource_storage,
            };
            auto paint = decoded_image_data->image_paint(request);
            if (paint.has_value())
                write_image_paint_facts(*paint, context, facts);
            return facts;
        },
        .backdrop_filter_bytes = [](void* context_pointer, void* paintable_shell, void* sink) -> bool {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            auto const& backdrop_filter = paintable.layout_node().backdrop_filter();
            if (!backdrop_filter.has_filters())
                return false;
            auto resolved = resolve_css_filter(backdrop_filter, paintable);
            auto gfx_filter = to_gfx_filter(resolved, context.device_pixels_per_css_pixel);
            if (!gfx_filter.has_value())
                return false;
            auto filter_data = Gfx::serialize_filter(*gfx_filter, [&](Gfx::DecodedImageFrame const& frame) {
                return context.resource_storage.add_image_frame(frame).value();
            });
            Layout::RustFFI::layout_arena_paint_push_bytes(sink, filter_data.data(), filter_data.size());
            return true;
        },
        .nested_display_list_from_bytes = [](void* context_pointer, u8 const* bytes, size_t length, i32 content_offset_x, i32 content_offset_y) -> u64 {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto visual_context_tree = AccumulatedVisualContextTree::create_with_content_offset({ content_offset_x, content_offset_y });
            auto command_bytes = MUST(ByteBuffer::copy(bytes, length));
            auto display_list = DisplayList::create_from_command_bytes(visual_context_tree, move(command_bytes));
            return context.resource_storage.add_display_list(move(display_list), visual_context_tree).value();
        },
        .svg_host_facts = [](void*, void* paintable_shell) -> Layout::RustFFI::FfiSvgHostFacts {
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            Layout::RustFFI::FfiSvgHostFacts facts {};
            if (paintable.is_svg_path_paintable()) {
                facts.percentage_basis = as<SVG::SVGGraphicsElement>(*paintable.dom_node()).viewport_percentage_basis().raw_value();
                if (auto const* viewport_paintable = nearest_svg_viewport_paintable_of(paintable.layout_node())) {
                    facts.has_viewport = true;
                    auto viewport = svg_viewport_user_rect(*viewport_paintable);
                    facts.viewport[0] = viewport.x();
                    facts.viewport[1] = viewport.y();
                    facts.viewport[2] = viewport.width();
                    facts.viewport[3] = viewport.height();
                }
            }
            if (paintable.kind() == Layout::RustFFI::PaintableKind::SVGImagePaintable) {
                auto const& image_provider = as<SVG::SVGImageElement>(*paintable.layout_node().dom_node());
                facts.has_decoded_image_data = image_provider.decoded_image_data() != nullptr;
                if (auto natural_size = image_provider.intrinsic_size(); natural_size.has_value()) {
                    facts.has_natural_size = true;
                    facts.natural_width = natural_size->width().to_float();
                    facts.natural_height = natural_size->height().to_float();
                }
            }
            return facts;
        },
        .svg_paint_style = [](void* context_pointer, void* paintable_shell, bool is_stroke, Layout::RustFFI::FfiSvgPaintContext const* ffi_paint_context, void* sink) -> Layout::RustFFI::FfiSvgPaintStyle {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
            Layout::RustFFI::FfiSvgPaintStyle style {};
            auto const& viewport = ffi_paint_context->viewport;
            auto const& path_bounding_box = ffi_paint_context->path_bounding_box;
            auto const& paint_transform = ffi_paint_context->paint_transform;
            auto const& content_scale = ffi_paint_context->content_scale;
            SVG::SVGPaintContext paint_context {
                .viewport = { viewport[0], viewport[1], viewport[2], viewport[3] },
                .path_bounding_box = { path_bounding_box[0], path_bounding_box[1], path_bounding_box[2], path_bounding_box[3] },
                .paint_transform = Gfx::AffineTransform { paint_transform[0], paint_transform[1], paint_transform[2], paint_transform[3], paint_transform[4], paint_transform[5] },
                .content_scale = { content_scale[0], content_scale[1] },
            };
            auto const& graphics_element = as<SVG::SVGGraphicsElement>(*paintable.dom_node());
            auto paint_server = is_stroke ? graphics_element.stroke_paint_server(paint_context, context.device_pixels_per_css_pixel) : graphics_element.fill_paint_server(paint_context, context.device_pixels_per_css_pixel);
            if (!paint_server.has_value())
                return style;
            auto write_transform = [](Gfx::AffineTransform const& transform, float (&out)[6]) {
                out[0] = transform.a();
                out[1] = transform.b();
                out[2] = transform.c();
                out[3] = transform.d();
                out[4] = transform.e();
                out[5] = transform.f();
            };
            auto write_gradient = [&](GradientPaintStyle const& gradient) {
                if (auto const& transform = gradient.gradient_transform(); transform.has_value()) {
                    style.has_gradient_transform = true;
                    write_transform(*transform, style.gradient_transform);
                }
                style.spread_method = static_cast<Layout::RustFFI::FfiSvgGradientSpreadMethod>(to_underlying(gradient.spread_method()));
                style.color_space = to_underlying(gradient.color_space());
                auto colors = gradient.color_stop_colors();
                auto positions = gradient.color_stop_positions();
                for (size_t i = 0; i < colors.size(); ++i)
                    Layout::RustFFI::layout_arena_paint_push_color_stop(sink, colors[i].value(), positions[i]);
            };
            paint_server->visit(
                [&](PaintStyle const& paint_style) {
                    paint_style.visit(
                        [&](LinearGradientPaintStyle const& linear) {
                            style.kind = Layout::RustFFI::FfiSvgPaintStyleKind::LinearGradient;
                            write_gradient(linear);
                            style.start[0] = linear.start_point().x();
                            style.start[1] = linear.start_point().y();
                            style.end[0] = linear.end_point().x();
                            style.end[1] = linear.end_point().y();
                        },
                        [&](RadialGradientPaintStyle const& radial) {
                            style.kind = Layout::RustFFI::FfiSvgPaintStyleKind::RadialGradient;
                            write_gradient(radial);
                            style.start[0] = radial.start_center().x();
                            style.start[1] = radial.start_center().y();
                            style.start_radius = radial.start_radius();
                            style.end[0] = radial.end_center().x();
                            style.end[1] = radial.end_center().y();
                            style.end_radius = radial.end_radius();
                        },
                        [&](PatternPaintStyle const&) {
                            VERIFY_NOT_REACHED();
                        });
                },
                [&](SVG::SVGGraphicsElement::PatternPaintServer const& pattern) {
                    style.kind = Layout::RustFFI::FfiSvgPaintStyleKind::Pattern;
                    style.pattern_paintable = pattern.pattern_paintable->rust_slot();
                    write_matrix(pattern.tile_content_transform.matrix, style.tile_content_transform);
                    style.tile_rect[0] = pattern.tile_rect.x();
                    style.tile_rect[1] = pattern.tile_rect.y();
                    style.tile_rect[2] = pattern.tile_rect.width();
                    style.tile_rect[3] = pattern.tile_rect.height();
                    style.content_scale[0] = pattern.content_scale.width();
                    style.content_scale[1] = pattern.content_scale.height();
                    if (pattern.device_pattern_transform.has_value()) {
                        style.has_pattern_transform = true;
                        write_transform(*pattern.device_pattern_transform, style.pattern_transform);
                    }
                });
            return style;
        },
        .accumulated_2d_scale = [](void* context_pointer, void const* rust_visual_context_tree, size_t visual_context_index, float* out) {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto scale = rust_visual_context_tree
                ? materialize_rust_visual_context_tree(rust_visual_context_tree).accumulated_2d_scale(VisualContextIndex { visual_context_index }, ScrollStateSnapshot {}, AccumulatedVisualContextTree::IncludeVisualViewportTransform::No)
                : context.viewport_paintable.visual_context_tree().accumulated_2d_scale(VisualContextIndex { visual_context_index }, ScrollStateSnapshot {}, AccumulatedVisualContextTree::IncludeVisualViewportTransform::No);
            out[0] = scale.width();
            out[1] = scale.height(); },
        .materialize_visual_context_tree = [](void*, void const* tree) -> void* {
            return new AccumulatedVisualContextTree(materialize_rust_visual_context_tree(tree));
        },
        .nested_display_list_from_tree = [](void* context_pointer, u8 const* bytes, size_t length, void* tree_handle, u64 const* mask_pairs, size_t mask_pair_count) -> u64 {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            OwnPtr<AccumulatedVisualContextTree> visual_context_tree = adopt_own(*static_cast<AccumulatedVisualContextTree*>(tree_handle));
            auto command_bytes = MUST(ByteBuffer::copy(bytes, length));
            auto display_list = DisplayList::create_from_command_bytes(*visual_context_tree, move(command_bytes));
            for (size_t i = 0; i < mask_pair_count; ++i)
                display_list->set_mask_display_list_id(VisualContextIndex { static_cast<size_t>(mask_pairs[i * 2]) }, DisplayListResourceId { mask_pairs[i * 2 + 1] });
            return context.resource_storage.add_display_list(move(display_list), *visual_context_tree).value();
        },
        .overlay_label = [](void* context_pointer, void* paintable_shell, u16 const* text_units, size_t text_unit_count, size_t utf16_fly_string_raw, float css_font_size, void* sink) -> Layout::RustFFI::FfiOverlayLabelFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            Utf16String text;
            if (paintable_shell) {
                auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
                auto border_rect = paintable.absolute_border_box_rect();
                Utf16StringBuilder builder;
                builder.appendff("{}", paintable.debug_description());
                builder.appendff(" {}x{} @ {},{}", border_rect.width(), border_rect.height(), border_rect.x(), border_rect.y());
                text = builder.to_string();
            } else if (utf16_fly_string_raw) {
                text = Utf16FlyString::from_raw(utf16_fly_string_raw).to_utf16_string();
            } else {
                text = Utf16String::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(text_units), text_unit_count });
            }
            auto font = Platform::FontPlugin::the().default_font(css_font_size);
            auto label_font = font->with_size(font->point_size() * context.device_pixels_per_css_pixel);
            auto glyph_run = Gfx::shape_text({}, 0, 0, text.utf16_view(), label_font, Gfx::GlyphRun::TextType::Ltr);
            for (auto const& glyph : glyph_run->glyphs())
                Layout::RustFFI::layout_arena_paint_push_overlay_glyph(sink, glyph.glyph_id, glyph.position.x(), glyph.position.y());
            auto bounds = glyph_run->bounding_box(1.0f);
            auto metrics = label_font->pixel_metrics();
            return Layout::RustFFI::FfiOverlayLabelFacts {
                .font_id = context.resource_storage.add_font(glyph_run->font()).value(),
                .css_width = font->width(text),
                .css_pixel_size = font->pixel_size(),
                .device_glyph_width = glyph_run->width(),
                .device_ascent = metrics.ascent,
                .device_descent = metrics.descent,
                .blob_bounds = { bounds.x(), bounds.y(), bounds.width(), bounds.height() },
            };
        },
    };
}

}

RefPtr<DisplayList> record_rust_display_list(ViewportPaintable& viewport_paintable, DisplayList const& placeholder_display_list, DisplayListResourceStorage& resource_storage, PaintCommandCacheMode cache_mode, HTML::PaintConfig const& config, InspectorOverlayInputs const& overlay_inputs)
{
    static u64 s_next_paint_generation_id = 0;
    auto paint_generation_id = s_next_paint_generation_id++;
    auto* arena = viewport_paintable.rust_arena().handle();
    auto& document = viewport_paintable.document();
    auto device_pixels_per_css_pixel = document.page().client().device_pixels_per_css_pixel();
    auto device_viewport_rect = document.page().css_to_device_rect(document.viewport_rect());
    auto wheel_event_region_state = viewport_paintable.collect_root_blocking_wheel_event_regions();
    Layout::RustFFI::FfiRecordingInputs inputs {};
    if (overlay_inputs.highlighted_paintable) {
        inputs.has_inspector_highlight = true;
        inputs.inspector_highlight_paintable = overlay_inputs.highlighted_paintable->rust_slot();
    }
    inputs.tooltip_color = overlay_inputs.tooltip_color.value();
    inputs.tooltip_text_color = overlay_inputs.tooltip_text_color.value();
    inputs.tooltip_border_color = overlay_inputs.tooltip_border_color.value();
    Vector<Layout::RustFFI::FfiGridOverlayInput> ffi_grid_overlays;
    auto grid_label_css_pixel_size = overlay_inputs.grid_highlights.is_empty()
        ? 0.0f
        : Platform::FontPlugin::the().default_font(10)->pixel_size();
    for (auto const& highlight : overlay_inputs.grid_highlights) {
        ffi_grid_overlays.append({
            .paintable = highlight.paintable->rust_slot(),
            .color = highlight.options.color.value(),
            .label_foreground_color = highlight.options.color.with_alpha(235).suggested_foreground_color().value(),
            .label_css_pixel_size = grid_label_css_pixel_size,
            .show_area_names = highlight.options.show_area_names,
            .show_line_numbers = highlight.options.show_line_numbers,
            .show_track_sizes = highlight.options.show_track_sizes,
            .show_infinite_lines = highlight.options.show_infinite_lines,
        });
    }
    inputs.grid_overlays = ffi_grid_overlays.data();
    inputs.grid_overlay_count = ffi_grid_overlays.size();
    Vector<Layout::RustFFI::FfiFlexOverlayInput> ffi_flex_overlays;
    for (auto const& highlight : overlay_inputs.flex_highlights) {
        ffi_flex_overlays.append({
            .paintable = highlight.paintable->rust_slot(),
            .color = highlight.options.color.value(),
        });
    }
    inputs.flex_overlays = ffi_flex_overlays.data();
    inputs.flex_overlay_count = ffi_flex_overlays.size();
    if (overlay_inputs.caret_debug_rect.has_value()) {
        inputs.has_caret_debug_rect = true;
        inputs.caret_debug_rect = to_ffi_css_pixel_rect(*overlay_inputs.caret_debug_rect);
    }
    inputs.device_pixels_per_css_pixel = device_pixels_per_css_pixel;
    write_rect(device_viewport_rect, inputs.device_viewport_rect);
    if (auto navigable = viewport_paintable.navigable())
        write_css_rect(navigable->viewport_rect(), inputs.css_viewport_rect);
    inputs.should_show_line_box_borders = config.should_show_line_box_borders;
    inputs.should_paint_overlay = config.paint_overlay;
    inputs.is_recording_async_scrolling_metadata = true;
    inputs.document_id = document.unique_id().value();
    inputs.has_blocking_wheel_event_region_covering_viewport = wheel_event_region_state.has_blocking_wheel_event_region_covering_viewport;
    inputs.paint_command_cache_read_write = cache_mode == PaintCommandCacheMode::ReadWrite;
    inputs.display_list_id = placeholder_display_list.id();
    {
        auto navigable = viewport_paintable.navigable();
        inputs.window_is_focused = navigable && navigable->is_focused();
        inputs.outline_auto_color = CSS::SystemColor::accent_color(CSS::PreferredColorScheme::Auto).value();
    }
    {
        auto color_scheme = document.canvas_color_scheme();
        bool opaque_canvas = false;
        if (auto container_element = document.navigable()->container(); container_element && container_element->layout_node()) {
            auto container_scheme = container_element->layout_node()->color_scheme();
            if (container_scheme == CSS::PreferredColorScheme::Auto)
                container_scheme = CSS::PreferredColorScheme::Light;
            opaque_canvas = container_scheme != color_scheme;
        }
        if (config.canvas_fill_rect.has_value()) {
            inputs.has_canvas_fill_rect = true;
            write_rect(*config.canvas_fill_rect, inputs.canvas_fill_rect);
        }
        inputs.canvas_color = CSS::SystemColor::canvas(color_scheme).value();
        inputs.opaque_canvas = opaque_canvas;
        Gfx::IntRect bitmap_rect { {}, device_viewport_rect.size().to_type<int>() };
        write_rect(bitmap_rect, inputs.bitmap_rect);
        inputs.background_color = document.background_color().value();
    }
    PaintHostContext paint_host_context { resource_storage, viewport_paintable, paint_generation_id, device_pixels_per_css_pixel };
    auto rust_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    auto generation = Layout::RustFFI::layout_arena_record_display_list(arena, viewport_paintable.rust_slot(), hit_test_host_callbacks(), paint_host_callbacks(paint_host_context), visual_context_host_callbacks(viewport_paintable), inputs);
    if (generation == 0)
        return nullptr;
    if (Layout::RustFFI::layout_arena_last_recording_has_blocking_wheel_event_listeners(arena))
        wheel_event_region_state.has_blocking_wheel_event_listeners = true;

    size_t rust_length = 0;
    auto const* rust_bytes = Layout::RustFFI::layout_arena_display_list_bytes(arena, &rust_length);
    if (rust_painting_timing_enabled())
        dbgln("PAINT_RECORD rust={} µs commands={} bytes spliced={} captures", rust_timer.elapsed_time().to_microseconds(), rust_length, Layout::RustFFI::layout_arena_last_recording_spliced_capture_count(arena));

    VERIFY(rust_length % DisplayList::command_alignment == 0);
    auto command_bytes = MUST(ByteBuffer::copy(rust_bytes, rust_length));
    auto display_list = DisplayList::create_from_command_bytes(viewport_paintable.visual_context_tree(), move(command_bytes));
    auto registration_count = Layout::RustFFI::layout_arena_display_list_mask_registration_count(arena);
    for (size_t i = 0; i < registration_count; ++i) {
        size_t context_index = 0;
        u64 display_list_id = 0;
        Layout::RustFFI::layout_arena_display_list_mask_registration(arena, i, &context_index, &display_list_id);
        display_list->set_mask_display_list_id(VisualContextIndex { context_index }, DisplayListResourceId { display_list_id });
    }
    if (auto color = placeholder_display_list.surface_clear_color(); color.has_value())
        display_list->set_surface_clear_color(*color);
    if (auto navigable = viewport_paintable.navigable()) {
        display_list->set_async_scrolling_metadata({
            .viewport_rect = device_viewport_rect.to_type<int>(),
            .wheel_event_listener_state_generation = navigable->page().wheel_event_listener_state_generation(),
            .has_blocking_wheel_event_listeners = wheel_event_region_state.has_blocking_wheel_event_listeners,
            .has_blocking_wheel_event_region_covering_viewport = wheel_event_region_state.has_blocking_wheel_event_region_covering_viewport,
        });
    }
    return display_list;
}

NonnullRefPtr<DisplayList> record_image_paint_display_list(ImagePaint const& paint, Gfx::FloatRect dest_rect, CSS::ImageRendering image_rendering, double device_pixels_per_css_pixel, AccumulatedVisualContextTree const& visual_context_tree, DisplayListResourceStorage& resource_storage)
{
    Layout::RustFFI::FfiImagePaintRecordInputs inputs {};
    inputs.dest_rect[0] = dest_rect.x();
    inputs.dest_rect[1] = dest_rect.y();
    inputs.dest_rect[2] = dest_rect.width();
    inputs.dest_rect[3] = dest_rect.height();
    Vector<u32> color_stop_colors;
    auto write_color_stops = [&](ColorStopData const& color_stops) {
        for (auto color : color_stops.colors)
            color_stop_colors.append(color.value());
        inputs.color_stop_colors = color_stop_colors.data();
        inputs.color_stop_positions = color_stops.positions.data();
        inputs.color_stop_count = color_stops.colors.size();
        inputs.color_stops_repeating = color_stops.repeating;
    };
    auto write_interpolation_method = [&](Gfx::GradientInterpolationMethod const& method) {
        inputs.interpolation_type = to_underlying(method.type);
        inputs.rectangular_color_space = to_underlying(method.rectangular_color_space);
        inputs.polar_color_space = to_underlying(method.polar_color_space);
        inputs.hue_interpolation_method = to_underlying(method.hue_interpolation_method);
    };
    inputs.device_pixels_per_css_pixel = device_pixels_per_css_pixel;
    paint.value.visit(
        [&](ImagePaint::DecodedFrame const& decoded_frame) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::DecodedFrame;
            inputs.frame_id = resource_storage.add_image_frame(decoded_frame.frame).value();
            inputs.scaling_mode = to_underlying(CSS::to_gfx_scaling_mode(image_rendering, decoded_frame.natural_size, dest_rect.to_rounded<int>().size()));
        },
        [&](ImagePaint::NestedDisplayList const& nested) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::NestedDisplayList;
            inputs.nested_display_list_id = resource_storage.add_display_list(nested.resource.display_list, nested.resource.visual_context_tree).value();
            inputs.nested_display_list_size[0] = nested.list_size.width();
            inputs.nested_display_list_size[1] = nested.list_size.height();
        },
        [&](LinearGradientData const& gradient) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::LinearGradient;
            inputs.gradient_angle = gradient.gradient_angle;
            inputs.first_stop_position = gradient.first_stop_position;
            inputs.repeat_length = gradient.repeat_length;
            write_color_stops(gradient.color_stops);
            write_interpolation_method(gradient.interpolation_method);
        },
        [&](ResolvedRadialGradient const& gradient) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::RadialGradient;
            inputs.center = { gradient.center.x().raw_value(), gradient.center.y().raw_value() };
            inputs.size = { gradient.gradient_size.width().raw_value(), gradient.gradient_size.height().raw_value() };
            write_color_stops(gradient.data.color_stops);
            write_interpolation_method(gradient.data.interpolation_method);
        },
        [&](ResolvedConicGradient const& gradient) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::ConicGradient;
            inputs.gradient_angle = gradient.data.start_angle;
            inputs.position = { gradient.position.x().raw_value(), gradient.position.y().raw_value() };
            write_color_stops(gradient.data.color_stops);
            write_interpolation_method(gradient.data.interpolation_method);
        });
    ByteBuffer command_bytes;
    Layout::RustFFI::ladybird_web_record_image_paint_display_list(&inputs, &command_bytes,
        [](void* context, u8 const* bytes, size_t length) {
            *static_cast<ByteBuffer*>(context) = MUST(ByteBuffer::copy(bytes, length));
        });
    return DisplayList::create_from_command_bytes(visual_context_tree, move(command_bytes));
}

}
