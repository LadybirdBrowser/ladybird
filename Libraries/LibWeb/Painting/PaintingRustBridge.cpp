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
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageRequest.h>
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
#include <LibWeb/Painting/PaintFacts.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/PaintingRustFFI.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/Scrolling.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGFilterElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>

namespace Web::Painting {

static_assert(sizeof(Layout::RustFFI::ScrollDirection) == sizeof(ScrollDirection));
static_assert(to_underlying(Layout::RustFFI::ScrollDirection::Horizontal) == to_underlying(ScrollDirection::Horizontal));
static_assert(to_underlying(Layout::RustFFI::ScrollDirection::Vertical) == to_underlying(ScrollDirection::Vertical));

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

// Resolves one url() reference of a filter list and hands the referenced <filter>'s primitives to
// the Rust graph builder the callback was handed as its sink. Frames an feImage draws are
// registered with the display list resource storage under the id the primitives name them by.
static Layout::RustFFI::FfiResolvedSvgFilter push_svg_filter_reference(void const* url_value, Layout::NodeWithStyle const& layout_node, DisplayListResourceStorage* image_storage, void* sink)
{
    auto resolved_reference = resolve_svg_filter_reference({ .pointer = url_value }, layout_node);
    Layout::RustFFI::FfiResolvedSvgFilter result {};
    result.failed = resolved_reference.failed;
    if (resolved_reference.failed)
        return result;
    result.svg_filter_bounds = resolved_reference.bounds;
    Vector<Gfx::DecodedImageFrame> image_frames;
    resolved_reference.filter_element->push_primitives(sink, layout_node, image_frames);
    // A document without a navigable has nowhere to register the frames an feImage draws, so its
    // filter list is dropped rather than handed over unresolvable; such documents are not painted.
    if (!image_storage && !image_frames.is_empty()) {
        result.failed = true;
        return result;
    }
    for (auto const& image_frame : image_frames)
        image_storage->add_image_frame(image_frame);
    return result;
}

static Layout::NodeWithStyle::ImageObserver const* layer_image_observer(Layout::NodeWithStyle const& layout_node, Layout::RustFFI::FfiLayerImageList list, u32 computed_index)
{
    switch (list) {
    case Layout::RustFFI::FfiLayerImageList::Background:
        return layout_node.background_image_observer(computed_index);
    case Layout::RustFFI::FfiLayerImageList::Mask:
        return layout_node.mask_image_observer(computed_index);
    case Layout::RustFFI::FfiLayerImageList::BorderImageSource:
        return layout_node.border_image_source_observer();
    }
    VERIFY_NOT_REACHED();
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
            inputs.viewport_wheel_overflow_x = static_cast<u8>(to_underlying(overflow_value_applied_to_viewport_for_wheel_scrolling(document, ScrollDirection::Horizontal)));
            inputs.viewport_wheel_overflow_y = static_cast<u8>(to_underlying(overflow_value_applied_to_viewport_for_wheel_scrolling(document, ScrollDirection::Vertical)));
            return inputs;
        },
        .scroll_offset = [](void*, void* layout_node_shell) -> CSSPixelPoint {
            return scroll_offset(*static_cast<Layout::Node const*>(layout_node_shell));
        },
        .scroll_node_identity = [](void*, void* layout_node_shell) -> i64 {
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            if (is_viewport_paintable(layout_node))
                return layout_node.document().unique_id().value();
            if (layout_node.generated_for_pseudo_element().has_value())
                return layout_node.pseudo_element_generator()->unique_id().value();
            if (auto dom_node = layout_node.dom_node(); dom_node && is<DOM::Element>(*dom_node))
                return dom_node->unique_id().value();
            return 0;
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
        .resolve_svg_filter = [](void* context, void* layout_node_shell, void const* url_value, void* sink) -> Layout::RustFFI::FfiResolvedSvgFilter {
            auto& document = *static_cast<DOM::Document*>(context);
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            return push_svg_filter_reference(url_value, layout_node, visual_context_filter_image_storage(document), sink);
        },
    };
}

}

