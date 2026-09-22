/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/ContextRef.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/RustFFI.h>
#include <LibCompositing/Types.h>
#include <LibCompositing/TypesRustFFI.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/GradientInterpolation.h>
#include <LibGfx/LineStyle.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibGfx/WindingRule.h>

// The Rust side mirrors these LibGfx value types field for field; the checks keep the two in step.

namespace Compositing {

static_assert(sizeof(Compositing::RustFFI::IntPoint) == sizeof(Gfx::IntPoint));
static_assert(alignof(Compositing::RustFFI::IntPoint) == alignof(Gfx::IntPoint));
static_assert(sizeof(Compositing::RustFFI::FloatPoint) == sizeof(Gfx::FloatPoint));
static_assert(alignof(Compositing::RustFFI::FloatPoint) == alignof(Gfx::FloatPoint));
static_assert(sizeof(Compositing::RustFFI::IntSize) == sizeof(Gfx::IntSize));
static_assert(alignof(Compositing::RustFFI::IntSize) == alignof(Gfx::IntSize));
static_assert(sizeof(Compositing::RustFFI::FloatSize) == sizeof(Gfx::FloatSize));
static_assert(alignof(Compositing::RustFFI::FloatSize) == alignof(Gfx::FloatSize));
static_assert(sizeof(Compositing::RustFFI::IntRect) == sizeof(Gfx::IntRect));
static_assert(alignof(Compositing::RustFFI::IntRect) == alignof(Gfx::IntRect));
static_assert(sizeof(Compositing::RustFFI::FloatRect) == sizeof(Gfx::FloatRect));
static_assert(alignof(Compositing::RustFFI::FloatRect) == alignof(Gfx::FloatRect));
static_assert(sizeof(Compositing::RustFFI::Color) == sizeof(Gfx::Color));
static_assert(alignof(Compositing::RustFFI::Color) == alignof(Gfx::Color));
static_assert(sizeof(Compositing::RustFFI::AffineTransform) == sizeof(Gfx::AffineTransform));
static_assert(alignof(Compositing::RustFFI::AffineTransform) == alignof(Gfx::AffineTransform));
static_assert(sizeof(Compositing::RustFFI::FloatMatrix4x4) == sizeof(Gfx::FloatMatrix4x4));
static_assert(alignof(Compositing::RustFFI::FloatMatrix4x4) == alignof(Gfx::FloatMatrix4x4));
static_assert(sizeof(Compositing::RustFFI::CornerRadius) == sizeof(Gfx::CornerRadius));
static_assert(alignof(Compositing::RustFFI::CornerRadius) == alignof(Gfx::CornerRadius));
static_assert(sizeof(Compositing::RustFFI::CornerRadii) == sizeof(Gfx::CornerRadii));
static_assert(alignof(Compositing::RustFFI::CornerRadii) == alignof(Gfx::CornerRadii));
static_assert(sizeof(Compositing::RustFFI::GradientInterpolationMethod) == sizeof(Gfx::GradientInterpolationMethod));
static_assert(alignof(Compositing::RustFFI::GradientInterpolationMethod) == alignof(Gfx::GradientInterpolationMethod));
static_assert(offsetof(Compositing::RustFFI::CornerRadius, horizontal_radius) == offsetof(Gfx::CornerRadius, horizontal_radius));
static_assert(offsetof(Compositing::RustFFI::CornerRadius, vertical_radius) == offsetof(Gfx::CornerRadius, vertical_radius));
static_assert(offsetof(Compositing::RustFFI::CornerRadii, top_left) == offsetof(Gfx::CornerRadii, top_left));
static_assert(offsetof(Compositing::RustFFI::CornerRadii, top_right) == offsetof(Gfx::CornerRadii, top_right));
static_assert(offsetof(Compositing::RustFFI::CornerRadii, bottom_right) == offsetof(Gfx::CornerRadii, bottom_right));
static_assert(offsetof(Compositing::RustFFI::CornerRadii, bottom_left) == offsetof(Gfx::CornerRadii, bottom_left));
static_assert(offsetof(Compositing::RustFFI::GradientInterpolationMethod, interpolation_type) == offsetof(Gfx::GradientInterpolationMethod, type));
static_assert(offsetof(Compositing::RustFFI::GradientInterpolationMethod, rectangular_color_space) == offsetof(Gfx::GradientInterpolationMethod, rectangular_color_space));
static_assert(offsetof(Compositing::RustFFI::GradientInterpolationMethod, polar_color_space) == offsetof(Gfx::GradientInterpolationMethod, polar_color_space));
static_assert(offsetof(Compositing::RustFFI::GradientInterpolationMethod, hue_interpolation_method) == offsetof(Gfx::GradientInterpolationMethod, hue_interpolation_method));
static_assert(sizeof(Compositing::RustFFI::WindingRule) == sizeof(Gfx::WindingRule));
static_assert(to_underlying(Compositing::RustFFI::WindingRule::Nonzero) == to_underlying(Gfx::WindingRule::Nonzero));
static_assert(to_underlying(Compositing::RustFFI::WindingRule::EvenOdd) == to_underlying(Gfx::WindingRule::EvenOdd));
static_assert(sizeof(Compositing::RustFFI::LineStyle) == sizeof(Gfx::LineStyle));
static_assert(to_underlying(Compositing::RustFFI::LineStyle::Solid) == to_underlying(Gfx::LineStyle::Solid));
static_assert(to_underlying(Compositing::RustFFI::LineStyle::Dotted) == to_underlying(Gfx::LineStyle::Dotted));
static_assert(to_underlying(Compositing::RustFFI::LineStyle::Dashed) == to_underlying(Gfx::LineStyle::Dashed));
static_assert(sizeof(Compositing::RustFFI::ScalingMode) == sizeof(Gfx::ScalingMode));
static_assert(to_underlying(Compositing::RustFFI::ScalingMode::None) == to_underlying(Gfx::ScalingMode::None));
static_assert(to_underlying(Compositing::RustFFI::ScalingMode::Bilinear) == to_underlying(Gfx::ScalingMode::Bilinear));
static_assert(to_underlying(Compositing::RustFFI::ScalingMode::BilinearMipmap) == to_underlying(Gfx::ScalingMode::BilinearMipmap));
static_assert(to_underlying(Compositing::RustFFI::ScalingMode::NearestNeighbor) == to_underlying(Gfx::ScalingMode::NearestNeighbor));
static_assert(sizeof(Compositing::RustFFI::CompositingAndBlendingOperator) == sizeof(Gfx::CompositingAndBlendingOperator));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Normal) == to_underlying(Gfx::CompositingAndBlendingOperator::Normal));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Multiply) == to_underlying(Gfx::CompositingAndBlendingOperator::Multiply));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Screen) == to_underlying(Gfx::CompositingAndBlendingOperator::Screen));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Darken) == to_underlying(Gfx::CompositingAndBlendingOperator::Darken));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Lighten) == to_underlying(Gfx::CompositingAndBlendingOperator::Lighten));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Overlay) == to_underlying(Gfx::CompositingAndBlendingOperator::Overlay));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::ColorDodge) == to_underlying(Gfx::CompositingAndBlendingOperator::ColorDodge));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::ColorBurn) == to_underlying(Gfx::CompositingAndBlendingOperator::ColorBurn));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::HardLight) == to_underlying(Gfx::CompositingAndBlendingOperator::HardLight));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::SoftLight) == to_underlying(Gfx::CompositingAndBlendingOperator::SoftLight));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Difference) == to_underlying(Gfx::CompositingAndBlendingOperator::Difference));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Exclusion) == to_underlying(Gfx::CompositingAndBlendingOperator::Exclusion));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Hue) == to_underlying(Gfx::CompositingAndBlendingOperator::Hue));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Saturation) == to_underlying(Gfx::CompositingAndBlendingOperator::Saturation));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Color) == to_underlying(Gfx::CompositingAndBlendingOperator::Color));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Luminosity) == to_underlying(Gfx::CompositingAndBlendingOperator::Luminosity));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Clear) == to_underlying(Gfx::CompositingAndBlendingOperator::Clear));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Copy) == to_underlying(Gfx::CompositingAndBlendingOperator::Copy));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::SourceOver) == to_underlying(Gfx::CompositingAndBlendingOperator::SourceOver));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::DestinationOver) == to_underlying(Gfx::CompositingAndBlendingOperator::DestinationOver));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::SourceIn) == to_underlying(Gfx::CompositingAndBlendingOperator::SourceIn));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::DestinationIn) == to_underlying(Gfx::CompositingAndBlendingOperator::DestinationIn));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::SourceOut) == to_underlying(Gfx::CompositingAndBlendingOperator::SourceOut));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::DestinationOut) == to_underlying(Gfx::CompositingAndBlendingOperator::DestinationOut));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::SourceATop) == to_underlying(Gfx::CompositingAndBlendingOperator::SourceATop));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::DestinationATop) == to_underlying(Gfx::CompositingAndBlendingOperator::DestinationATop));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Xor) == to_underlying(Gfx::CompositingAndBlendingOperator::Xor));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::Lighter) == to_underlying(Gfx::CompositingAndBlendingOperator::Lighter));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::PlusDarker) == to_underlying(Gfx::CompositingAndBlendingOperator::PlusDarker));
static_assert(to_underlying(Compositing::RustFFI::CompositingAndBlendingOperator::PlusLighter) == to_underlying(Gfx::CompositingAndBlendingOperator::PlusLighter));
static_assert(sizeof(Compositing::RustFFI::MaskKind) == sizeof(Gfx::MaskKind));
static_assert(to_underlying(Compositing::RustFFI::MaskKind::Alpha) == to_underlying(Gfx::MaskKind::Alpha));
static_assert(to_underlying(Compositing::RustFFI::MaskKind::Luminance) == to_underlying(Gfx::MaskKind::Luminance));
static_assert(sizeof(Compositing::RustFFI::Orientation) == sizeof(Gfx::Orientation));
static_assert(to_underlying(Compositing::RustFFI::Orientation::Horizontal) == to_underlying(Gfx::Orientation::Horizontal));
static_assert(to_underlying(Compositing::RustFFI::Orientation::Vertical) == to_underlying(Gfx::Orientation::Vertical));
static_assert(sizeof(Compositing::RustFFI::ShouldAntiAlias) == sizeof(Gfx::ShouldAntiAlias));
static_assert(to_underlying(Compositing::RustFFI::ShouldAntiAlias::Yes) == to_underlying(Gfx::ShouldAntiAlias::Yes));
static_assert(to_underlying(Compositing::RustFFI::ShouldAntiAlias::No) == to_underlying(Gfx::ShouldAntiAlias::No));
static_assert(sizeof(Compositing::RustFFI::CornerClip) == sizeof(Gfx::CornerClip));
static_assert(to_underlying(Compositing::RustFFI::CornerClip::Outside) == to_underlying(Gfx::CornerClip::Outside));
static_assert(to_underlying(Compositing::RustFFI::CornerClip::Inside) == to_underlying(Gfx::CornerClip::Inside));
static_assert(sizeof(Compositing::RustFFI::InterpolationColorSpace) == sizeof(Gfx::InterpolationColorSpace));
static_assert(to_underlying(Compositing::RustFFI::InterpolationColorSpace::LinearRGB) == to_underlying(Gfx::InterpolationColorSpace::LinearRGB));
static_assert(to_underlying(Compositing::RustFFI::InterpolationColorSpace::SRGB) == to_underlying(Gfx::InterpolationColorSpace::SRGB));
static_assert(sizeof(Compositing::RustFFI::RectangularColorSpace) == sizeof(Gfx::RectangularColorSpace));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::Srgb) == to_underlying(Gfx::RectangularColorSpace::Srgb));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::SrgbLinear) == to_underlying(Gfx::RectangularColorSpace::SrgbLinear));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::DisplayP3) == to_underlying(Gfx::RectangularColorSpace::DisplayP3));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::DisplayP3Linear) == to_underlying(Gfx::RectangularColorSpace::DisplayP3Linear));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::A98Rgb) == to_underlying(Gfx::RectangularColorSpace::A98Rgb));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::ProphotoRgb) == to_underlying(Gfx::RectangularColorSpace::ProphotoRgb));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::Rec2020) == to_underlying(Gfx::RectangularColorSpace::Rec2020));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::Lab) == to_underlying(Gfx::RectangularColorSpace::Lab));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::Oklab) == to_underlying(Gfx::RectangularColorSpace::Oklab));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::Xyz) == to_underlying(Gfx::RectangularColorSpace::Xyz));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::XyzD50) == to_underlying(Gfx::RectangularColorSpace::XyzD50));
static_assert(to_underlying(Compositing::RustFFI::RectangularColorSpace::XyzD65) == to_underlying(Gfx::RectangularColorSpace::XyzD65));
static_assert(sizeof(Compositing::RustFFI::PolarColorSpace) == sizeof(Gfx::PolarColorSpace));
static_assert(to_underlying(Compositing::RustFFI::PolarColorSpace::Hsl) == to_underlying(Gfx::PolarColorSpace::Hsl));
static_assert(to_underlying(Compositing::RustFFI::PolarColorSpace::Hwb) == to_underlying(Gfx::PolarColorSpace::Hwb));
static_assert(to_underlying(Compositing::RustFFI::PolarColorSpace::Lch) == to_underlying(Gfx::PolarColorSpace::Lch));
static_assert(to_underlying(Compositing::RustFFI::PolarColorSpace::Oklch) == to_underlying(Gfx::PolarColorSpace::Oklch));
static_assert(sizeof(Compositing::RustFFI::HueInterpolationMethod) == sizeof(Gfx::HueInterpolationMethod));
static_assert(to_underlying(Compositing::RustFFI::HueInterpolationMethod::Shorter) == to_underlying(Gfx::HueInterpolationMethod::Shorter));
static_assert(to_underlying(Compositing::RustFFI::HueInterpolationMethod::Longer) == to_underlying(Gfx::HueInterpolationMethod::Longer));
static_assert(to_underlying(Compositing::RustFFI::HueInterpolationMethod::Increasing) == to_underlying(Gfx::HueInterpolationMethod::Increasing));
static_assert(to_underlying(Compositing::RustFFI::HueInterpolationMethod::Decreasing) == to_underlying(Gfx::HueInterpolationMethod::Decreasing));
static_assert(sizeof(Compositing::RustFFI::GradientInterpolationType) == sizeof(Gfx::GradientInterpolationMethod::Type));
static_assert(to_underlying(Compositing::RustFFI::GradientInterpolationType::Rectangular) == to_underlying(Gfx::GradientInterpolationMethod::Type::Rectangular));
static_assert(to_underlying(Compositing::RustFFI::GradientInterpolationType::Polar) == to_underlying(Gfx::GradientInterpolationMethod::Type::Polar));
static_assert(sizeof(Compositing::RustFFI::CapStyle) == sizeof(Gfx::Path::CapStyle));
static_assert(to_underlying(Compositing::RustFFI::CapStyle::Butt) == to_underlying(Gfx::Path::CapStyle::Butt));
static_assert(to_underlying(Compositing::RustFFI::CapStyle::Round) == to_underlying(Gfx::Path::CapStyle::Round));
static_assert(to_underlying(Compositing::RustFFI::CapStyle::Square) == to_underlying(Gfx::Path::CapStyle::Square));
static_assert(sizeof(Compositing::RustFFI::JoinStyle) == sizeof(Gfx::Path::JoinStyle));
static_assert(to_underlying(Compositing::RustFFI::JoinStyle::Miter) == to_underlying(Gfx::Path::JoinStyle::Miter));
static_assert(to_underlying(Compositing::RustFFI::JoinStyle::Round) == to_underlying(Gfx::Path::JoinStyle::Round));
static_assert(to_underlying(Compositing::RustFFI::JoinStyle::Bevel) == to_underlying(Gfx::Path::JoinStyle::Bevel));
static_assert(sizeof(Compositing::RustFFI::ColorFilterType) == sizeof(Gfx::ColorFilterType));
static_assert(to_underlying(Compositing::RustFFI::ColorFilterType::Brightness) == to_underlying(Gfx::ColorFilterType::Brightness));
static_assert(to_underlying(Compositing::RustFFI::ColorFilterType::Contrast) == to_underlying(Gfx::ColorFilterType::Contrast));
static_assert(to_underlying(Compositing::RustFFI::ColorFilterType::Grayscale) == to_underlying(Gfx::ColorFilterType::Grayscale));
static_assert(to_underlying(Compositing::RustFFI::ColorFilterType::Invert) == to_underlying(Gfx::ColorFilterType::Invert));
static_assert(to_underlying(Compositing::RustFFI::ColorFilterType::Opacity) == to_underlying(Gfx::ColorFilterType::Opacity));
static_assert(to_underlying(Compositing::RustFFI::ColorFilterType::Saturate) == to_underlying(Gfx::ColorFilterType::Saturate));
static_assert(to_underlying(Compositing::RustFFI::ColorFilterType::Sepia) == to_underlying(Gfx::ColorFilterType::Sepia));
static_assert(sizeof(Compositing::RustFFI::FontResourceId) == sizeof(FontResourceId));
static_assert(sizeof(Compositing::RustFFI::ImageFrameResourceId) == sizeof(ImageFrameResourceId));
static_assert(sizeof(Compositing::RustFFI::VideoSinkResourceId) == sizeof(VideoSinkResourceId));
static_assert(sizeof(Compositing::RustFFI::DisplayListResourceId) == sizeof(DisplayListResourceId));
static_assert(sizeof(Compositing::RustFFI::CanvasId) == sizeof(CanvasId));
static_assert(sizeof(Compositing::RustFFI::CompositorContextId) == sizeof(Compositing::CompositorContextId));
static_assert(sizeof(Compositing::RustFFI::UniqueNodeId) == sizeof(UniqueNodeID));
static_assert(sizeof(Compositing::RustFFI::OptionalFloatRect) == sizeof(Optional<Gfx::FloatRect>));
static_assert(alignof(Compositing::RustFFI::OptionalFloatRect) == alignof(Optional<Gfx::FloatRect>));
static_assert(sizeof(Compositing::RustFFI::OptionalColor) == sizeof(Optional<Gfx::Color>));
static_assert(alignof(Compositing::RustFFI::OptionalColor) == alignof(Optional<Gfx::Color>));
static_assert(sizeof(Compositing::RustFFI::OptionalU32) == sizeof(Optional<u32>));
static_assert(alignof(Compositing::RustFFI::OptionalU32) == alignof(Optional<u32>));
static_assert(sizeof(Compositing::RustFFI::OptionalF32) == sizeof(Optional<float>));
static_assert(alignof(Compositing::RustFFI::OptionalF32) == alignof(Optional<float>));
static_assert(sizeof(Compositing::RustFFI::OptionalAffineTransform) == sizeof(Optional<Gfx::AffineTransform>));
static_assert(alignof(Compositing::RustFFI::OptionalAffineTransform) == alignof(Optional<Gfx::AffineTransform>));

}
