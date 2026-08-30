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
#include <LibGfx/FilterImpl.h>
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
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/ImagePaint.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/PaintingRustFFI.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/Scrolling.h>
#include <LibWeb/Painting/ShadowData.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>

namespace Web::Painting {

static_assert(sizeof(Layout::RustFFI::ScrollDirection) == sizeof(ScrollDirection));
static_assert(to_underlying(Layout::RustFFI::ScrollDirection::Horizontal) == to_underlying(ScrollDirection::Horizontal));
static_assert(to_underlying(Layout::RustFFI::ScrollDirection::Vertical) == to_underlying(ScrollDirection::Vertical));

static_assert(sizeof(Layout::RustFFI::FilterOperationType) == sizeof(Gfx::FilterImpl::OperationType));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Arithmetic) == to_underlying(Gfx::FilterImpl::OperationType::Arithmetic));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Compose) == to_underlying(Gfx::FilterImpl::OperationType::Compose));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Blend) == to_underlying(Gfx::FilterImpl::OperationType::Blend));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Flood) == to_underlying(Gfx::FilterImpl::OperationType::Flood));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::DisplacementMap) == to_underlying(Gfx::FilterImpl::OperationType::DisplacementMap));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::DropShadow) == to_underlying(Gfx::FilterImpl::OperationType::DropShadow));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Blur) == to_underlying(Gfx::FilterImpl::OperationType::Blur));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::ColorFilter) == to_underlying(Gfx::FilterImpl::OperationType::ColorFilter));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::ColorMatrix) == to_underlying(Gfx::FilterImpl::OperationType::ColorMatrix));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::ColorTable) == to_underlying(Gfx::FilterImpl::OperationType::ColorTable));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Saturate) == to_underlying(Gfx::FilterImpl::OperationType::Saturate));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::HueRotate) == to_underlying(Gfx::FilterImpl::OperationType::HueRotate));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Image) == to_underlying(Gfx::FilterImpl::OperationType::Image));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Merge) == to_underlying(Gfx::FilterImpl::OperationType::Merge));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Offset) == to_underlying(Gfx::FilterImpl::OperationType::Offset));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Erode) == to_underlying(Gfx::FilterImpl::OperationType::Erode));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Dilate) == to_underlying(Gfx::FilterImpl::OperationType::Dilate));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::Turbulence) == to_underlying(Gfx::FilterImpl::OperationType::Turbulence));
static_assert(to_underlying(Layout::RustFFI::FilterOperationType::ColorSpaceConversion) == to_underlying(Gfx::FilterImpl::OperationType::ColorSpaceConversion));

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

template<typename T>
struct RustOptionalLayout {
    T value;
    bool has_value;
};

}

static_assert(sizeof(CSSPixelRect) == 16);

static_assert(sizeof(RustOptionalLayout<CSSPixels>) == sizeof(Optional<CSSPixels>));
static_assert(alignof(RustOptionalLayout<CSSPixels>) == alignof(Optional<CSSPixels>));
static_assert(sizeof(RustOptionalLayout<CSSPixelRect>) == sizeof(Optional<CSSPixelRect>));
static_assert(alignof(RustOptionalLayout<CSSPixelRect>) == alignof(Optional<CSSPixelRect>));
static_assert(sizeof(RustOptionalLayout<Gfx::IntRect>) == sizeof(Optional<Gfx::IntRect>));
static_assert(alignof(RustOptionalLayout<Gfx::IntRect>) == alignof(Optional<Gfx::IntRect>));
static_assert(sizeof(RustOptionalLayout<float>) == sizeof(Optional<float>));
static_assert(alignof(RustOptionalLayout<float>) == alignof(Optional<float>));
static_assert(sizeof(RustOptionalLayout<Gfx::FloatPoint>) == sizeof(Optional<Gfx::FloatPoint>));
static_assert(alignof(RustOptionalLayout<Gfx::FloatPoint>) == alignof(Optional<Gfx::FloatPoint>));
static_assert(sizeof(RustOptionalLayout<Gfx::FloatSize>) == sizeof(Optional<Gfx::FloatSize>));
static_assert(alignof(RustOptionalLayout<Gfx::FloatSize>) == alignof(Optional<Gfx::FloatSize>));
static_assert(sizeof(RustOptionalLayout<i64>) == sizeof(Optional<i64>));
static_assert(alignof(RustOptionalLayout<i64>) == alignof(Optional<i64>));
static_assert(sizeof(RustOptionalLayout<size_t>) == sizeof(Optional<size_t>));
static_assert(alignof(RustOptionalLayout<size_t>) == alignof(Optional<size_t>));

static_assert(sizeof(Optional<CSSPixels>) == 8);
static_assert(alignof(Optional<CSSPixels>) == 4);

static_assert(sizeof(ClipMode) == sizeof(u8));
static_assert(to_underlying(ClipMode::Intersect) == 0);
static_assert(to_underlying(ClipMode::Difference) == 1);

#define VERIFY_SHARED_FFI_TYPE(type) static_assert(IsTriviallyCopyable<type>)
VERIFY_SHARED_FFI_TYPE(CSSPixels);
VERIFY_SHARED_FFI_TYPE(CSSPixelPoint);
VERIFY_SHARED_FFI_TYPE(CSSPixelSize);
VERIFY_SHARED_FFI_TYPE(CSSPixelRect);
VERIFY_SHARED_FFI_TYPE(Gfx::IntPoint);
VERIFY_SHARED_FFI_TYPE(Gfx::FloatPoint);
VERIFY_SHARED_FFI_TYPE(Gfx::IntSize);
VERIFY_SHARED_FFI_TYPE(Gfx::FloatSize);
VERIFY_SHARED_FFI_TYPE(Gfx::FloatVector3);
VERIFY_SHARED_FFI_TYPE(Gfx::IntRect);
VERIFY_SHARED_FFI_TYPE(Gfx::FloatRect);
VERIFY_SHARED_FFI_TYPE(Gfx::Color);
VERIFY_SHARED_FFI_TYPE(Gfx::AffineTransform);
VERIFY_SHARED_FFI_TYPE(Gfx::FloatMatrix4x4);
VERIFY_SHARED_FFI_TYPE(Gfx::CornerRadius);
VERIFY_SHARED_FFI_TYPE(Gfx::CornerRadii);
VERIFY_SHARED_FFI_TYPE(Gfx::GradientInterpolationMethod);
VERIFY_SHARED_FFI_TYPE(Gfx::WindingRule);
VERIFY_SHARED_FFI_TYPE(Gfx::MaskKind);
VERIFY_SHARED_FFI_TYPE(Gfx::CompositingAndBlendingOperator);
VERIFY_SHARED_FFI_TYPE(Gfx::ScalingMode);
VERIFY_SHARED_FFI_TYPE(Gfx::InterpolationColorSpace);
VERIFY_SHARED_FFI_TYPE(ClipMode);
VERIFY_SHARED_FFI_TYPE(ChromeMetrics);
static_assert(sizeof(ChromeMetrics) == 7 * sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, scroll_thumb_min_length) == 0);
static_assert(offsetof(ChromeMetrics, scroll_thumb_padding_thin) == sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, scroll_thumb_thickness_thin) == 2 * sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, scroll_thumb_thickness) == 3 * sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, scroll_gutter_thickness) == 4 * sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, resize_gripper_size) == 5 * sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, resize_gripper_padding) == 6 * sizeof(CSSPixels));
VERIFY_SHARED_FFI_TYPE(Optional<CSSPixels>);
VERIFY_SHARED_FFI_TYPE(Optional<CSSPixelRect>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::IntRect>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::FloatPoint>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::FloatSize>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::FloatRect>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::Color>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::AffineTransform>);
VERIFY_SHARED_FFI_TYPE(Optional<i64>);
VERIFY_SHARED_FFI_TYPE(Optional<size_t>);
VERIFY_SHARED_FFI_TYPE(Optional<u32>);
VERIFY_SHARED_FFI_TYPE(Optional<float>);
#undef VERIFY_SHARED_FFI_TYPE