Optional<Gfx::Filter> filter_from_functions(ReadonlySpan<Layout::RustFFI::FfiFilterFunction> functions)
{
    ByteBuffer serialized_filter;
    bool has_filter = Layout::RustFFI::layout_arena_filter_functions_serialize(
        functions.data(),
        functions.size(),
        [](void* context, u8 const* bytes, size_t length) {
            static_cast<ByteBuffer*>(context)->append(bytes, length);
        },
        &serialized_filter);
    if (!has_filter)
        return {};
    return Gfx::Filter { move(serialized_filter) };
}

static void* layout_arena_handle(DOM::Document const& document)
{
    return const_cast<DOM::Document&>(document).layout_node_arena().handle();
}

Layout::RustFFI::FfiVisualContextUpdateOutcome rust_update_accumulated_visual_contexts(DOM::Document& document)
{
    auto update_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    auto outcome = Layout::RustFFI::layout_arena_update_accumulated_visual_contexts(layout_arena_handle(document), viewport_row_slot(document), visual_context_host_callbacks(document));
    if (rust_painting_timing_enabled())
        dbgln("AVC_UPDATE rust={} µs {}", update_timer.elapsed_time().to_microseconds(), outcome.performed_full_build ? "full"sv : "incremental"sv);
    return outcome;
}

Vector<u32> rust_owned_visual_context_node_indices(Layout::Node const& layout_node, Layout::RustFFI::FfiVisualContextBoxNodeList list)
{
    Vector<u32> indices;
    if (!has_committed_box(layout_node))
        return indices;
    auto* arena = layout_node.arena_handle();
    auto slot = committed_row_slot(layout_node);
    indices.resize(Layout::RustFFI::layout_arena_paintable_visual_context_node_count(arena, slot, list));
    if (!indices.is_empty())
        Layout::RustFFI::layout_arena_paintable_visual_context_copy_node_indices(arena, slot, list, indices.data(), indices.size());
    return indices;
}

Vector<u32> rust_visual_animation_target_node_indices(Layout::Node const& layout_node, AccumulatedVisualContextTree const& visual_context_tree, Layout::RustFFI::FfiVisualAnimationTargetKind target_kind)
{
    Vector<u32> indices;
    if (!has_committed_box(layout_node))
        return indices;
    Layout::RustFFI::layout_arena_paintable_visual_animation_target_indices(
        layout_node.arena_handle(), committed_row_slot(layout_node), visual_context_tree.rust_handle(), target_kind,
        &indices, [](void* context, u32 index) { static_cast<Vector<u32>*>(context)->append(index); });
    return indices;
}

bool rust_background_color_can_be_compositor_animated(Layout::Node const& layout_node)
{
    if (!has_committed_box(layout_node))
        return false;
    return Layout::RustFFI::layout_arena_background_color_can_be_compositor_animated(
        layout_node.arena_handle(), committed_row_slot(layout_node), rust_root_background_source(layout_node.document()));
}

void const* retain_rust_main_visual_context_tree(DOM::Document const& document)
{
    auto const* tree = Layout::RustFFI::layout_arena_main_visual_context_tree_retain(layout_arena_handle(document));
    VERIFY(tree);
    return tree;
}

CSSPixelRect rust_apply_css_transform_to_rect(Layout::Node const& box, CSSPixelRect const& rect)
{
    auto& document = const_cast<DOM::Document&>(box.document());
    return Layout::RustFFI::layout_arena_apply_css_transform_to_rect(box.arena_handle(), committed_row_slot(box), visual_context_host_callbacks(document), rect);
}

Layout::RustFFI::FfiPhysicalOverflowDirections rust_physical_overflow_directions(Layout::Node const& box)
{
    return Layout::RustFFI::layout_arena_physical_overflow_directions(box.arena_handle(), committed_row_slot(box));
}

