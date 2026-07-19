/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CursorStyleValue.h"
#include <LibGfx/Bitmap.h>
#include <LibGfx/Painter.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

namespace Web::CSS {

StyleValueFFI::StyleValueData* CursorStyleValue::make_cursor_data(NonnullRefPtr<AbstractImageStyleValue const> const& image, RefPtr<StyleValue const> const& x, RefPtr<StyleValue const> const& y)
{
    // The Rust allocation takes ownership of one strong reference to the image and to each
    // non-null coordinate.
    image->ref();
    if (x)
        x->ref();
    if (y)
        y->ref();
    return StyleValueFFI::rust_style_value_create_cursor(image.ptr(), x.ptr(), y.ptr());
}

void CursorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    image().serialize(builder, mode);

    if (x()) {
        VERIFY(y());
        builder.append(' ');
        x()->serialize(builder, mode);
        builder.append(' ');
        y()->serialize(builder, mode);
    }
}

ValueComparingNonnullRefPtr<StyleValue const> CursorStyleValue::absolutized(ComputationContext const& computation_context) const
{
    RefPtr<StyleValue const> absolutized_x;
    RefPtr<StyleValue const> absolutized_y;

    if (x())
        absolutized_x = x()->absolutized(computation_context);

    if (y())
        absolutized_y = y()->absolutized(computation_context);

    return CursorStyleValue::create(image().absolutized(computation_context)->as_abstract_image(), absolutized_x, absolutized_y);
}

Optional<Gfx::ImageCursor> CursorStyleValue::make_image_cursor(Layout::NodeWithStyle const& layout_node) const
{
    auto const& image = this->image();
    auto const& document = layout_node.document();
    if (!image.is_paintable(document))
        return {};

    auto const& current_color = layout_node.computed_values().color();

    // Create a bitmap if needed.
    // The cursor size for a given image never changes. It's based either on the image itself, or our default size,
    // neither of which is affected by what layout node it's for.
    if (!m_cached_bitmap.has_value()) {
        // Determine the size of the cursor.
        // "The default object size for cursor images is a UA-defined size that should be based on the size of a
        // typical cursor on the UA’s operating system.
        // The concrete object size is determined using the default sizing algorithm. If an operating system is
        // incapable of rendering a cursor above a given size, cursors larger than that size must be shrunk to
        // within the OS-supported size bounds, while maintaining the cursor image’s natural aspect ratio, if any."
        // https://drafts.csswg.org/css-ui-3/#cursor

        // 32x32 is selected arbitrarily.
        // FIXME: Ask the OS for the default size?
        CSSPixelSize const default_cursor_size { 32, 32 };
        auto cursor_css_size = run_default_sizing_algorithm({}, {}, { image.natural_width(document), image.natural_height(document), image.natural_aspect_ratio(document) }, default_cursor_size);
        // FIXME: How do we determine what cursor sizes the OS allows?
        // We don't multiply by the pixel ratio, because we want to use the image's actual pixel size.
        DevicePixelSize cursor_device_size { cursor_css_size.to_type<double>().to_rounded<int>() };

        auto maybe_bitmap = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, cursor_device_size.to_type<int>());
        if (maybe_bitmap.is_error()) {
            dbgln("Failed to create cursor bitmap: {}", maybe_bitmap.error());
            return {};
        }
        auto bitmap = maybe_bitmap.release_value();
        m_cached_bitmap = bitmap->to_shareable_bitmap();
    }

    // Repaint the bitmap if necessary
    if (m_cached_bitmap_color != current_color) {
        m_cached_bitmap_color = current_color;

        // Clear whatever was in the bitmap before.
        auto& bitmap = *m_cached_bitmap->bitmap();
        auto painter = Gfx::Painter::create(bitmap);
        painter->clear_rect(bitmap.rect().to_type<float>(), Color::Transparent);

        // Paint the cursor into a bitmap.
        auto visual_context_tree = Painting::AccumulatedVisualContextTree::create();
        auto display_list = Painting::DisplayList::create(visual_context_tree);
        Painting::DisplayListResourceStorage resource_storage;
        Painting::DisplayListRecorder display_list_recorder(display_list, visual_context_tree, resource_storage);
        DisplayListRecordingContext paint_context { display_list_recorder, document.page().palette(), document.page().client().device_pixels_per_css_pixel(), document.page().chrome_metrics() };

        image.resolve_for_size(layout_node, CSSPixelSize { bitmap.size() });
        image.paint(paint_context, document, DevicePixelRect { bitmap.rect() }, ImageRendering::Auto);

        auto painting_surface = Gfx::PaintingSurface::wrap_bitmap(bitmap);
        Painting::DisplayListPlayerSkia display_list_player;
        display_list_player.execute(*display_list, visual_context_tree, resource_storage, {}, painting_surface);
        display_list_player.flush(*painting_surface);
    }

    // "If the values are unspecified, then the natural hotspot defined inside the image resource itself is used.
    // If both the values are unspecific and the referenced cursor has no defined hotspot, the effect is as if a
    // value of "0 0" were specified."
    // FIXME: Make use of embedded hotspots.
    Gfx::IntPoint hotspot = { 0, 0 };
    if (x() && y()) {
        VERIFY(document.window());

        hotspot = { number_from_style_value(*x(), {}), number_from_style_value(*y(), {}) };
    }

    return Gfx::ImageCursor {
        .bitmap = *m_cached_bitmap,
        .hotspot = hotspot
    };
}

}