namespace {

static bool rust_painting_timing_enabled()
{
    static bool enabled = [] {
        auto value = Core::Environment::get("LADYBIRD_RUST_PAINTING_TIMING"sv);
        return value.has_value() && !value->is_empty() && *value != "0"sv;
    }();
    return enabled;
}

static DisplayListResourceStorage* visual_context_filter_image_storage(DOM::Document const& document)
{
    auto navigable = document.navigable();
    if (!navigable)
        return nullptr;
    return &navigable->display_list_resource_storage();
}

struct LayerImage {
    CSS::AbstractImageStyleValue const* value { nullptr };
    GC::Ptr<HTML::DecodedImageData> decoded_image_data;
};

static GC::Ptr<HTML::DecodedImageData> decoded_image_data_of(Layout::NodeWithStyle::ImageObserver const* observer)
{
    if (!observer)
        return nullptr;
    return observer->decoded_image_data();
}

static LayerImage layer_image_for(Layout::NodeWithStyle const& layout_node, Layout::RustFFI::FfiLayerImageList list, u32 computed_index)
{
    switch (list) {
    case Layout::RustFFI::FfiLayerImageList::Background: {
        auto const& layers = layout_node.background_layers();
        if (computed_index >= layers.size())
            return {};
        return { layers[computed_index].background_image.ptr(), decoded_image_data_of(layout_node.background_image_observer(computed_index)) };
    }
    case Layout::RustFFI::FfiLayerImageList::Mask: {
        auto const& layers = layout_node.mask_layers();
        if (computed_index >= layers.size())
            return {};
        return { layers[computed_index].background_image.ptr(), decoded_image_data_of(layout_node.mask_image_observer(computed_index)) };
    }
    case Layout::RustFFI::FfiLayerImageList::BorderImageSource:
        return { layout_node.border_image().source.ptr(), decoded_image_data_of(layout_node.border_image_source_observer()) };
    case Layout::RustFFI::FfiLayerImageList::DocumentBackground: {
        auto* body_element = layout_node.document().body();
        if (!body_element)
            return {};
        auto const* body_layout_node = body_element->unsafe_layout_node();
        if (!body_layout_node)
            return {};
        return layer_image_for(*body_layout_node, Layout::RustFFI::FfiLayerImageList::Background, computed_index);
    }
    default:
        return {};
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

Layout::RustFFI::FfiVisualContextHostCallbacks visual_context_host_callbacks(DOM::Document& document)
{
    return {
        .context = &document,
        .tree_inputs = [](void* context) -> Layout::RustFFI::FfiVisualContextTreeInputs {
            auto& document = *static_cast<DOM::Document*>(context);
            Layout::RustFFI::FfiVisualContextTreeInputs inputs {};
            inputs.device_pixels_per_css_pixel = document.page().client().device_pixels_per_css_pixel();
            auto const& visual_viewport = *document.visual_viewport();
            auto offset = visual_viewport.offset().to_type<double>();
            inputs.visual_viewport_offset_x = offset.x();
            inputs.visual_viewport_offset_y = offset.y();
            inputs.visual_viewport_scale = visual_viewport.scale();
            inputs.may_have_default_scroll_shift_anchor = document.may_have_default_scroll_shift_anchor();
            inputs.viewport_wheel_overflow_x = static_cast<u8>(to_underlying(overflow_value_applied_to_viewport_for_wheel_scrolling(document, ScrollDirection::Horizontal)));
            inputs.viewport_wheel_overflow_y = static_cast<u8>(to_underlying(overflow_value_applied_to_viewport_for_wheel_scrolling(document, ScrollDirection::Vertical)));
            return inputs;
        },
        .scroll_offset = [](void*, void* layout_node_shell) -> CSSPixelPoint {
            return scroll_offset(*static_cast<Layout::Node const*>(layout_node_shell));
        },
        .svg_additional_element_transform = [](void*, void* layout_node_shell, Gfx::AffineTransform* out) -> bool {
            auto const& layout_node = *static_cast<Layout::Node const*>(layout_node_shell);
            auto const* graphics_element = as_if<SVG::SVGGraphicsElement>(layout_node.dom_node());
            if (!graphics_element)
                return false;
            *out = graphics_element->additional_element_transform();
            return true;
        },
        .root_background_source = [](void* context) -> Layout::RustFFI::FfiRootBackgroundSource {
            auto& document = *static_cast<DOM::Document*>(context);
            return rust_root_background_source(document);
        },
        .svg_mask_facts = [](void*, void* layout_node_shell) -> Layout::RustFFI::FfiSvgMaskFacts {
            auto const& layout_node = *static_cast<Layout::Node const*>(layout_node_shell);
            Layout::RustFFI::FfiSvgMaskFacts facts {};
            if (auto area = mask_area(layout_node); area.has_value()) {
                facts.mask_area = *area;
                facts.mask_kind = mask_type(layout_node).value_or(Gfx::MaskKind::Alpha);
            }
            facts.clip_area = clip_area(layout_node);
            return facts;
        },
        .resolve_effects_filter = [](void* context, void* layout_node_shell, void* sink) -> Layout::RustFFI::FfiResolvedEffectsFilter {
            auto& document = *static_cast<DOM::Document*>(context);
            auto const& style_source = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            Layout::RustFFI::FfiResolvedEffectsFilter result {};
            ResolvedCSSFilter resolved_filter;
            if (style_source.filter().has_filters())
                resolved_filter = resolve_css_filter(style_source.filter(), style_source);
            result.svg_filter_bounds = resolved_filter.svg_filter_bounds;
            if (!resolved_filter.has_filters())
                return result;
            auto pixel_ratio = document.page().client().device_pixels_per_css_pixel();
            auto gfx_filter = to_gfx_filter(resolved_filter, pixel_ratio);
            if (!gfx_filter.has_value())
                return result;
            auto* filter_image_storage = visual_context_filter_image_storage(document);
            bool has_unregistered_image = false;
            auto filter_data = Gfx::serialize_filter(*gfx_filter, [&](Gfx::DecodedImageFrame const& frame) -> u64 {
                if (!filter_image_storage) {
                    has_unregistered_image = true;
                    return frame.id();
                }
                return filter_image_storage->add_image_frame(frame).value();
            });
            if (has_unregistered_image)
                return result;
            Layout::RustFFI::layout_arena_paint_push_bytes(sink, filter_data.data(), filter_data.size());
            result.has_filter = true;
            return result;
        },
        .default_scroll_shift_anchor = [](void*, void* layout_node_shell) -> Layout::RustFFI::NodeSlotId {
            if (auto const* box = as_if<Layout::Box>(static_cast<Layout::Node const*>(layout_node_shell))) {
                if (auto const* anchor_box = as_if<Layout::Box>(box->default_scroll_shift_anchor()))
                    return Layout::Node::slot_id(anchor_box);
            }
            return Layout::RustFFI::NodeSlotId { Layout::RustFFI::INVALID_NODE_SLOT_INDEX };
        },
    };
}

}

static void* layout_arena_handle(DOM::Document const& document)
{
    return const_cast<DOM::Document&>(document).layout_node_arena().handle();
}

bool rust_assign_accumulated_visual_contexts(DOM::Document& document, bool forced_incompatible_rebuild)
{
    return Layout::RustFFI::layout_arena_assign_accumulated_visual_contexts(layout_arena_handle(document), viewport_row_slot(document), visual_context_host_callbacks(document), forced_incompatible_rebuild);
}

void const* retain_rust_main_visual_context_tree(DOM::Document const& document)
{
    auto const* tree = Layout::RustFFI::layout_arena_main_visual_context_tree_retain(layout_arena_handle(document));
    VERIFY(tree);
    return tree;
}

bool rust_update_accumulated_visual_context_values(DOM::Document& document, Layout::RustFFI::NodeSlotId paintable_slot)
{
    return Layout::RustFFI::layout_arena_update_visual_context_values(layout_arena_handle(document), paintable_slot, visual_context_host_callbacks(document));
}

Optional<TransformWithOrigin> rust_compute_css_transform(Layout::Node const& box, double pixel_ratio)
{
    auto& document = const_cast<DOM::Document&>(box.document());
    float matrix_values[16];
    float origin_values[2];
    if (!Layout::RustFFI::layout_arena_compute_css_transform(box.arena_handle(), committed_row_slot(box), visual_context_host_callbacks(document), pixel_ratio, matrix_values, origin_values))
        return {};
    return TransformWithOrigin {
        Gfx::FloatMatrix4x4(
            matrix_values[0], matrix_values[1], matrix_values[2], matrix_values[3],
            matrix_values[4], matrix_values[5], matrix_values[6], matrix_values[7],
            matrix_values[8], matrix_values[9], matrix_values[10], matrix_values[11],
            matrix_values[12], matrix_values[13], matrix_values[14], matrix_values[15]),
        { origin_values[0], origin_values[1] },
    };
}

Layout::RustFFI::FfiPhysicalOverflowDirections rust_physical_overflow_directions(Layout::Node const& box)
{
    return Layout::RustFFI::layout_arena_physical_overflow_directions(box.arena_handle(), committed_row_slot(box));
}

void rust_measure_scrollable_overflow(Layout::Node const& box)
{
    auto& document = const_cast<DOM::Document&>(box.document());
    if (!document.has_committed_viewport_box())
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
    Layout::RustFFI::layout_arena_measure_scrollable_overflow(box.arena_handle(), committed_row_slot(box), visual_context_host_callbacks(document), overflow_callbacks);
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
    auto append_stop = [](void* stop_context, Gfx::Color color, float position) {
        auto& color_stops = *static_cast<ColorStopData*>(stop_context);
        color_stops.colors.append(color);
        color_stops.positions.append(position);
    };
    Layout::RustFFI::layout_arena_resolve_gradient_paint_for_size(
        gradient_style_value.rust_style_value_data(),
        layout_node.color(),
        current_color_value,
        static_cast<u8>(to_underlying(layout_node.color_scheme())),
        size,
        &resolved, &color_stops, append_stop);
    color_stops.repeating = resolved.color_stops_repeating;

    auto interpolation_method = resolved.interpolation_method;
    switch (resolved.kind) {
    case Layout::RustFFI::FfiResolvedGradientPaintKind::None:
        break;
    case Layout::RustFFI::FfiResolvedGradientPaintKind::Linear:
        return LinearGradientData { resolved.gradient_angle, resolved.first_stop_position, resolved.repeat_length, move(color_stops), interpolation_method };
    case Layout::RustFFI::FfiResolvedGradientPaintKind::Radial:
        return ResolvedRadialGradient {
            RadialGradientData { move(color_stops), interpolation_method },
            resolved.size,
            resolved.center,
        };
    case Layout::RustFFI::FfiResolvedGradientPaintKind::Conic:
        return ResolvedConicGradient {
            ConicGradientData { resolved.gradient_angle, move(color_stops), interpolation_method },
            resolved.center,
        };
    }
    VERIFY_NOT_REACHED();
}

void rust_update_visual_viewport_transform(DOM::Document& document)
{
    Layout::RustFFI::layout_arena_update_visual_viewport_transform(layout_arena_handle(document), visual_context_host_callbacks(document));
}

void rust_refresh_scroll_state(DOM::Document& document)
{
    Layout::RustFFI::layout_arena_refresh_scroll_state(layout_arena_handle(document), visual_context_host_callbacks(document));
}

ScrollStateSnapshot rust_scroll_state_snapshot(DOM::Document& document)
{
    auto* arena = layout_arena_handle(document);
    auto count = Layout::RustFFI::layout_arena_scroll_state_snapshot(arena, nullptr, 0);
    Vector<Gfx::FloatPoint> values;
    values.resize(count);
    if (count > 0)
        Layout::RustFFI::layout_arena_scroll_state_snapshot(arena, values.data(), values.size());
    ScrollStateSnapshot snapshot;
    for (size_t index = 0; index < values.size(); ++index)
        snapshot.set_device_offset_for_index(SpatialNodeIndex { static_cast<u32>(index) }, values[index]);
    return snapshot;
}

bool mirror_rust_refresh_sticky_constraints(DOM::Document& document)
{
    return Layout::RustFFI::layout_arena_refresh_sticky_constraints(layout_arena_handle(document), visual_context_host_callbacks(document));
}

void mirror_rust_clear_scroll_state(DOM::Document& document)
{
    Layout::RustFFI::layout_arena_clear_scroll_state(layout_arena_handle(document));
}

void mirror_rust_set_needs_to_refresh_scroll_state(DOM::Document& document, bool value)
{
    Layout::RustFFI::layout_arena_set_needs_to_refresh_scroll_state(layout_arena_handle(document), value);
}

void mirror_rust_invalidate_paint_cache(Layout::Node const& node)
{
    Layout::RustFFI::layout_arena_paintable_invalidate_paint_cache(node.arena_handle(), committed_row_slot(node), false);
}

void rust_invalidate_propagated_text_decoration_caches(Layout::Node const& node)
{
    Layout::RustFFI::layout_arena_paintable_invalidate_paint_cache(node.arena_handle(), committed_row_slot(node), true);
}

void rust_build_stacking_context_tree(DOM::Document& document)
{
    Layout::RustFFI::layout_arena_build_stacking_context_tree(layout_arena_handle(document), viewport_row_slot(document));
}

static void dump_stacking_context_node(StringBuilder& builder, void* arena, size_t index, int indent)
{
    auto node = Layout::RustFFI::layout_arena_stacking_context_tree_node(arena, index);
    for (int i = 0; i < indent; ++i)
        builder.append(' ');
    if (auto const* layout_node = static_cast<Layout::Node const*>(node.layout_node_shell)) {
        builder.appendff("SC for {} {} (z-index: ", layout_node->debug_description(), absolute_rect(*layout_node));
        if (node.has_effective_z_index)
            builder.appendff("{}", node.effective_z_index);
        else
            builder.append("auto"sv);
        builder.append(')');
        if (has_css_transform(*layout_node))
            builder.append(", has_transform"sv);
    } else {
        builder.append("SC for (gone)"sv);
    }
    builder.append('\n');
    for (size_t child = 0; child < node.child_count; ++child)
        dump_stacking_context_node(builder, arena, Layout::RustFFI::layout_arena_stacking_context_tree_child(arena, index, child), indent + 1);
}

void dump_stacking_context_tree(StringBuilder& builder, DOM::Document const& document)
{
    auto* arena = layout_arena_handle(document);
    if (Layout::RustFFI::layout_arena_stacking_context_tree_node_count(arena) == 0)
        return;
    dump_stacking_context_node(builder, arena, 0, 0);
}

namespace {

Layout::RustFFI::FfiHitTestHostCallbacks hit_test_host_callbacks()
{
    return {
        .context = nullptr,
        .paintable_facts = [](void*, void* layout_node_shell) -> Layout::RustFFI::FfiHitTestPaintableFacts {
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            Layout::RustFFI::FfiHitTestPaintableFacts facts {};
            auto dom_node = layout_node.dom_node();
            facts.is_inert = dom_node && dom_node->is_inert();
            facts.dom_node_has_parent = dom_node && dom_node->parent();
            facts.is_editable_or_editing_host = dom_node && dom_node->is_editable_or_editing_host();
            if (auto const* graphics_element = as_if<SVG::SVGGraphicsElement>(dom_node); graphics_element && graphics_element->unsafe_layout_node()) {
                for (auto child = graphics_element->unsafe_layout_node()->first_child(); child; child = child->next_sibling()) {
                    if (child->kind() == Layout::RustFFI::NodeKind::SVGMaskBox)
                        facts.svg_mask_content_units_object_bbox = as<SVG::SVGMaskElement>(*child->dom_node()).mask_content_units() == SVG::MaskContentUnits::ObjectBoundingBox;
                    else if (child->kind() == Layout::RustFFI::NodeKind::SVGClipBox)
                        facts.svg_clip_path_units_object_bbox = as<SVG::SVGClipPathElement>(*child->dom_node()).clip_path_units() == SVG::ClipPathUnits::ObjectBoundingBox;
                }
            }
            facts.inside_blocking_wheel_event_handler = dom_node && dom_node->inside_blocking_wheel_event_handler();
            return facts;
        },
        .text_node_facts = [](void*, void* node_shell) -> Layout::RustFFI::FfiHitTestTextNodeFacts {
            auto const& text_node = *static_cast<Layout::TextNode const*>(node_shell);
            auto const* dom_text = text_node.dom_text();
            return {
                .is_inert = dom_text && dom_text->is_inert(),
            };
        },
        .line_break_caret_targets = [](void*, void* layout_node_shell, void* sink) {
            auto const& layout_node = *static_cast<Layout::Node const*>(layout_node_shell);
            VERIFY(is_paintable_with_lines(layout_node));
            auto* dom_node = layout_node.dom_node();
            if (!dom_node)
                return;
            for (auto* child = dom_node->first_child(); child; child = child->next_sibling()) {
                auto* br = as_if<HTML::HTMLBRElement>(*child);
                if (!br || !br->represents_empty_line())
                    continue;
                Layout::RustFFI::FfiLineBreakCaretTarget target {};
                target.caret_offset = br->index();
                target.rect = caret_rect_for_child_offset(layout_node, br->index());
                Layout::RustFFI::layout_arena_hit_test_push_line_break_caret_target(sink, target);
            } },
    };
}

}

namespace {

struct PaintHostContext {
    DisplayListResourceStorage& resource_storage;
    GC::Ref<DOM::Document const> document;
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

static NonnullRefPtr<DisplayList> display_list_from_rust_recording(AccumulatedVisualContextTree const& visual_context_tree, Layout::RustFFI::FfiRecordedDisplayList const& recorded)
{
    VERIFY(recorded.byte_count % DisplayList::command_alignment == 0);
    auto command_bytes = MUST(ByteBuffer::copy(recorded.bytes, recorded.byte_count));
    Vector<DisplayListCommandRun> command_runs { ReadonlySpan<DisplayListCommandRun> { recorded.command_runs, recorded.command_run_count } };
    return DisplayList::create_from_command_bytes(visual_context_tree, move(command_bytes), move(command_runs));
}

Layout::RustFFI::FfiPaintHostCallbacks paint_host_callbacks(PaintHostContext& context)
{
    return {
        .context = &context,
        .async_scroll_facts = [](void*, void* layout_node_shell) -> Layout::RustFFI::FfiAsyncScrollFacts {
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            Layout::RustFFI::FfiAsyncScrollFacts facts {};
            auto dom_node = layout_node.dom_node();
            facts.is_nested_navigable_container = dom_node && dom_node->is_navigable_container() && as<HTML::NavigableContainer const>(*dom_node).content_navigable();
            if (is_viewport_paintable(layout_node)) {
                facts.scroll_node_kind = Layout::RustFFI::FfiScrollNodeKind::Viewport;
                facts.scrollable_node_id = layout_node.document().unique_id().value();
            } else if (layout_node.generated_for_pseudo_element().has_value()) {
                facts.scroll_node_kind = Layout::RustFFI::FfiScrollNodeKind::PseudoElement;
                facts.scrollable_node_id = layout_node.pseudo_element_generator()->unique_id().value();
            } else if (dom_node && is<DOM::Element>(*dom_node)) {
                facts.scroll_node_kind = Layout::RustFFI::FfiScrollNodeKind::Element;
                facts.scrollable_node_id = dom_node->unique_id().value();
            }
            facts.pseudo_element_type = layout_node.generated_for_pseudo_element().has_value() ? static_cast<u8>(to_underlying(*layout_node.generated_for_pseudo_element())) : 0;
            if (facts.scroll_node_kind != Layout::RustFFI::FfiScrollNodeKind::None) {
                auto snap_axes = snap_axes_of_scroll_container(layout_node);
                facts.snaps_scroll_position_horizontally = snap_axes.x;
                facts.snaps_scroll_position_vertically = snap_axes.y;
            }
            return facts;
        },
        .outline_facts = [](void*, void* layout_node_shell) -> Layout::RustFFI::FfiOutlineFacts {
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            Layout::RustFFI::FfiOutlineFacts facts {};
            if (auto const* area_element = as_if<HTML::HTMLAreaElement>(layout_node.document().focused_area().ptr())) {
                auto const* map_element = area_element->first_ancestor_of_type<HTML::HTMLMapElement>();
                auto const* image_element = as_if<HTML::HTMLImageElement>(layout_node.dom_node());
                if (map_element && image_element && map_element->first_painted_image_with_focusable_shapes().ptr() == image_element) {
                    if (auto area_computed_values = area_element->computed_style(); area_computed_values && area_computed_values->outline_style() == CSS::OutlineStyle::Auto) {
                        if (auto outline_data = Painting::outline_data(layout_node, *area_computed_values); outline_data.has_value()) {
                            if (auto path = area_element->shape_path(absolute_rect(layout_node).size()); path.has_value()) {
                                facts.paints_focused_area_outline = true;
                                facts.focused_area_path = new Gfx::Path(path.release_value());
                                facts.focused_area_color = outline_data->color;
                                facts.focused_area_width = outline_data->width;
                            }
                        }
                    }
                }
            }
            return facts;
        },
        .image_intrinsic_facts = [](void*, void* layout_node_shell, Layout::RustFFI::FfiLayerImageList list, u32 computed_index) -> Layout::RustFFI::FfiImageIntrinsicFacts {
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            Layout::RustFFI::FfiImageIntrinsicFacts facts {};
            auto [image, decoded_image_data] = layer_image_for(layout_node, list, computed_index);
            if (!image)
                return facts;
            facts.is_paintable = image->is_paintable(decoded_image_data);
            if (decoded_image_data) {
                auto natural_size = image->natural_size(*decoded_image_data);
                facts.natural_width = natural_size.width;
                facts.natural_height = natural_size.height;
                if (natural_size.aspect_ratio.has_value()) {
                    facts.has_natural_aspect_ratio = true;
                    facts.natural_aspect_ratio_numerator = natural_size.aspect_ratio->numerator();
                    facts.natural_aspect_ratio_denominator = natural_size.aspect_ratio->denominator();
                }
            }
            if (auto const* image_set = as_if<CSS::ImageSetStyleValue>(*image)) {
                if (auto const* selected_image = image_set->selected_image()) {
                    facts.has_selected_image_value = true;
                    facts.selected_image_value = selected_image->rust_style_value_data();
                }
            }
            return facts;
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
            auto style = selection_style_for_node(*text_node, text_node->dom_text());
            facts.background_color = style.background_color;
            facts.text_color = style.text_color;
            if (style.text_shadow.has_value()) {
                facts.has_text_shadow = true;
                for (auto const& layer : *style.text_shadow)
                    Layout::RustFFI::layout_arena_paint_push_selection_shadow(shadow_sink, layer.color, layer.offset_x, layer.offset_y, layer.blur_radius);
            }
            if (style.text_decoration.has_value()) {
                facts.has_text_decoration = true;
                facts.text_decoration_line_count = min(style.text_decoration->line.size(), array_size(facts.text_decoration_lines));
                for (size_t i = 0; i < facts.text_decoration_line_count; ++i)
                    facts.text_decoration_lines[i] = to_underlying(style.text_decoration->line[i]);
                facts.text_decoration_style = to_underlying(style.text_decoration->style);
                facts.text_decoration_color = style.text_decoration->color;
            }
            return facts;
        },
        .register_font = [](void* context_pointer, void const* font) -> u64 {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            return context.resource_storage.add_font(*static_cast<Gfx::Font const*>(font)).value();
        },
        .cursor_facts = [](void*, void* layout_node_shell, void* owner_layout_node_shell) -> Layout::RustFFI::FfiCursorFacts {
            auto const& layout_node = *static_cast<Layout::Node const*>(layout_node_shell);
            Layout::RustFFI::FfiCursorFacts facts {};
            if (!layout_node.document().cursor_position())
                return facts;
            auto const* owner_layout_node = static_cast<Layout::Node const*>(owner_layout_node_shell);
            Optional<CaretPaint> caret;
            if (is_inline_paintable(layout_node)) {
                caret = resolve_empty_editable_caret_paint(layout_node);
            } else {
                VERIFY(is_paintable_with_lines(layout_node));
                caret = resolve_caret_paint(layout_node, owner_layout_node);
            }
            if (!caret.has_value())
                return facts;
            facts.paints = true;
            facts.rect = caret->rect;
            facts.color = caret->color;
            return facts;
        },
        .layer_image_prepare = [](void*, void* layout_node_shell, Layout::RustFFI::FfiLayerImageList list, u32 computed_index) -> Layout::RustFFI::FfiLayerImagePrepareFacts {
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            Layout::RustFFI::FfiLayerImagePrepareFacts facts {};
            auto [image, decoded_image_data] = layer_image_for(layout_node, list, computed_index);
            if (!image)
                return facts;
            facts.is_image_style_value = is<CSS::ImageStyleValue>(*image);
            if (decoded_image_data)
                facts.single_pixel_color = decoded_image_data->color_if_single_pixel_bitmap();
            return facts;
        },
        .layer_image_nested_display_list = [](void* context_pointer, void* layout_node_shell, Layout::RustFFI::FfiLayerImageList list, u32 computed_index, Gfx::IntRect dest) -> Layout::RustFFI::FfiLayerImageNestedDisplayListFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            Layout::RustFFI::FfiLayerImageNestedDisplayListFacts facts {};
            auto [image, decoded_image_data] = layer_image_for(layout_node, list, computed_index);
            if (!image || !is<CSS::ImageStyleValue>(*image) || !decoded_image_data)
                return facts;
            if (auto display_list = decoded_image_data->record_display_list(dest.size(), layout_node.color_scheme(), context.resource_storage); display_list.has_value()) {
                facts.has_nested_display_list = true;
                facts.nested_display_list_id = context.resource_storage.add_display_list(display_list->display_list, display_list->visual_context_tree).value();
            }
            return facts;
        },
        .layer_image_current_frame = [](void* context_pointer, void* layout_node_shell, Layout::RustFFI::FfiLayerImageList list, u32 computed_index, Gfx::IntRect dest) -> Layout::RustFFI::FfiLayerImageFrameFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            Layout::RustFFI::FfiLayerImageFrameFacts facts {};
            auto [image, decoded_image_data] = layer_image_for(layout_node, list, computed_index);
            if (!image || !is<CSS::ImageStyleValue>(*image) || !decoded_image_data)
                return facts;
            if (auto frame = decoded_image_data->current_frame(dest.size()); frame.has_value()) {
                facts.has_frame = true;
                facts.frame_id = context.resource_storage.add_image_frame(*frame).value();
                facts.frame_width = frame->size().width();
                facts.frame_height = frame->size().height();
            }
            return facts;
        },
        .layer_image_paint = [](void* context_pointer, void* layout_node_shell, Layout::RustFFI::FfiLayerImageList list, u32 computed_index, Gfx::FloatRect dest_rect, CSSPixelSize css_size, u8 image_rendering_raw, Gfx::FloatSize accumulated_scale) -> Layout::RustFFI::FfiImagePaintFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            Layout::RustFFI::FfiImagePaintFacts facts {};
            auto [image, decoded_image_data] = layer_image_for(layout_node, list, computed_index);
            if (!image)
                return facts;
            ImagePaintRequest request {
                .document = layout_node.document(),
                .dest_rect = decoded_image_data ? dest_rect.to_type<int>().to_type<float>() : dest_rect,
                .image_rendering = static_cast<CSS::ImageRendering>(image_rendering_raw),
                .color_scheme = layout_node.color_scheme(),
                .accumulated_scale = accumulated_scale,
                .resource_storage = context.resource_storage,
            };
            auto paint = decoded_image_data ? decoded_image_data->image_paint(request) : image->image_paint(request, image->resolve_for_size(layout_node, css_size));
            if (paint.has_value())
                write_image_paint_facts(*paint, context, facts);
            return facts;
        },
        .replaced_paint_facts = [](void* context_pointer, void* layout_node_shell) -> Layout::RustFFI::FfiReplacedPaintFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            auto const* row = committed_row(layout_node);
            VERIFY(row);
            auto kind = layout_node.kind();
            Layout::RustFFI::FfiReplacedPaintFacts facts {};
            if (kind == Layout::RustFFI::NodeKind::ImageBox) {
                auto const& image_provider = static_cast<Layout::Box const&>(layout_node).image_provider();
                facts.has_decoded_image_data = image_provider.decoded_image_data() != nullptr;
                facts.natural_width = image_provider.intrinsic_width();
                facts.natural_height = image_provider.intrinsic_height();
                if (auto aspect_ratio = image_provider.intrinsic_aspect_ratio(); aspect_ratio.has_value()) {
                    facts.has_natural_aspect_ratio = true;
                    facts.natural_aspect_ratio_numerator = aspect_ratio->numerator();
                    facts.natural_aspect_ratio_denominator = aspect_ratio->denominator();
                }
                if (selection_state(layout_node) != SelectionState::None)
                    facts.selection_background_color = selection_style(layout_node).background_color;
            } else if (kind == Layout::RustFFI::NodeKind::CanvasBox) {
                auto& canvas_element = as<HTML::HTMLCanvasElement>(*layout_node.dom_node());
                if (auto content_size = canvas_element.canvas_surface_content_size(); content_size.has_value()) {
                    facts.has_canvas_content = true;
                    facts.canvas_content_width = content_size->width();
                    facts.canvas_content_height = content_size->height();
                    facts.canvas_id = canvas_element.canvas_id().value().value();
                    facts.canvas_content_generation = canvas_element.content_generation();
                }
            } else if (kind == Layout::RustFFI::NodeKind::VideoBox) {
                auto const& video_element = as<HTML::HTMLVideoElement>(*layout_node.dom_node());
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
            } else if (is_navigable_container_viewport_paintable(layout_node)) {
                auto const& navigable_container = as<HTML::NavigableContainer>(*layout_node.dom_node());
                auto content_navigable = navigable_container.content_navigable();
                VERIFY(content_navigable);
                auto& local_navigable = as<HTML::LocalNavigable>(*content_navigable);
                if (!local_navigable.has_been_destroyed()) {
                    auto context_id = layout_node.document().page().client().compositor_context_id_for_remote_child_frame(content_navigable->id());
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
            } else if (kind == Layout::RustFFI::NodeKind::CheckBox || kind == Layout::RustFFI::NodeKind::RadioButton) {
                auto const& input = as<HTML::HTMLInputElement const>(*layout_node.dom_node());
                facts.enabled = input.enabled();
                facts.checked = input.checked();
                facts.indeterminate = input.indeterminate();
                facts.being_activated = input.is_being_activated();
                auto color_scheme = layout_node.color_scheme();
                facts.canvas_color = CSS::SystemColor::canvas(color_scheme);
                facts.canvas_text_color = CSS::SystemColor::canvas_text(color_scheme);
                facts.accent_color = layout_node.accent_color().value_or(CSS::SystemColor::accent_color(color_scheme));
            }
            return facts;
        },
        .replaced_image_paint = [](void* context_pointer, void* layout_node_shell, Gfx::FloatRect dest_rect, Gfx::FloatSize accumulated_scale) -> Layout::RustFFI::FfiImagePaintFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            auto const* row = committed_row(layout_node);
            VERIFY(row);
            Layout::RustFFI::FfiImagePaintFacts facts {};
            GC::Ptr<HTML::DecodedImageData> decoded_image_data;
            if (layout_node.kind() == Layout::RustFFI::NodeKind::ImageBox)
                decoded_image_data = static_cast<Layout::Box const&>(layout_node).image_provider().decoded_image_data();
            else if (layout_node.kind() == Layout::RustFFI::NodeKind::SVGImageBox) {
                auto const& image_provider = as<SVG::SVGImageElement>(*layout_node.dom_node());
                decoded_image_data = image_provider.decoded_image_data();
            }
            if (!decoded_image_data)
                return facts;
            ImagePaintRequest request {
                .document = layout_node.document(),
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
        .backdrop_filter_bytes = [](void* context_pointer, void* layout_node_shell, void* sink) -> bool {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            auto const& backdrop_filter = layout_node.backdrop_filter();
            if (!backdrop_filter.has_filters())
                return false;
            auto resolved = resolve_css_filter(backdrop_filter, layout_node);
            auto gfx_filter = to_gfx_filter(resolved, context.device_pixels_per_css_pixel);
            if (!gfx_filter.has_value())
                return false;
            auto filter_data = Gfx::serialize_filter(*gfx_filter, [&](Gfx::DecodedImageFrame const& frame) {
                return context.resource_storage.add_image_frame(frame).value();
            });
            Layout::RustFFI::layout_arena_paint_push_bytes(sink, filter_data.data(), filter_data.size());
            return true;
        },
        .svg_image_facts = [](void*, void* layout_node_shell) -> Layout::RustFFI::FfiSvgImageFacts {
            auto const& layout_node = *static_cast<Layout::Node const*>(layout_node_shell);
            auto const* row = committed_row(layout_node);
            VERIFY(row);
            VERIFY(layout_node.kind() == Layout::RustFFI::NodeKind::SVGImageBox);
            Layout::RustFFI::FfiSvgImageFacts facts {};
            auto const& image_provider = as<SVG::SVGImageElement>(*layout_node.dom_node());
            facts.has_decoded_image_data = image_provider.decoded_image_data() != nullptr;
            if (auto natural_size = image_provider.intrinsic_size(); natural_size.has_value())
                facts.natural_size = natural_size->to_type<float>();
            return facts;
        },
        .svg_paint_style = [](void* context_pointer, void* layout_node_shell, bool is_stroke, Layout::RustFFI::FfiSvgPaintContext const* ffi_paint_context, void* sink) -> Layout::RustFFI::FfiSvgPaintStyle {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& layout_node = *static_cast<Layout::Node const*>(layout_node_shell);
            Layout::RustFFI::FfiSvgPaintStyle style {};
            SVG::SVGPaintContext paint_context {
                .viewport = ffi_paint_context->viewport,
                .path_bounding_box = ffi_paint_context->path_bounding_box,
                .paint_transform = ffi_paint_context->paint_transform,
                .content_scale = ffi_paint_context->content_scale,
            };
            auto const& graphics_element = as<SVG::SVGGraphicsElement>(*layout_node.dom_node());
            auto paint_server = is_stroke ? graphics_element.stroke_paint_server(paint_context, context.device_pixels_per_css_pixel) : graphics_element.fill_paint_server(paint_context, context.device_pixels_per_css_pixel);
            if (!paint_server.has_value())
                return style;
            auto write_gradient = [&](GradientPaintStyle const& gradient) {
                style.gradient_transform = gradient.gradient_transform();
                style.spread_method = static_cast<Layout::RustFFI::FfiSvgGradientSpreadMethod>(to_underlying(gradient.spread_method()));
                style.color_space = gradient.color_space();
                auto colors = gradient.color_stop_colors();
                auto positions = gradient.color_stop_positions();
                for (size_t i = 0; i < colors.size(); ++i)
                    Layout::RustFFI::layout_arena_paint_push_color_stop(sink, colors[i], positions[i]);
            };
            paint_server->visit(
                [&](PaintStyle const& paint_style) {
                    paint_style.visit(
                        [&](LinearGradientPaintStyle const& linear) {
                            style.kind = Layout::RustFFI::FfiSvgPaintStyleKind::LinearGradient;
                            write_gradient(linear);
                            style.start = linear.start_point();
                            style.end = linear.end_point();
                        },
                        [&](RadialGradientPaintStyle const& radial) {
                            style.kind = Layout::RustFFI::FfiSvgPaintStyleKind::RadialGradient;
                            write_gradient(radial);
                            style.start = radial.start_center();
                            style.start_radius = radial.start_radius();
                            style.end = radial.end_center();
                            style.end_radius = radial.end_radius();
                        },
                        [&](PatternPaintStyle const&) {
                            VERIFY_NOT_REACHED();
                        });
                },
                [&](SVG::SVGGraphicsElement::PatternPaintServer const& pattern) {
                    style.kind = Layout::RustFFI::FfiSvgPaintStyleKind::Pattern;
                    style.pattern_paintable = committed_row_slot(*pattern.pattern_layout_node);
                    style.tile_content_transform = pattern.tile_content_transform;
                    style.tile_rect = pattern.tile_rect;
                    style.content_scale = pattern.content_scale;
                    style.pattern_transform = pattern.device_pattern_transform;
                });
            return style;
        },
        .nested_display_list_from_tree = [](void* context_pointer, Layout::RustFFI::FfiRecordedDisplayList recorded, void const* retained_tree, u64 const* mask_pairs, size_t mask_pair_count) -> u64 {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto visual_context_tree = AccumulatedVisualContextTree::adopt_rust_handle(retained_tree);
            auto display_list = display_list_from_rust_recording(visual_context_tree, recorded);
            for (size_t i = 0; i < mask_pair_count; ++i)
                display_list->set_mask_display_list_id(FrameNodeIndex { static_cast<u32>(mask_pairs[i * 2]) }, DisplayListResourceId { mask_pairs[i * 2 + 1] });
            return context.resource_storage.add_display_list(move(display_list), visual_context_tree).value();
        },
        .overlay_label = [](void* context_pointer, void* layout_node_shell, u16 const* text_units, size_t text_unit_count, size_t utf16_fly_string_raw, float css_font_size, void* sink) -> Layout::RustFFI::FfiOverlayLabelFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            Utf16String text;
            if (layout_node_shell) {
                auto const& layout_node = *static_cast<Layout::Node const*>(layout_node_shell);
                auto border_rect = absolute_border_box_rect(layout_node);
                Utf16StringBuilder builder;
                builder.appendff("{}", layout_node.debug_description());
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
                .blob_bounds = bounds,
            };
        },
    };
}

}

