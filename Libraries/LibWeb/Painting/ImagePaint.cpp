/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DevicePixelConverter.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/ImagePaint.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

ImagePaintRequest image_paint_request_for_recording(DisplayListRecordingContext& context, DOM::Document const& document, Gfx::FloatRect dest_rect, CSS::ImageRendering image_rendering, CSS::PreferredColorScheme color_scheme)
{
    auto& recorder = context.display_list_recorder();
    auto accumulated_scale = recorder.visual_context_tree().accumulated_2d_scale(
        recorder.accumulated_visual_context(), ScrollStateSnapshot {}, AccumulatedVisualContextTree::IncludeVisualViewportTransform::No);
    return ImagePaintRequest {
        .document = document,
        .dest_rect = dest_rect,
        .image_rendering = image_rendering,
        .color_scheme = color_scheme,
        .accumulated_scale = accumulated_scale,
        .resource_storage = recorder.resource_storage(),
    };
}

static void record_image_paint_with_recorder(DisplayListRecorder& recorder, DevicePixelConverter const& device_pixel_converter, ImagePaint const& image_paint, Gfx::FloatRect dest_rect, CSS::ImageRendering image_rendering)
{
    image_paint.value.visit(
        [&](ImagePaint::DecodedFrame const& decoded_frame) {
            auto scaling_mode = CSS::to_gfx_scaling_mode(image_rendering, decoded_frame.natural_size, dest_rect.to_rounded<int>().size());
            recorder.draw_scaled_decoded_image_frame(dest_rect, decoded_frame.frame, scaling_mode);
        },
        [&](ImagePaint::NestedDisplayList const& nested_display_list) {
            recorder.paint_nested_display_list(nested_display_list.resource, dest_rect, nested_display_list.list_size);
        },
        [&](LinearGradientData const& linear_gradient) {
            recorder.fill_rect_with_linear_gradient(dest_rect.to_type<int>(), linear_gradient);
        },
        [&](ResolvedRadialGradient const& radial_gradient) {
            auto center = device_pixel_converter.rounded_device_point(radial_gradient.center).to_type<int>();
            auto size = device_pixel_converter.rounded_device_size(radial_gradient.gradient_size).to_type<int>();
            recorder.fill_rect_with_radial_gradient(dest_rect.to_type<int>(), radial_gradient.data, center, size);
        },
        [&](ResolvedConicGradient const& conic_gradient) {
            auto position = device_pixel_converter.rounded_device_point(conic_gradient.position).to_type<int>();
            recorder.fill_rect_with_conic_gradient(dest_rect.to_type<int>(), conic_gradient.data, position);
        });
}

void record_image_paint(DisplayListRecordingContext& context, ImagePaint const& image_paint, Gfx::FloatRect dest_rect, CSS::ImageRendering image_rendering)
{
    record_image_paint_with_recorder(context.display_list_recorder(), context.device_pixel_converter(), image_paint, dest_rect, image_rendering);
}

NonnullRefPtr<DisplayList> record_image_paint_display_list(ImagePaint const& image_paint, Gfx::FloatRect dest_rect, CSS::ImageRendering image_rendering, double device_pixels_per_css_pixel, AccumulatedVisualContextTree const& visual_context_tree, DisplayListResourceStorage& resource_storage)
{
    auto display_list = DisplayList::create(visual_context_tree);
    DisplayListRecorder recorder(display_list, visual_context_tree, resource_storage);
    record_image_paint_with_recorder(recorder, DevicePixelConverter { device_pixels_per_css_pixel }, image_paint, dest_rect, image_rendering);
    return display_list;
}

}
