/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Filters/BrightnessFilter.h>
#include <LibGfx/Filters/ContrastFilter.h>
#include <LibGfx/Filters/GrayscaleFilter.h>
#include <LibGfx/Filters/HueRotateFilter.h>
#include <LibGfx/Filters/InvertFilter.h>
#include <LibGfx/Filters/OpacityFilter.h>
#include <LibGfx/Filters/SaturateFilter.h>
#include <LibGfx/Filters/SepiaFilter.h>
#include <LibGfx/Filters/StackBlurFilter.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/FilterPainting.h>

namespace Web::Painting {

void apply_filter_list(Gfx::Bitmap& target_bitmap, ReadonlySpan<CSS::ResolvedBackdropFilter::FilterFunction> filter_list)
{
    auto apply_color_filter = [&](Gfx::ColorFilter const& filter) {
        const_cast<Gfx::ColorFilter&>(filter).apply(target_bitmap, target_bitmap.rect(), target_bitmap, target_bitmap.rect());
    };
    for (auto& filter_function : filter_list) {
        // See: https://drafts.fxtf.org/filter-effects-1/#supported-filter-functions
        filter_function.visit(
            [&](CSS::ResolvedBackdropFilter::Blur const& blur_filter) {
                // Applies a Gaussian blur to the input image.
                // The passed parameter defines the value of the standard deviation to the Gaussian function.
                Gfx::StackBlurFilter filter { target_bitmap };
                filter.process_rgba(blur_filter.radius, Color::Transparent);
            },
            [&](CSS::ResolvedBackdropFilter::ColorOperation const& color) {
                auto amount = color.amount;
                auto amount_clamped = clamp(amount, 0.0f, 1.0f);
                switch (color.operation) {
                case CSS::Filter::Color::Operation::Grayscale: {
                    // Converts the input image to grayscale. The passed parameter defines the proportion of the conversion.
                    // A value of 100% is completely grayscale. A value of 0% leaves the input unchanged.
                    apply_color_filter(Gfx::GrayscaleFilter { amount_clamped });
                    break;
                }
                case CSS::Filter::Color::Operation::Brightness: {
                    // Applies a linear multiplier to input image, making it appear more or less bright.
                    // A value of 0% will create an image that is completely black. A value of 100% leaves the input unchanged.
                    // Values of amount over 100% are allowed, providing brighter results.
                    apply_color_filter(Gfx::BrightnessFilter { amount });
                    break;
                }
                case CSS::Filter::Color::Operation::Contrast: {
                    // Adjusts the contrast of the input. A value of 0% will create an image that is completely gray.
                    // A value of 100% leaves the input unchanged. Values of amount over 100% are allowed, providing results with more contrast.
                    apply_color_filter(Gfx::ContrastFilter { amount });
                    break;
                }
                case CSS::Filter::Color::Operation::Invert: {
                    // Inverts the samples in the input image. The passed parameter defines the proportion of the conversion.
                    // A value of 100% is completely inverted. A value of 0% leaves the input unchanged.
                    apply_color_filter(Gfx::InvertFilter { amount_clamped });
                    break;
                }
                case CSS::Filter::Color::Operation::Opacity: {
                    // Applies transparency to the samples in the input image. The passed parameter defines the proportion of the conversion.
                    // A value of 0% is completely transparent. A value of 100% leaves the input unchanged.
                    apply_color_filter(Gfx::OpacityFilter { amount_clamped });
                    break;
                }
                case CSS::Filter::Color::Operation::Sepia: {
                    // Converts the input image to sepia. The passed parameter defines the proportion of the conversion.
                    // A value of 100% is completely sepia. A value of 0% leaves the input unchanged.
                    apply_color_filter(Gfx::SepiaFilter { amount_clamped });
                    break;
                }
                case CSS::Filter::Color::Operation::Saturate: {
                    // Saturates the input image. The passed parameter defines the proportion of the conversion.
                    // A value of 0% is completely un-saturated. A value of 100% leaves the input unchanged.
                    // Other values are linear multipliers on the effect.
                    // Values of amount over 100% are allowed, providing super-saturated results
                    apply_color_filter(Gfx::SaturateFilter { amount });
                    break;
                }
                default:
                    break;
                }
            },
            [&](CSS::ResolvedBackdropFilter::HueRotate const& hue_rotate) {
                // Applies a hue rotation on the input image.
                // The passed parameter defines the number of degrees around the color circle the input samples will be adjusted.
                // A value of 0deg leaves the input unchanged. Implementations must not normalize this value in order to allow animations beyond 360deg.
                apply_color_filter(Gfx::HueRotateFilter { hue_rotate.angle_degrees });
            },
            [&](CSS::ResolvedBackdropFilter::DropShadow const&) {
                dbgln("TODO: Implement drop-shadow() filter function!");
            });
    }
}

void apply_backdrop_filter(PaintContext& context, CSSPixelRect const& backdrop_rect, BorderRadiiData const& border_radii_data, CSS::ResolvedBackdropFilter const& backdrop_filter)
{
    auto backdrop_region = context.rounded_device_rect(backdrop_rect);

    ScopedCornerRadiusClip corner_clipper { context, backdrop_region, border_radii_data };
    context.display_list_recorder().apply_backdrop_filter(backdrop_region.to_type<int>(), border_radii_data, backdrop_filter);
}

}