RefPtr<DisplayList> record_rust_display_list(DOM::Document& document, DisplayList const& placeholder_display_list, DisplayListResourceStorage& resource_storage, PaintCommandCacheMode cache_mode, HTML::PaintConfig const& config, InspectorOverlayInputs const& overlay_inputs)
{
    static u64 s_next_paint_generation_id = 0;
    auto paint_generation_id = s_next_paint_generation_id++;
    auto* arena = layout_arena_handle(document);
    auto device_pixels_per_css_pixel = document.page().client().device_pixels_per_css_pixel();
    auto device_viewport_rect = document.page().css_to_device_rect(document.viewport_rect());
    auto wheel_event_region_state = document.paint_state().collect_root_blocking_wheel_event_regions(document);
    Layout::RustFFI::FfiRecordingInputs inputs {};
    if (overlay_inputs.highlighted_layout_node) {
        inputs.has_inspector_highlight = true;
        inputs.inspector_highlight_paintable = committed_row_slot(*overlay_inputs.highlighted_layout_node);
    }
    inputs.tooltip_color = overlay_inputs.tooltip_color;
    inputs.tooltip_text_color = overlay_inputs.tooltip_text_color;
    inputs.tooltip_border_color = overlay_inputs.tooltip_border_color;
    Vector<Layout::RustFFI::FfiGridOverlayInput> ffi_grid_overlays;
    auto grid_label_css_pixel_size = overlay_inputs.grid_highlights.is_empty()
        ? 0.0f
        : Platform::FontPlugin::the().default_font(10)->pixel_size();
    for (auto const& highlight : overlay_inputs.grid_highlights) {
        ffi_grid_overlays.append({
            .paintable = committed_row_slot(*highlight.layout_node),
            .color = highlight.options.color,
            .label_foreground_color = highlight.options.color.with_alpha(235).suggested_foreground_color(),
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
            .paintable = committed_row_slot(*highlight.layout_node),
            .color = highlight.options.color,
        });
    }
    inputs.flex_overlays = ffi_flex_overlays.data();
    inputs.flex_overlay_count = ffi_flex_overlays.size();
    inputs.caret_debug_rect = overlay_inputs.caret_debug_rect;
    inputs.device_pixels_per_css_pixel = device_pixels_per_css_pixel;
    inputs.device_viewport_rect = device_viewport_rect.to_type<int>();
    if (auto navigable = document.navigable())
        inputs.css_viewport_rect = navigable->viewport_rect();
    inputs.should_show_line_box_borders = config.should_show_line_box_borders;
    inputs.should_paint_overlay = config.paint_overlay;
    inputs.is_recording_async_scrolling_metadata = true;
    inputs.document_id = document.unique_id().value();
    inputs.has_blocking_wheel_event_region_covering_viewport = wheel_event_region_state.has_blocking_wheel_event_region_covering_viewport;
    inputs.viewport_wheel_overflow_x = static_cast<u8>(to_underlying(overflow_value_applied_to_viewport_for_wheel_scrolling(document, ScrollDirection::Horizontal)));
    inputs.viewport_wheel_overflow_y = static_cast<u8>(to_underlying(overflow_value_applied_to_viewport_for_wheel_scrolling(document, ScrollDirection::Vertical)));
    inputs.chrome_metrics = document.page().chrome_metrics();
    inputs.root_background_source = rust_root_background_source(document);
    inputs.paint_viewport_scrollbars = should_paint_viewport_scrollbars();
    inputs.async_scrolling_enabled = document.page().async_scrolling_enabled();
    if (auto navigable = document.navigable()) {
        if (auto handler = navigable->event_handler().middle_button_scroll_handler(); handler.has_value()) {
            inputs.middle_button_scroll_active = true;
            inputs.middle_button_scroll_origin = handler->origin();
        }
    }
    inputs.paint_command_cache_read_write = cache_mode == PaintCommandCacheMode::ReadWrite;
    inputs.display_list_id = placeholder_display_list.id();
    {
        auto navigable = document.navigable();
        inputs.window_is_focused = navigable && navigable->is_focused();
        inputs.outline_auto_color = CSS::SystemColor::accent_color(CSS::PreferredColorScheme::Auto);
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
        inputs.canvas_fill_rect = config.canvas_fill_rect;
        inputs.canvas_color = CSS::SystemColor::canvas(color_scheme);
        inputs.opaque_canvas = opaque_canvas;
        Gfx::IntRect bitmap_rect { {}, device_viewport_rect.size().to_type<int>() };
        inputs.bitmap_rect = bitmap_rect;
        inputs.background_color = document.background_color();
    }
    PaintHostContext paint_host_context { resource_storage, document, paint_generation_id, device_pixels_per_css_pixel };
    auto rust_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    auto generation = Layout::RustFFI::layout_arena_record_display_list(arena, viewport_row_slot(document), hit_test_host_callbacks(), paint_host_callbacks(paint_host_context), visual_context_host_callbacks(document), inputs);
    if (generation == 0)
        return nullptr;
    if (Layout::RustFFI::layout_arena_last_recording_has_blocking_wheel_event_listeners(arena))
        wheel_event_region_state.has_blocking_wheel_event_listeners = true;

    auto recorded = Layout::RustFFI::layout_arena_recorded_display_list(arena);
    if (rust_painting_timing_enabled())
        dbgln("PAINT_RECORD rust={} µs commands={} bytes spliced={} captures", rust_timer.elapsed_time().to_microseconds(), recorded.byte_count, Layout::RustFFI::layout_arena_last_recording_spliced_capture_count(arena));

    auto display_list = display_list_from_rust_recording(document.visual_context_tree(), recorded);
    auto registration_count = Layout::RustFFI::layout_arena_display_list_mask_registration_count(arena);
    for (size_t i = 0; i < registration_count; ++i) {
        FrameNodeIndex frame;
        u64 display_list_id = 0;
        Layout::RustFFI::layout_arena_display_list_mask_registration(arena, i, &frame, &display_list_id);
        display_list->set_mask_display_list_id(frame, DisplayListResourceId { display_list_id });
    }
    if (auto color = placeholder_display_list.surface_clear_color(); color.has_value())
        display_list->set_surface_clear_color(*color);
    if (auto navigable = document.navigable()) {
        display_list->set_async_scrolling_metadata({
            .viewport_rect = device_viewport_rect.to_type<int>(),
            .wheel_event_listener_state_generation = navigable->page().wheel_event_listener_state_generation(),
            .has_blocking_wheel_event_listeners = wheel_event_region_state.has_blocking_wheel_event_listeners,
            .has_blocking_wheel_event_region_covering_viewport = wheel_event_region_state.has_blocking_wheel_event_region_covering_viewport,
        });
    }
    return display_list;
}

