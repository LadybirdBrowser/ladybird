/*
 * Copyright (c) 2025, Bohdan Sverdlov <freezar92@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Painting/AnonymousImagePaintable.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(AnonymousImagePaintable);

GC::Ref<AnonymousImagePaintable> AnonymousImagePaintable::create(Layout::AnonymousImageBox const& layout_box)
{
    return layout_box.heap().allocate<AnonymousImagePaintable>(layout_box);
}

AnonymousImagePaintable::AnonymousImagePaintable(Layout::Box const& layout_box)
    : PaintableBox(layout_box)
{
    const_cast<DOM::Document&>(layout_box.document()).register_viewport_client(*this);
}

void AnonymousImagePaintable::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

void AnonymousImagePaintable::finalize()
{
    Base::finalize();

    // NOTE: We unregister from the document in finalize() to avoid trouble
    //       in the scenario where our Document has already been swept by GC.
    document().unregister_viewport_client(*this);
}

void AnonymousImagePaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (computed_values().content().type != CSS::ContentData::Type::Image) {
        return;
    }

    VERIFY(computed_values().content().image.has_value());

    // TODO: Support for other AbstractImageStyleValue's heirs.
    if (!computed_values().content().image.value()->is_image()) {
        return;
    }

    auto& image = computed_values().content().image.value()->as_image();
    if (!image.is_paintable()) {
        return;
    }

    Base::paint(context, phase);

    // TODO: Alternative text painting.
    if (phase == PaintPhase::Foreground) {
        auto image_rect = absolute_rect();
        auto image_rect_device_pixels = context.rounded_device_rect(image_rect);
        auto image_int_rect_device_pixels = image_rect_device_pixels.to_type<int>();

        auto const* bitmap = image.current_frame_bitmap(image_rect_device_pixels);
        auto bitmap_rect = bitmap->rect();
        auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(), bitmap_rect, image_int_rect_device_pixels);

        auto scaled_bitmap_width = CSSPixels::nearest_value_for(static_cast<float>(bitmap_rect.width()));
        auto scaled_bitmap_height = CSSPixels::nearest_value_for(static_cast<float>(bitmap_rect.height()));

        Gfx::IntRect draw_rect = {
            image_int_rect_device_pixels.x(),
            image_int_rect_device_pixels.y(),
            context.rounded_device_pixels(scaled_bitmap_width).value(),
            context.rounded_device_pixels(scaled_bitmap_height).value()
        };

        context.display_list_recorder().draw_scaled_immutable_bitmap(draw_rect, image_int_rect_device_pixels, *bitmap, scaling_mode);
    }
}

void AnonymousImagePaintable::did_set_viewport_rect(CSSPixelRect const&)
{
}

}