static Layout::RustFFI::FfiScrollableOverflowHostCallbacks scrollable_overflow_host_callbacks()
{
    return {
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
}

void rust_measure_scrollable_overflow(Layout::Node const& box)
{
    auto& document = const_cast<DOM::Document&>(box.document());
    if (!document.has_committed_viewport_box())
        return;
    Layout::RustFFI::layout_arena_measure_scrollable_overflow(box.arena_handle(), committed_row_slot(box), visual_context_host_callbacks(document), scrollable_overflow_host_callbacks());
}

Layout::RustFFI::FfiScrollableOverflowUpdateOutcome rust_update_scrollable_overflow(DOM::Document& document, bool handled_by_full_layout_commit)
{
    // The scroll offset can become invalid if the scrollable overflow rectangle has changed. For
    // example, if the scroll container has been scrolled to the very end and then its scrollable
    // overflow rect becomes smaller, the scroll offset would be out of bounds. Re-applying the
    // current offset clamps it against the new rect.
    auto clamp_scroll_offset_if_nonzero = [](void*, void* layout_node_shell) {
        auto& box = *static_cast<Layout::Node*>(layout_node_shell);
        if (!scroll_offset(box).is_zero())
            set_scroll_offset(box, scroll_offset(box));
    };

    return Layout::RustFFI::layout_arena_update_scrollable_overflow(
        layout_arena_handle(document), viewport_row_slot(document), handled_by_full_layout_commit,
        visual_context_host_callbacks(document), scrollable_overflow_host_callbacks(),
        nullptr, clamp_scroll_offset_if_nonzero);
}

static CSS::PreferredColorScheme image_color_scheme(Layout::NodeWithStyle const& layout_node)
{
    auto supports_color_scheme = [](ReadonlySpan<Utf16FlyString> schemes) {
        return schemes.contains_slow("light"_utf16) || schemes.contains_slow("dark"_utf16);
    };
    if (supports_color_scheme(layout_node.color_schemes()))
        return layout_node.color_scheme();
    auto& document = layout_node.document();
    if (auto schemes = document.supported_color_schemes(); schemes.has_value() && supports_color_scheme(*schemes))
        return layout_node.color_scheme();
    // INTEROP: Like Firefox, images use the preferred scheme when neither the element nor
    //          its document opts into a supported scheme. Controls still default to light.
    return document.svg_image_color_scheme().value_or(document.page().preferred_color_scheme());
}

CSS::ColorResolutionContext gradient_stop_color_resolution_context(Layout::NodeWithStyle const& layout_node)
{
    void const* current_color_style_value_data = nullptr;
    if (auto* dom_node = layout_node.dom_node()) {
        if (auto* element = as_if<DOM::Element>(*dom_node)) {
            if (auto const* values = element->style_group<CSS::ComputedValues::InheritedTextValues>())
                current_color_style_value_data = values->color_style_value.pointer;
        }
    }
    return {
        .color_scheme = layout_node.color_scheme(),
        .current_color = layout_node.color(),
        .current_color_style_value_data = current_color_style_value_data,
        .calculation_resolution_context = {},
    };
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

Utf16String serialize_painting_dump(DOM::Document const& document, AccumulatedVisualContextTree const& visual_context_tree, DisplayList const& display_list, DisplayListResourceStorage const& resource_storage)
{
    struct DumpContext {
        DisplayListResourceStorage const& resource_storage;
        Utf16String dump;
    } context { resource_storage, {} };

    Layout::RustFFI::FfiPaintingDumpCallbacks callbacks {
        .context = &context,
        .debug_description = [](void*, void* layout_node_shell, void* description_sink) {
            auto description = static_cast<Layout::Node const*>(layout_node_shell)->debug_description();
            auto bytes = description.bytes();
            Layout::RustFFI::layout_arena_paint_push_bytes(description_sink, bytes.data(), bytes.size()); },
        .command_bytes = [](void*, void const* display_list_pointer, size_t* byte_count) -> u8 const* {
            auto bytes = static_cast<DisplayList const*>(display_list_pointer)->command_bytes();
            *byte_count = bytes.size();
            return bytes.data();
        },
        .nested_display_list = [](void* context_pointer, u64 display_list_id) -> void const* {
            auto& context = *static_cast<DumpContext*>(context_pointer);
            return &context.resource_storage.display_list(DisplayListResourceId { display_list_id });
        },
        .append_text = [](void* context_pointer, u8 const* bytes, size_t byte_count) { static_cast<DumpContext*>(context_pointer)->dump = Utf16String::from_utf8_without_validation(StringView { bytes, byte_count }); },
    };
    auto command_runs = display_list.command_runs();
    Layout::RustFFI::painting_dump(layout_arena_handle(document), viewport_row_slot(document), visual_context_tree.rust_handle(), command_runs.data(), command_runs.size(), &display_list, callbacks);
    return move(context.dump);
}

static void append_bytes_to_string_builder(void* context, u8 const* bytes, size_t byte_count)
{
    static_cast<StringBuilder*>(context)->append(StringView { bytes, byte_count });
}

void dump_stacking_context_tree(StringBuilder& builder, DOM::Document const& document)
{
    Layout::RustFFI::FfiStackingContextDumpCallbacks callbacks {
        .context = &builder,
        .debug_description = [](void*, void* layout_node_shell, void* description_sink) {
            auto description = static_cast<Layout::Node const*>(layout_node_shell)->debug_description();
            auto bytes = description.bytes();
            Layout::RustFFI::layout_arena_paint_push_bytes(description_sink, bytes.data(), bytes.size()); },
        .append_text = append_bytes_to_string_builder,
    };
    Layout::RustFFI::layout_arena_dump_stacking_context_tree(
        layout_arena_handle(document), viewport_row_slot(document), callbacks);
}

static void push_bytes_to_dump_sink(void* sink, ReadonlyBytes bytes)
{
    Layout::RustFFI::layout_arena_paint_push_bytes(sink, bytes.data(), bytes.size());
}

static void dump_layout_tree(Layout::Node const& root, size_t initial_indent, bool interactive, void* output_context, void (*append_text)(void*, u8 const*, size_t))
{
    auto& document = const_cast<DOM::Document&>(root.document());
    Layout::RustFFI::FfiLayoutTreeDumpCallbacks callbacks {
        .context = output_context,
        .describe_dom_node = [](void*, void* dom_node_pointer, void* tag_name_sink, void* identifier_sink) {
            auto const& dom_node = *static_cast<DOM::Node const*>(dom_node_pointer);
            auto const* element = as_if<DOM::Element>(dom_node);
            StringBuilder tag_name_builder;
            tag_name_builder.append(element ? element->local_name() : dom_node.node_name());
            push_bytes_to_dump_sink(tag_name_sink, tag_name_builder.string_view().bytes());
            if (!element)
                return;
            StringBuilder identifier_builder;
            if (element->id().has_value() && !element->id()->is_empty()) {
                identifier_builder.append('#');
                identifier_builder.append(*element->id());
            }
            for (auto const& class_name : element->class_names()) {
                identifier_builder.append('.');
                identifier_builder.append(class_name);
            }
            push_bytes_to_dump_sink(identifier_sink, identifier_builder.string_view().bytes()); },
        .navigable_container_content_document = [](void*, void* dom_node_pointer, void* url_sink) -> Layout::RustFFI::FfiNestedLayoutRoot {
            auto const* content_document = as<HTML::NavigableContainer>(*static_cast<DOM::Node const*>(dom_node_pointer)).content_document_without_origin_check();
            if (!content_document)
                return { .has_document = false, .layout_root_shell = nullptr };
            auto serialized_url = content_document->url().serialize();
            push_bytes_to_dump_sink(url_sink, serialized_url.bytes());
            return { .has_document = true, .layout_root_shell = const_cast<Layout::Viewport*>(content_document->layout_node()) }; },
        .svg_as_image_layout_root = [](void*, void* dom_node_pointer) -> void* {
            auto const* image_element = as_if<HTML::HTMLImageElement>(*static_cast<DOM::Node const*>(dom_node_pointer));
            if (!image_element)
                return nullptr;
            auto const* svg_image_data = as_if<SVG::SVGDecodedImageData>(image_element->current_request().image_data().ptr());
            if (!svg_image_data)
                return nullptr;
            return const_cast<Layout::Viewport*>(svg_image_data->svg_document().unsafe_layout_node()); },
        .dump_nested_layout_tree = [](void*, void* layout_root_shell, size_t indent, bool interactive, void* output_sink) { dump_layout_tree(*static_cast<Layout::Node const*>(layout_root_shell), indent, interactive, output_sink, Layout::RustFFI::layout_arena_paint_push_bytes); },
        .append_text = append_text,
        .visual_context = visual_context_host_callbacks(document),
        .scrollable_overflow = scrollable_overflow_host_callbacks(),
    };
    Layout::RustFFI::layout_arena_dump_layout_tree(root.arena_handle(), Layout::Node::slot_id(&root), viewport_row_slot(document), initial_indent, interactive, callbacks);
}

void dump_layout_tree(StringBuilder& builder, Layout::Node const& root, bool interactive)
{
    dump_layout_tree(root, 0, interactive, &builder, append_bytes_to_string_builder);
}

namespace {

struct PaintHostContext {
    DisplayListResourceStorage& resource_storage;
    GC::Ref<DOM::Document const> document;
    u64 paint_generation_id { 0 };
    double device_pixels_per_css_pixel { 1 };
};

static NonnullRefPtr<DisplayList> display_list_from_rust_recording(AccumulatedVisualContextTree const& visual_context_tree, Layout::RustFFI::FfiRecordedDisplayList const& recorded)
{
    VERIFY(recorded.byte_count % DisplayList::command_alignment == 0);
    auto command_bytes = MUST(ByteBuffer::copy(recorded.bytes, recorded.byte_count));
    Vector<DisplayListCommandRun> command_runs { ReadonlySpan<DisplayListCommandRun> { recorded.command_runs, recorded.command_run_count } };
    return DisplayList::create_from_command_bytes(visual_context_tree, move(command_bytes), move(command_runs));
}

static Layout::RustFFI::FfiRecordingPublishCallbacks recording_publish_callbacks(PaintHostContext& context)
{
    return {
        .context = &context,
        .add_font = [](void* context_pointer, void const* font) {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            context.resource_storage.add_font(*static_cast<Gfx::Font const*>(font)); },
        .add_image_frame = [](void* context_pointer, void const* frame) {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            context.resource_storage.add_image_frame(*static_cast<Gfx::DecodedImageFrame const*>(frame)); },
        .resolve_vector_image_display_list = [](void* context_pointer, Layout::RustFFI::FfiVectorImageRenderRequest const* request) -> u64 {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& document = *context.document;
            auto empty_display_list = [&] {
                return context.resource_storage.add_display_list(DisplayList::create(document.paint_state().visual_context_tree(document)), document.paint_state().visual_context_tree(document)).value();
            };
            auto const* layout_node = static_cast<Layout::NodeWithStyle const*>(Layout::RustFFI::layout_arena_node_shell_if_live(layout_arena_handle(document), request->owner));
            if (!layout_node)
                return empty_display_list();
            GC::Ptr<HTML::DecodedImageData> decoded_image_data;
            if (request->is_replaced_content) {
                if (layout_node->kind() == Layout::RustFFI::NodeKind::ImageBox)
                    decoded_image_data = static_cast<Layout::Box const&>(*layout_node).image_provider().decoded_image_data();
                else if (layout_node->kind() == Layout::RustFFI::NodeKind::SVGImageBox)
                    decoded_image_data = as<SVG::SVGImageElement>(*layout_node->dom_node()).decoded_image_data();
            } else if (auto const* observer = layer_image_observer(*layout_node, request->list, request->computed_index)) {
                decoded_image_data = observer->decoded_image_data();
            }
            auto const* svg_image_data = as_if<SVG::SVGDecodedImageData>(decoded_image_data.ptr());
            if (!svg_image_data)
                return empty_display_list();
            auto display_list = svg_image_data->record_display_list_at_scale({ request->css_width, request->css_height }, request->raster_scale, image_color_scheme(*layout_node), context.resource_storage);
            if (!display_list.has_value())
                return empty_display_list();
            return context.resource_storage.add_display_list(move(*display_list)).value();
        },
    };
}

static void take_recording_trace_if_pending(DOM::Document& document)
{
    StringBuilder trace;
    bool has_pending_trace = Layout::RustFFI::layout_arena_take_recording_trace(
        layout_arena_handle(document), &trace,
        [](void*, void* layout_node_shell, void* description_sink) {
            auto description = static_cast<Layout::Node const*>(layout_node_shell)->debug_description();
            auto bytes = description.bytes();
            Layout::RustFFI::layout_arena_paint_push_bytes(description_sink, bytes.data(), bytes.size()); },
        append_bytes_to_string_builder);
    if (has_pending_trace)
        document.paint_state().append_recording_trace(MUST(trace.to_string()));
}

Layout::RustFFI::FfiPaintHostCallbacks paint_host_callbacks(PaintHostContext& context)
{
    return {
        .context = &context,
        .replaced_paint_facts = [](void* context_pointer, void* layout_node_shell) -> Layout::RustFFI::FfiReplacedPaintFacts {
            auto& context = *static_cast<PaintHostContext*>(context_pointer);
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            auto const* row = committed_row(layout_node);
            VERIFY(row);
            auto kind = layout_node.kind();
            Layout::RustFFI::FfiReplacedPaintFacts facts {};
            if (kind == Layout::RustFFI::NodeKind::VideoBox) {
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
            }
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
    };
}

// The platform default font at an overlay label's CSS size and at that size in device pixels, kept alive for the
// recording call.
struct OverlayLabelFonts {
    RefPtr<Gfx::Font> css_font;
    RefPtr<Gfx::Font> device_font;

    Layout::RustFFI::FfiOverlayLabelFonts ffi() const { return { .css_font = css_font.ptr(), .device_font = device_font.ptr() }; }
};

static OverlayLabelFonts overlay_label_fonts(float css_size, double device_pixels_per_css_pixel)
{
    OverlayLabelFonts fonts {
        .css_font = Platform::FontPlugin::the().default_font(css_size),
        .device_font = Platform::FontPlugin::the().default_font(css_size * static_cast<float>(device_pixels_per_css_pixel)),
    };
    VERIFY(fonts.css_font && fonts.device_font);
    return fonts;
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
    OverlayLabelFonts grid_label_fonts;
    if (!overlay_inputs.grid_highlights.is_empty()) {
        grid_label_fonts = overlay_label_fonts(10.0f, device_pixels_per_css_pixel);
        inputs.grid_label_fonts = grid_label_fonts.ffi();
    }
    for (auto const& highlight : overlay_inputs.grid_highlights) {
        ffi_grid_overlays.append({
            .paintable = committed_row_slot(*highlight.layout_node),
            .color = highlight.options.color,
            .label_foreground_color = highlight.options.color.with_alpha(235).suggested_foreground_color(),
            .label_css_pixel_size = grid_label_fonts.css_font->pixel_size(),
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
    ByteString inspector_highlight_label_text;
    OverlayLabelFonts inspector_label_fonts;
    if (overlay_inputs.highlighted_layout_node) {
        auto const& layout_node = *overlay_inputs.highlighted_layout_node;
        auto border_rect = absolute_border_box_rect(layout_node);
        inspector_highlight_label_text = ByteString::formatted("{} {}x{} @ {},{}", layout_node.debug_description(), border_rect.width(), border_rect.height(), border_rect.x(), border_rect.y());
        inspector_label_fonts = overlay_label_fonts(12.0f, device_pixels_per_css_pixel);
        inputs.inspector_highlight_label = {
            .fonts = inspector_label_fonts.ffi(),
            .text = inspector_highlight_label_text.bytes().data(),
            .text_byte_count = inspector_highlight_label_text.length(),
        };
    }
    inputs.device_viewport_rect = device_viewport_rect.to_type<int>();
    if (auto navigable = document.navigable())
        inputs.css_viewport_rect = navigable->viewport_rect();
    inputs.should_show_line_box_borders = config.should_show_line_box_borders;
    inputs.force_dark_enabled = config.force_dark_enabled;
    inputs.force_dark_foreground_threshold = config.force_dark_foreground_threshold;
    inputs.force_dark_background_threshold = config.force_dark_background_threshold;
    inputs.should_paint_overlay = config.paint_overlay;
    inputs.is_recording_async_scrolling_metadata = true;
    inputs.document_id = document.unique_id().value();
    inputs.has_blocking_wheel_event_region_covering_viewport = wheel_event_region_state.has_blocking_wheel_event_region_covering_viewport;
    inputs.wheel_event_listener_state_generation = document.page().wheel_event_listener_state_generation();
    inputs.chrome_metrics = document.page().chrome_metrics();
    inputs.paint_viewport_scrollbars = should_paint_viewport_scrollbars();
    inputs.async_scrolling_enabled = document.page().async_scrolling_enabled();
    if (auto navigable = document.navigable()) {
        if (auto handler = navigable->event_handler().middle_button_scroll_handler(); handler.has_value()) {
            inputs.middle_button_scroll_active = true;
            inputs.middle_button_scroll_origin = handler->origin();
        }
    }
    inputs.paint_command_cache_read_write = cache_mode == PaintCommandCacheMode::ReadWrite;
    {
        auto navigable = document.navigable();
        inputs.window_is_focused = navigable && navigable->is_focused();
        inputs.outline_auto_color = CSS::SystemColor::accent_color(CSS::PreferredColorScheme::Auto);
        auto palette = document.page().palette();
        inputs.palette_is_dark = palette.is_dark();
        inputs.selection_background_from_palette = CSS::SystemColor::transform_selection_background_color(inputs.window_is_focused ? palette.selection() : palette.inactive_selection());
        inputs.selection_background_light = CSS::SystemColor::transform_selection_background_color(inputs.window_is_focused ? CSS::SystemColor::highlight(CSS::PreferredColorScheme::Light) : CSS::SystemColor::inactive_highlight(CSS::PreferredColorScheme::Light));
        inputs.selection_background_dark = CSS::SystemColor::transform_selection_background_color(inputs.window_is_focused ? CSS::SystemColor::highlight(CSS::PreferredColorScheme::Dark) : CSS::SystemColor::inactive_highlight(CSS::PreferredColorScheme::Dark));
        inputs.document_has_supported_color_schemes = document.supported_color_schemes().has_value();
    }
    inputs.caret = resolve_document_caret_paint(document);
    inputs.focused_text_control = resolve_focused_text_control_selection(document);
    Vector<u8> focused_area_path_bytes;
    inputs.focused_area_outline = resolve_focused_area_outline(document, focused_area_path_bytes);
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
    reconcile_navigable_container_paint_facts(document);
    PaintHostContext paint_host_context { resource_storage, document, paint_generation_id, device_pixels_per_css_pixel };
    auto rust_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    if (!Layout::RustFFI::layout_arena_record_display_list(arena, viewport_row_slot(document), paint_host_callbacks(paint_host_context), visual_context_host_callbacks(document), inputs))
        return nullptr;
    Layout::RustFFI::layout_arena_publish_recording(arena, recording_publish_callbacks(paint_host_context));
    take_recording_trace_if_pending(document);
    if (Layout::RustFFI::layout_arena_last_recording_has_blocking_wheel_event_listeners(arena))
        wheel_event_region_state.has_blocking_wheel_event_listeners = true;
    auto stamp_async_scrolling_metadata_with_current_viewport_rect = [&](DisplayList& display_list) {
        if (auto navigable = document.navigable()) {
            display_list.set_async_scrolling_metadata({
                .viewport_rect = device_viewport_rect.to_type<int>(),
                .wheel_event_listener_state_generation = navigable->page().wheel_event_listener_state_generation(),
                .has_blocking_wheel_event_listeners = wheel_event_region_state.has_blocking_wheel_event_listeners,
                .has_blocking_wheel_event_region_covering_viewport = wheel_event_region_state.has_blocking_wheel_event_region_covering_viewport,
            });
        }
    };

    if (Layout::RustFFI::layout_arena_last_recording_is_identical_to_cache_source(arena)) {
        if (auto* source = document.paint_state().display_list_used_as_paint_command_cache_source()) {
            if (rust_painting_timing_enabled())
                dbgln("PAINT_RECORD rust={} µs identical to the previous recording", rust_timer.elapsed_time().to_microseconds());
            stamp_async_scrolling_metadata_with_current_viewport_rect(*source);
            return *source;
        }
    }

    auto recorded = Layout::RustFFI::layout_arena_recorded_display_list(arena);
    if (rust_painting_timing_enabled())
        dbgln("PAINT_RECORD rust={} µs commands={} bytes", rust_timer.elapsed_time().to_microseconds(), recorded.byte_count);

    auto display_list = display_list_from_rust_recording(document.visual_context_tree(), recorded);
    if (auto color = placeholder_display_list.surface_clear_color(); color.has_value())
        display_list->set_surface_clear_color(*color);
    stamp_async_scrolling_metadata_with_current_viewport_rect(*display_list);
    return display_list;
}

DisplayListResource record_image_paint_display_list(ImagePaint const& paint, ImagePaintRequest const& request, double device_pixels_per_css_pixel)
{
    Layout::RustFFI::FfiImagePaintRecordInputs inputs {};
    inputs.dest_rect = request.dest_rect;
    inputs.device_pixels_per_css_pixel = device_pixels_per_css_pixel;
    Optional<CSS::ComputedValuesFFI::FfiLengthResolutionContext> gradient_stop_length_resolution_context_storage;
    CSS::StyleValueFFI::FfiColorResolutionInput gradient_stop_color_resolution_input {};
    paint.value.visit(
        [&](ImagePaint::DecodedFrame const& decoded_frame) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::DecodedFrame;
            inputs.frame_id = request.resource_storage.add_image_frame(decoded_frame.frame).value();
            inputs.scaling_mode = CSS::to_gfx_scaling_mode(request.image_rendering, decoded_frame.natural_size, request.dest_rect.to_rounded<int>().size());
        },
        [&](ImagePaint::NestedDisplayList const& nested) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::NestedDisplayList;
            inputs.nested_display_list_id = request.resource_storage.add_display_list(nested.resource.display_list, nested.resource.visual_context_tree).value();
            inputs.nested_display_list_size = nested.list_size;
        },
        [&](ImagePaint::Gradient const& gradient) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::Gradient;
            inputs.gradient_style_value = gradient.style_value->rust_style_value_data();
            inputs.gradient_tile_size = request.dest_rect.size().to_type<CSSPixels>();
            gradient_stop_color_resolution_input = CSS::make_rust_color_resolution_input(request.gradient_stop_color_resolution_context, gradient_stop_length_resolution_context_storage);
            inputs.gradient_stop_color_resolution_input = &gradient_stop_color_resolution_input;
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