DisplayListResource record_image_paint_display_list(ImagePaint const& paint, Gfx::FloatRect dest_rect, CSS::ImageRendering image_rendering, double device_pixels_per_css_pixel, DisplayListResourceStorage& resource_storage)
{
    Layout::RustFFI::FfiImagePaintRecordInputs inputs {};
    inputs.dest_rect = dest_rect;
    Vector<Gfx::Color> color_stop_colors;
    auto write_color_stops = [&](ColorStopData const& color_stops) {
        color_stop_colors = color_stops.colors;
        inputs.color_stop_colors = color_stop_colors.data();
        inputs.color_stop_positions = color_stops.positions.data();
        inputs.color_stop_count = color_stops.colors.size();
        inputs.color_stops_repeating = color_stops.repeating;
    };
    inputs.device_pixels_per_css_pixel = device_pixels_per_css_pixel;
    paint.value.visit(
        [&](ImagePaint::DecodedFrame const& decoded_frame) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::DecodedFrame;
            inputs.frame_id = resource_storage.add_image_frame(decoded_frame.frame).value();
            inputs.scaling_mode = CSS::to_gfx_scaling_mode(image_rendering, decoded_frame.natural_size, dest_rect.to_rounded<int>().size());
        },
        [&](ImagePaint::NestedDisplayList const& nested) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::NestedDisplayList;
            inputs.nested_display_list_id = resource_storage.add_display_list(nested.resource.display_list, nested.resource.visual_context_tree).value();
            inputs.nested_display_list_size = nested.list_size;
        },
        [&](LinearGradientData const& gradient) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::LinearGradient;
            inputs.gradient_angle = gradient.gradient_angle;
            inputs.first_stop_position = gradient.first_stop_position;
            inputs.repeat_length = gradient.repeat_length;
            write_color_stops(gradient.color_stops);
            inputs.interpolation_method = gradient.interpolation_method;
        },
        [&](ResolvedRadialGradient const& gradient) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::RadialGradient;
            inputs.center = gradient.center;
            inputs.size = gradient.gradient_size;
            write_color_stops(gradient.data.color_stops);
            inputs.interpolation_method = gradient.data.interpolation_method;
        },
        [&](ResolvedConicGradient const& gradient) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::ConicGradient;
            inputs.gradient_angle = gradient.data.start_angle;
            inputs.position = gradient.position;
            write_color_stops(gradient.data.color_stops);
            inputs.interpolation_method = gradient.data.interpolation_method;
        });
    Optional<DisplayListResource> recorded_display_list;
    Layout::RustFFI::ladybird_web_record_image_paint_display_list(&inputs, &recorded_display_list,
        [](void* context, Layout::RustFFI::FfiRecordedDisplayList recorded, void const* retained_tree) {
            auto visual_context_tree = AccumulatedVisualContextTree::adopt_rust_handle(retained_tree);
            auto display_list = display_list_from_rust_recording(visual_context_tree, recorded);
            *static_cast<Optional<DisplayListResource>*>(context) = DisplayListResource { move(display_list), move(visual_context_tree) };
        });
    return recorded_display_list.release_value();
}

}
