/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Format.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OffscreenCanvasRenderingContext2DPrototype.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>
#include <LibWeb/HTML/Path2D.h>
#include <LibWeb/HTML/TextMetrics.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(OffscreenCanvasRenderingContext2D);

JS::ThrowCompletionOr<GC::Ref<OffscreenCanvasRenderingContext2D>> OffscreenCanvasRenderingContext2D::create(JS::Realm& realm, OffscreenCanvas& offscreen_canvas, JS::Value options)
{
    auto context_attributes = TRY(CanvasRenderingContext2DSettings::from_js_value(realm.vm(), options));
    return realm.create<OffscreenCanvasRenderingContext2D>(realm, offscreen_canvas, context_attributes);
}

OffscreenCanvasRenderingContext2D::OffscreenCanvasRenderingContext2D(JS::Realm& realm, OffscreenCanvas& offscreen_canvas, CanvasRenderingContext2DSettings context_attributes)
    : PlatformObject(realm)
    , CanvasPath(static_cast<Bindings::PlatformObject&>(*this), *this)
    , m_canvas(offscreen_canvas)
    , m_size(offscreen_canvas.bitmap_size_for_canvas())
    , m_context_attributes(context_attributes)
{
}

OffscreenCanvasRenderingContext2D::~OffscreenCanvasRenderingContext2D() = default;

void OffscreenCanvasRenderingContext2D::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    set_prototype(&Bindings::ensure_web_prototype<Bindings::OffscreenCanvasRenderingContext2DPrototype>(realm, "OffscreenCanvasRenderingContext2D"_string));
}

void OffscreenCanvasRenderingContext2D::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_canvas);
}

void OffscreenCanvasRenderingContext2D::set_size(Gfx::IntSize const& size)
{
    if (m_size == size)
        return;
    m_size = size;
}

void OffscreenCanvasRenderingContext2D::allocate_painting_surface_if_needed()
{
    if (m_surface || m_size.is_empty())
        return;

    auto color_type = m_context_attributes.alpha ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
    // FIXME: throw some kind of error rather than crash if bitmap creation fails
    auto err_or_bitmap = Gfx::Bitmap::create(color_type, Gfx::AlphaType::Premultiplied, m_size);
    auto bitmap = MUST(err_or_bitmap);
    m_bitmap = Gfx::ShareableBitmap(bitmap, Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap);
    m_surface = Gfx::PaintingSurface::wrap_bitmap(*m_bitmap.bitmap());

    // https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-settings:concept-canvas-alpha
    // Thus, the bitmap of such a context starts off as opaque black instead of transparent black;
    // AD-HOC: Skia provides us with a full transparent surface by default; only clear the surface if alpha is disabled.
    if (!m_context_attributes.alpha) {
        auto* painter = this->painter();
        painter->clear_rect(m_surface->rect().to_type<float>(), clear_color());
    }
}

RefPtr<Gfx::PaintingSurface> OffscreenCanvasRenderingContext2D::surface() const
{
    return m_surface;
}

GC::Ref<OffscreenCanvas> OffscreenCanvasRenderingContext2D::canvas()
{
    return m_canvas;
}

OffscreenCanvas& OffscreenCanvasRenderingContext2D::canvas_element()
{
    return *m_canvas;
}

OffscreenCanvas const& OffscreenCanvasRenderingContext2D::canvas_element() const
{

    return *m_canvas;
}

Gfx::Path OffscreenCanvasRenderingContext2D::rect_path(float x, float y, float width, float height)
{
    auto top_left = Gfx::FloatPoint(x, y);
    auto top_right = Gfx::FloatPoint(x + width, y);
    auto bottom_left = Gfx::FloatPoint(x, y + height);
    auto bottom_right = Gfx::FloatPoint(x + width, y + height);

    Gfx::Path path;
    path.move_to(top_left);
    path.line_to(top_right);
    path.line_to(bottom_right);
    path.line_to(bottom_left);
    path.line_to(top_left);
    return path;
}

void OffscreenCanvasRenderingContext2D::fill_rect(float x, float y, float width, float height)
{
    fill_internal(rect_path(x, y, width, height), Gfx::WindingRule::EvenOdd);
}
// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-clearrect
void OffscreenCanvasRenderingContext2D::clear_rect(float x, float y, float width, float height)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height))
        return;

    if (auto* painter = this->painter()) {
        auto rect = Gfx::FloatRect(x, y, width, height);
        painter->clear_rect(rect, clear_color());
    }
}

void OffscreenCanvasRenderingContext2D::stroke_rect(float x, float y, float width, float height)
{
    stroke_internal(rect_path(x, y, width, height));
}
// 4.12.5.1.14 Drawing images, https://html.spec.whatwg.org/multipage/canvas.html#drawing-images
WebIDL::ExceptionOr<void> OffscreenCanvasRenderingContext2D::draw_image_internal(CanvasImageSource const& image, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(source_x) || !isfinite(source_y) || !isfinite(source_width) || !isfinite(source_height) || !isfinite(destination_x) || !isfinite(destination_y) || !isfinite(destination_width) || !isfinite(destination_height))
        return {};

    // 2. Let usability be the result of checking the usability of image.
    auto usability = TRY(check_usability_of_image(image));

    // 3. If usability is bad, then return (without drawing anything).
    if (usability == CanvasImageSourceUsability::Bad)
        return {};

    auto bitmap = canvas_image_source_bitmap(image);
    if (!bitmap)
        return {};

    // 4. Establish the source and destination rectangles as follows:
    //    If not specified, the dw and dh arguments must default to the values of sw and sh, interpreted such that one CSS pixel in the image is treated as one unit in the output bitmap's coordinate space.
    //    If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's intrinsic width in image pixels, and the image's intrinsic height in image pixels, respectively.
    //    If the image has no intrinsic dimensions, then the concrete object size must be used instead, as determined using the CSS "Concrete Object Size Resolution" algorithm, with the specified size having
    //    neither a definite width nor height, nor any additional constraints, the object's intrinsic properties being those of the image argument, and the default object size being the size of the output bitmap.
    //    The source rectangle is the rectangle whose corners are the four points (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh).
    //    The destination rectangle is the rectangle whose corners are the four points (dx, dy), (dx+dw, dy), (dx+dw, dy+dh), (dx, dy+dh).
    // NOTE: Implemented in drawImage() overloads
    if (source_width < 0) {
        source_x += source_width;
        source_width = abs(source_width);
    }
    if (source_height < 0) {
        source_y += source_height;
        source_height = abs(source_height);
    }
    if (destination_width < 0) {
        destination_x += destination_width;
        destination_width = abs(destination_width);
    }
    if (destination_height < 0) {
        destination_y += destination_height;
        destination_height = abs(destination_height);
    }

    //    The source rectangle is the rectangle whose corners are the four points (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh).
    auto source_rect = Gfx::FloatRect { source_x, source_y, source_width, source_height };
    //    The destination rectangle is the rectangle whose corners are the four points (dx, dy), (dx+dw, dy), (dx+dw, dy+dh), (dx, dy+dh).
    auto destination_rect = Gfx::FloatRect { destination_x, destination_y, destination_width, destination_height };
    //    When the source rectangle is outside the source image, the source rectangle must be clipped
    //    to the source image and the destination rectangle must be clipped in the same proportion.
    auto clipped_source = source_rect.intersected(bitmap->rect().to_type<float>());
    auto clipped_destination = destination_rect;
    if (clipped_source != source_rect) {
        clipped_destination.set_width(clipped_destination.width() * (clipped_source.width() / source_rect.width()));
        clipped_destination.set_height(clipped_destination.height() * (clipped_source.height() / source_rect.height()));
    }

    // 5. If one of the sw or sh arguments is zero, then return. Nothing is painted.
    if (source_width == 0 || source_height == 0)
        return {};

    // 6. Paint the region of the image argument specified by the source rectangle on the region of the rendering context's output bitmap specified by the destination rectangle, after applying the current transformation matrix to the destination rectangle.
    auto scaling_mode = Gfx::ScalingMode::NearestNeighbor;
    if (drawing_state().image_smoothing_enabled) {
        // FIXME: Honor drawing_state().image_smoothing_quality
        scaling_mode = Gfx::ScalingMode::BilinearMipmap;
    }

    if (auto* painter = this->painter()) {
        painter->draw_bitmap(destination_rect, *bitmap, source_rect.to_rounded<int>(), scaling_mode, drawing_state().filter, drawing_state().global_alpha, drawing_state().current_compositing_and_blending_operator);
    }

    // 7. If image is not origin-clean, then set the CanvasRenderingContext2D's origin-clean flag to false.
    if (image_is_not_origin_clean(image))
        m_origin_clean = false;

    return {};
}

void OffscreenCanvasRenderingContext2D::begin_path()
{
    path().clear();
}

static Gfx::Path::CapStyle to_gfx_cap(Bindings::CanvasLineCap const& cap_style)
{
    switch (cap_style) {
    case Bindings::CanvasLineCap::Butt:
        return Gfx::Path::CapStyle::Butt;
    case Bindings::CanvasLineCap::Round:
        return Gfx::Path::CapStyle::Round;
    case Bindings::CanvasLineCap::Square:
        return Gfx::Path::CapStyle::Square;
    }
    VERIFY_NOT_REACHED();
}

static Gfx::Path::JoinStyle to_gfx_join(Bindings::CanvasLineJoin const& join_style)
{
    switch (join_style) {
    case Bindings::CanvasLineJoin::Round:
        return Gfx::Path::JoinStyle::Round;
    case Bindings::CanvasLineJoin::Bevel:
        return Gfx::Path::JoinStyle::Bevel;
    case Bindings::CanvasLineJoin::Miter:
        return Gfx::Path::JoinStyle::Miter;
    }

    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-settings:concept-canvas-alpha
Gfx::Color OffscreenCanvasRenderingContext2D::clear_color() const
{
    return m_context_attributes.alpha ? Gfx::Color::Transparent : Gfx::Color::Black;
}

void OffscreenCanvasRenderingContext2D::stroke_internal(Gfx::Path const& path)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = drawing_state();
    auto paint_style = state.stroke_style.to_gfx_paint_style();
    if (!paint_style->is_visible())
        return;

    auto line_cap = to_gfx_cap(state.line_cap);
    auto line_join = to_gfx_join(state.line_join);
    // FIXME: Need a Vector<float> for rendering dash_array, but state.dash_list is Vector<double>.
    // Maybe possible to avoid creating copies?
    auto dash_array = Vector<float> {};
    dash_array.ensure_capacity(state.dash_list.size());
    for (auto const& dash : state.dash_list) {
        dash_array.append(static_cast<float>(dash));
    }
    paint_shadow_for_stroke_internal(path, line_cap, line_join, dash_array);
    painter->stroke_path(path, paint_style, state.filter, state.line_width, state.global_alpha, state.current_compositing_and_blending_operator, line_cap, line_join, state.miter_limit, dash_array, state.line_dash_offset);
}

void OffscreenCanvasRenderingContext2D::stroke()
{
    stroke_internal(path());
}

void OffscreenCanvasRenderingContext2D::stroke(Path2D const& path)
{
    stroke_internal(path.path());
}

Gfx::Path OffscreenCanvasRenderingContext2D::text_path(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (max_width.has_value() && max_width.value() <= 0)
        return {};

    auto& drawing_state = this->drawing_state();

    auto const& font_cascade_list = this->font_cascade_list();
    auto const& font = font_cascade_list->first();
    auto glyph_runs = Gfx::shape_text({ x, y }, text.utf16_view(), *font_cascade_list);
    Gfx::Path path;
    for (auto const& glyph_run : glyph_runs) {
        path.glyph_run(glyph_run);
    }

    auto text_width = path.bounding_box().width();
    Gfx::AffineTransform transform = {};

    // https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm:
    // 9. If maxWidth was provided and the hypothetical width of the inline box in the hypothetical line box
    // is greater than maxWidth CSS pixels, then change font to have a more condensed font (if one is
    // available or if a reasonably readable one can be synthesized by applying a horizontal scale
    // factor to the font) or a smaller font, and return to the previous step.
    if (max_width.has_value() && text_width > float(*max_width)) {
        auto horizontal_scale = float(*max_width) / text_width;
        transform = Gfx::AffineTransform {}.scale({ horizontal_scale, 1 });
        text_width *= horizontal_scale;
    }

    // Apply text align
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textalign
    // The direction property affects how "start" and "end" are interpreted:
    // - "ltr" or "inherit" (default): start=left, end=right
    // - "rtl": start=right, end=left

    // Determine if we're in RTL mode
    bool is_rtl = drawing_state.direction == Bindings::CanvasDirection::Rtl;

    // Center alignment is the same regardless of direction
    if (drawing_state.text_align == Bindings::CanvasTextAlign::Center) {
        transform = Gfx::AffineTransform {}.set_translation({ -text_width / 2, 0 }).multiply(transform);
    }
    // Handle "start" alignment
    else if (drawing_state.text_align == Bindings::CanvasTextAlign::Start) {
        // In RTL, "start" means right-aligned (translate by full width)
        if (is_rtl) {
            transform = Gfx::AffineTransform {}.set_translation({ -text_width, 0 }).multiply(transform);
        }
        // In LTR, "start" means left-aligned (no translation needed - default)
    }
    // Handle "end" alignment
    else if (drawing_state.text_align == Bindings::CanvasTextAlign::End) {
        // In RTL, "end" means left-aligned (no translation needed)
        if (!is_rtl) {
            // In LTR, "end" means right-aligned (translate by full width)
            transform = Gfx::AffineTransform {}.set_translation({ -text_width, 0 }).multiply(transform);
        }
    }
    // Explicit "left" and "right" alignments ignore direction
    else if (drawing_state.text_align == Bindings::CanvasTextAlign::Right) {
        transform = Gfx::AffineTransform {}.set_translation({ -text_width, 0 }).multiply(transform);
    }
    // Left is the default - no translation needed

    // Apply text baseline
    // FIXME: Implement CanvasTextBaseline::Hanging, Bindings::CanvasTextAlign::Alphabetic and Bindings::CanvasTextAlign::Ideographic for real
    //        right now they are just handled as textBaseline = top or bottom.
    //        https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textbaseline-hanging
    // Default baseline of draw_text is top so do nothing by CanvasTextBaseline::Top and CanvasTextBaseline::Hanging
    if (drawing_state.text_baseline == Bindings::CanvasTextBaseline::Middle) {
        transform = Gfx::AffineTransform {}.set_translation({ 0, font.pixel_size() / 2 }).multiply(transform);
    }
    if (drawing_state.text_baseline == Bindings::CanvasTextBaseline::Top || drawing_state.text_baseline == Bindings::CanvasTextBaseline::Hanging) {
        transform = Gfx::AffineTransform {}.set_translation({ 0, font.pixel_size() }).multiply(transform);
    }

    return path.copy_transformed(transform);
}

void OffscreenCanvasRenderingContext2D::fill_text(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (!isfinite(x) || !isfinite(y) || (max_width.has_value() && !isfinite(max_width.value())))
        return;

    fill_internal(text_path(text, x, y, max_width), Gfx::WindingRule::Nonzero);
}

void OffscreenCanvasRenderingContext2D::stroke_text(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (!isfinite(x) || !isfinite(y) || (max_width.has_value() && !isfinite(max_width.value())))
        return;

    stroke_internal(text_path(text, x, y, max_width));
}

static Gfx::WindingRule parse_fill_rule(StringView fill_rule)
{
    if (fill_rule == "evenodd"sv)
        return Gfx::WindingRule::EvenOdd;
    if (fill_rule == "nonzero"sv)
        return Gfx::WindingRule::Nonzero;
    dbgln("Unrecognized fillRule for CRC2D.fill() - this problem goes away once we pass an enum instead of a string");
    return Gfx::WindingRule::Nonzero;
}

void OffscreenCanvasRenderingContext2D::fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = this->drawing_state();
    auto paint_style = state.fill_style.to_gfx_paint_style();
    if (!paint_style->is_visible())
        return;

    paint_shadow_for_fill_internal(path, winding_rule);

    painter->fill_path(path, paint_style, state.filter, state.global_alpha, state.current_compositing_and_blending_operator, winding_rule);
}

void OffscreenCanvasRenderingContext2D::fill(StringView fill_rule)
{
    fill_internal(path(), parse_fill_rule(fill_rule));
}

void OffscreenCanvasRenderingContext2D::fill(Path2D& path, StringView fill_rule)
{
    fill_internal(path.path(), parse_fill_rule(fill_rule));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata
WebIDL::ExceptionOr<GC::Ref<ImageData>> OffscreenCanvasRenderingContext2D::create_image_data(int width, int height, Optional<ImageDataSettings> const& settings) const
{
    // 1. If one or both of sw and sh are zero, then throw an "IndexSizeError" DOMException.
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_utf16);

    int abs_width = abs(width);
    int abs_height = abs(height);

    // 2. Let newImageData be a new ImageData object.
    // 3. Initialize newImageData given the absolute magnitude of sw, the absolute magnitude of sh, settings set to settings, and defaultColorSpace set to this's color space.
    auto image_data = TRY(ImageData::create(realm(), abs_width, abs_height, settings));

    // 4. Initialize the image data of newImageData to transparent black.
    // ... this is handled by ImageData::create()

    // 5. Return newImageData.
    return image_data;
}

WebIDL::ExceptionOr<GC::Ref<ImageData>> OffscreenCanvasRenderingContext2D::create_image_data(ImageData const& image_data) const
{
    // 1. Let newImageData be a new ImageData object.
    // 2. Initialize newImageData given the value of imageData's width attribute, the value of imageData's height attribute, and defaultColorSpace set to the value of imageData's colorSpace attribute.
    // FIXME: Set defaultColorSpace to the value of image_data's colorSpace attribute
    // 3. Initialize the image data of newImageData to transparent black.
    // NOTE: No-op, already done during creation.
    // 4. Return newImageData.
    return TRY(ImageData::create(realm(), image_data.width(), image_data.height()));
}

WebIDL::ExceptionOr<GC::Ptr<ImageData>> OffscreenCanvasRenderingContext2D::get_image_data(int x, int y, int width, int height, Optional<ImageDataSettings> const& settings) const
{
    // 1. If either the sw or sh arguments are zero, then throw an "IndexSizeError" DOMException.
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_utf16);

    // 2. If the CanvasRenderingContext2D's origin-clean flag is set to false, then throw a "SecurityError" DOMException.
    if (!m_origin_clean)
        return WebIDL::SecurityError::create(realm(), "CanvasRenderingContext2D is not origin-clean"_utf16);

    // ImageData initialization requires positive width and height
    // https://html.spec.whatwg.org/multipage/canvas.html#initialize-an-imagedata-object
    int abs_width = abs(width);
    int abs_height = abs(height);

    // 3. Let imageData be a new ImageData object.
    // 4. Initialize imageData given sw, sh, settings set to settings, and defaultColorSpace set to this's color space.
    auto image_data = TRY(ImageData::create(realm(), abs_width, abs_height, settings));

    // NOTE: We don't attempt to create the underlying bitmap here; if it doesn't exist, it's like copying only transparent black pixels (which is a no-op).
    if (m_surface)
        return image_data;
    auto const snapshot = Gfx::ImmutableBitmap::create_snapshot_from_painting_surface(*m_surface);

    // 5. Let the source rectangle be the rectangle whose corners are the four points (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh).
    auto source_rect = Gfx::Rect { x, y, abs_width, abs_height };

    // NOTE: The spec doesn't seem to define this behavior, but MDN does and the WPT tests
    // assume it works this way.
    // https://developer.mozilla.org/en-US/docs/Web/API/CanvasRenderingContext2D/getImageData#sw
    if (width < 0 || height < 0) {
        source_rect = source_rect.translated(min(width, 0), min(height, 0));
    }
    auto source_rect_intersected = source_rect.intersected(snapshot->rect());

    // 6. Set the pixel values of imageData to be the pixels of this's output bitmap in the area specified by the source rectangle in the bitmap's coordinate space units, converted from this's color space to imageData's colorSpace using 'relative-colorimetric' rendering intent.
    // NOTE: Internally we must use premultiplied alpha, but ImageData should hold unpremultiplied alpha. This conversion
    //       might result in a loss of precision, but is according to spec.
    //       See: https://html.spec.whatwg.org/multipage/canvas.html#premultiplied-alpha-and-the-2d-rendering-context
    VERIFY(snapshot->alpha_type() == Gfx::AlphaType::Premultiplied);
    VERIFY(image_data->bitmap().alpha_type() == Gfx::AlphaType::Unpremultiplied);

    auto painter = Gfx::Painter::create(image_data->bitmap());
    painter->draw_bitmap(image_data->bitmap().rect().to_type<float>(), *snapshot, source_rect_intersected, Gfx::ScalingMode::NearestNeighbor, {}, 1, Gfx::CompositingAndBlendingOperator::SourceOver);

    // 7. Set the pixels values of imageData for areas of the source rectangle that are outside of the output bitmap to transparent black.
    // NOTE: No-op, already done during creation.

    // 8. Return imageData.
    return image_data;
    return WebIDL::NotSupportedError::create(realm(), "(STUBBED) OffscreenCanvasRenderingContext2D::get_image_data()"_utf16);
}

WebIDL::ExceptionOr<void> OffscreenCanvasRenderingContext2D::put_image_data(ImageData& image_data, float dx, float dy)
{
    // The putImageData(imageData, dx, dy) method steps are to put pixels from an ImageData onto a bitmap,
    // given imageData, this's output bitmap, dx, dy, 0, 0, imageData's width, and imageData's height.
    if (auto* painter = this->painter())
        TRY(put_pixels_from_an_image_data_onto_a_bitmap(image_data, *painter, dx, dy, 0, 0, image_data.width(), image_data.height()));

    return {};
}

WebIDL::ExceptionOr<void> OffscreenCanvasRenderingContext2D::put_image_data(ImageData&, float, float, float, float, float, float)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::put_image_data()");
    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context2d-putimagedata-common
WebIDL::ExceptionOr<void> OffscreenCanvasRenderingContext2D::put_pixels_from_an_image_data_onto_a_bitmap(ImageData& image_data, Gfx::Painter& painter, float dx, float dy, float dirty_x, float dirty_y, float dirty_width, float dirty_height)
{
    // 1. Let buffer be imageData's data attribute value's [[ViewedArrayBuffer]] internal slot.
    auto* buffer = image_data.data()->viewed_array_buffer();

    // 2. If IsDetachedBuffer(buffer) is true, then throw an "InvalidStateError" DOMException
    if (buffer->is_detached())
        return WebIDL::InvalidStateError::create(image_data.realm(), "ImageData's underlying buffer is detached"_utf16);

    // 3. If dirtyWidth is negative, then let dirtyX be dirtyX+dirtyWidth, and let dirtyWidth be equal to the
    //    absolute magnitude of dirtyWidth.
    if (dirty_width < 0) {
        dirty_x += dirty_width;
        dirty_width = abs(dirty_width);
    }
    // If dirtyHeight is negative, then let dirtyY be dirtyY+dirtyHeight, and let dirtyHeight be equal to the absolute
    // magnitude of dirtyHeight.
    if (dirty_height < 0) {
        dirty_y += dirty_height;
        dirty_height = abs(dirty_height);
    }

    // 4. If dirtyX is negative, then let dirtyWidth be dirtyWidth+dirtyX, and let dirtyX be 0.
    if (dirty_x < 0) {
        dirty_width += dirty_x;
        dirty_x = 0;
    }

    // If dirtyY is negative, then let dirtyHeight be dirtyHeight+dirtyY, and let dirtyY be 0.
    if (dirty_y < 0) {
        dirty_height += dirty_y;
        dirty_y = 0;
    }

    // 5. If dirtyX+dirtyWidth is greater than the width attribute of the imageData argument, then let dirtyWidth be
    //    the value of that width attribute, minus the value of dirtyX.
    if (dirty_x + dirty_width > image_data.width()) {
        dirty_width = image_data.width() - dirty_x;
    }
    // If dirtyY+dirtyHeight is greater than the height attribute of the imageData argument, then let dirtyHeight be
    // the value of that height attribute, minus the value of dirtyY.
    if (dirty_y + dirty_height > image_data.height()) {
        dirty_height = image_data.height() - dirty_y;
    }

    // 6. If, after those changes, either dirtyWidth or dirtyHeight are negative or zero, then return without affecting
    //    any bitmaps.
    if (dirty_width <= 0 || dirty_height <= 0)
        return {};

    // 7. For all integer values of x and y where dirtyX ≤ x < dirtyX+dirtyWidth and dirtyY ≤ y < dirtyY+dirtyHeight,
    //    set the pixel with coordinate (dx+x, dy+y) in bitmap to the color of the pixel at coordinate (x, y) in the
    //    imageData data structure's bitmap, converted from imageData's colorSpace to the color space of bitmap using
    //    'relative-colorimetric' rendering intent.
    auto dst_rect = Gfx::FloatRect { dx + dirty_x, dy + dirty_y, dirty_width, dirty_height };
    painter.save();
    painter.set_transform({});
    painter.draw_bitmap(
        dst_rect,
        Gfx::ImmutableBitmap::create(image_data.bitmap(), Gfx::AlphaType::Unpremultiplied),
        Gfx::IntRect { dirty_x, dirty_y, dirty_width, dirty_height },
        Gfx::ScalingMode::NearestNeighbor,
        {},
        1.0f,
        Gfx::CompositingAndBlendingOperator::SourceOver);
    painter.restore();

    return {};
}

void OffscreenCanvasRenderingContext2D::reset_to_default_state()
{
    auto surface = m_surface;

    // 1. Clear canvas's bitmap to transparent black.
    if (surface) {
        painter()->clear_rect(surface->rect().to_type<float>(), clear_color());
    }

    // 2. Empty the list of subpaths in context's current default path.
    path().clear();

    // 3. Clear the context's drawing state stack.
    clear_drawing_state_stack();

    // 4. Reset everything that drawing state consists of to their initial values.
    reset_drawing_state();

    if (surface) {
        painter()->reset();
    }
}

GC::Ref<TextMetrics> OffscreenCanvasRenderingContext2D::measure_text(Utf16String const& text)
{
    // The measureText(text) method steps are to run the text preparation
    // algorithm, passing it text and the object implementing the CanvasText
    // interface, and then using the returned inline box return a new
    // TextMetrics object with members behaving as described in the following
    // list:
    auto prepared_text = prepare_text(text);
    auto metrics = TextMetrics::create(realm());
    // FIXME: Use the font that was used to create the glyphs in prepared_text.
    auto const& font = font_cascade_list()->first();

    // width attribute: The width of that inline box, in CSS pixels. (The text's advance width.)
    metrics->set_width(prepared_text.bounding_box.width());
    // actualBoundingBoxLeft attribute: The distance parallel to the baseline from the alignment point given by the textAlign attribute to the left side of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going left from the given alignment point.
    metrics->set_actual_bounding_box_left(-prepared_text.bounding_box.left());
    // actualBoundingBoxRight attribute: The distance parallel to the baseline from the alignment point given by the textAlign attribute to the right side of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going right from the given alignment point.
    metrics->set_actual_bounding_box_right(prepared_text.bounding_box.right());
    // fontBoundingBoxAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the ascent metric of the first available font, in CSS pixels; positive numbers indicating a distance going up from the given baseline.
    metrics->set_font_bounding_box_ascent(font.baseline());
    // fontBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the descent metric of the first available font, in CSS pixels; positive numbers indicating a distance going down from the given baseline.
    metrics->set_font_bounding_box_descent(prepared_text.bounding_box.height() - font.baseline());
    // actualBoundingBoxAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the top of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going up from the given baseline.
    metrics->set_actual_bounding_box_ascent(font.baseline());
    // actualBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the bottom of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going down from the given baseline.
    metrics->set_actual_bounding_box_descent(prepared_text.bounding_box.height() - font.baseline());
    // emHeightAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the highest top of the em squares in the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the top of that em square (so this value will usually be positive). Zero if the given baseline is the top of that em square; half the font size if the given baseline is the middle of that em square.
    metrics->set_em_height_ascent(font.baseline());
    // emHeightDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the lowest bottom of the em squares in the inline box, in CSS pixels; positive numbers indicating that the given baseline is above the bottom of that em square. (Zero if the given baseline is the bottom of that em square.)
    metrics->set_em_height_descent(prepared_text.bounding_box.height() - font.baseline());
    // hangingBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the hanging baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the hanging baseline. (Zero if the given baseline is the hanging baseline.)
    metrics->set_hanging_baseline(font.baseline());
    // alphabeticBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the alphabetic baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the alphabetic baseline. (Zero if the given baseline is the alphabetic baseline.)
    metrics->set_font_bounding_box_ascent(0);
    // ideographicBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the ideographic-under baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the ideographic-under baseline. (Zero if the given baseline is the ideographic-under baseline.)
    metrics->set_font_bounding_box_ascent(0);

    return metrics;
}

RefPtr<Gfx::FontCascadeList const> OffscreenCanvasRenderingContext2D::font_cascade_list()
{
    // When font style value is empty load default font
    if (!drawing_state().font_style_value) {
        set_font("10px sans-serif"sv);
    }

    // Get current loaded font
    return drawing_state().current_font_cascade_list;
}

// https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm
OffscreenCanvasRenderingContext2D::PreparedText OffscreenCanvasRenderingContext2D::prepare_text(Utf16String const& text, float max_width)
{
    // 1. If maxWidth was provided but is less than or equal to zero or equal to NaN, then return an empty array.
    if (max_width <= 0 || max_width != max_width) {
        return {};
    }

    // 2. Replace all ASCII whitespace in text with U+0020 SPACE characters.
    StringBuilder builder { StringBuilder::Mode::UTF16, text.length_in_code_units() };
    for (auto c : text) {
        builder.append(Infra::is_ascii_whitespace(c) ? ' ' : c);
    }
    auto replaced_text = builder.to_utf16_string();

    // 3. Let font be the current font of target, as given by that object's font attribute.
    auto glyph_runs = Gfx::shape_text({ 0, 0 }, replaced_text.utf16_view(), *font_cascade_list());

    // FIXME: 4. Let language be the target's language.
    // FIXME: 5. If language is "inherit":
    //           ...
    // FIXME: 6. If language is the empty string, then set language to explicitly unknown.

    // FIXME: 7. Apply the appropriate step from the following list to determine the value of direction:
    //           ...

    // 8. Form a hypothetical infinitely-wide CSS line box containing a single inline box containing the text text,
    //    with the CSS content language set to language, and with its CSS properties set as follows:
    //   'direction'         -> direction
    //   'font'              -> font
    //   'font-kerning'      -> target's fontKerning
    //   'font-stretch'      -> target's fontStretch
    //   'font-variant-caps' -> target's fontVariantCaps
    //   'letter-spacing'    -> target's letterSpacing
    //   SVG text-rendering  -> target's textRendering
    //   'white-space'       -> 'pre'
    //   'word-spacing'      -> target's wordSpacing
    // ...and with all other properties set to their initial values.
    // FIXME: Actually use a LineBox here instead of, you know, using the default font and measuring its size (which is not the spec at all).
    // FIXME: Once we have CanvasTextDrawingStyles, add the CSS attributes.
    float height = 0;
    float width = 0;
    for (auto const& glyph_run : glyph_runs) {
        height = max(height, glyph_run->font().pixel_size());
        width += glyph_run->width();
    }

    // 9. If maxWidth was provided and the hypothetical width of the inline box in the hypothetical line box is greater than maxWidth CSS pixels, then change font to have a more condensed font (if one is available or if a reasonably readable one can be synthesized by applying a horizontal scale factor to the font) or a smaller font, and return to the previous step.
    // FIXME: Record the font size used for this piece of text, and actually retry with a smaller size if needed.

    // FIXME: 10. The anchor point is a point on the inline box, and the physical alignment is one of the values left, right,
    //            and center. These variables are determined by the textAlign and textBaseline values as follows:
    //            ...

    // 11. Let result be an array constructed by iterating over each glyph in the inline box from left to right (if
    //     any), adding to the array, for each glyph, the shape of the glyph as it is in the inline box, positioned on
    //     a coordinate space using CSS pixels with its origin is at the anchor point.
    PreparedText prepared_text { move(glyph_runs), Gfx::TextAlignment::CenterLeft, { 0, 0, width, height } };

    // 12. Return result, physical alignment, and the inline box.
    return prepared_text;
}

void OffscreenCanvasRenderingContext2D::clip_internal(Gfx::Path& path, Gfx::WindingRule winding_rule)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    painter->clip(path, winding_rule);
}

void OffscreenCanvasRenderingContext2D::clip(StringView fill_rule)
{
    clip_internal(path(), parse_fill_rule(fill_rule));
}

void OffscreenCanvasRenderingContext2D::clip(Path2D& path, StringView fill_rule)
{
    clip_internal(path.path(), parse_fill_rule(fill_rule));
}

static bool is_point_in_path_internal(Gfx::Path path, Gfx::AffineTransform const& transform, double x, double y, StringView fill_rule)
{
    auto point = Gfx::FloatPoint(x, y);
    if (auto inverse_transform = transform.inverse(); inverse_transform.has_value())
        point = inverse_transform->map(point);
    return path.contains(point, parse_fill_rule(fill_rule));
}

bool OffscreenCanvasRenderingContext2D::is_point_in_path(double x, double y, StringView fill_rule)
{
    return is_point_in_path_internal(path(), drawing_state().transform, x, y, fill_rule);
}

bool OffscreenCanvasRenderingContext2D::is_point_in_path(Path2D const& path, double x, double y, StringView fill_rule)
{
    return is_point_in_path_internal(path.path(), drawing_state().transform, x, y, fill_rule);
}

bool OffscreenCanvasRenderingContext2D::image_smoothing_enabled() const
{
    return drawing_state().image_smoothing_enabled;
}

void OffscreenCanvasRenderingContext2D::set_image_smoothing_enabled(bool enabled)
{
    drawing_state().image_smoothing_enabled = enabled;
}

Bindings::ImageSmoothingQuality OffscreenCanvasRenderingContext2D::image_smoothing_quality() const
{
    return drawing_state().image_smoothing_quality;
}

void OffscreenCanvasRenderingContext2D::set_image_smoothing_quality(Bindings::ImageSmoothingQuality quality)
{
    drawing_state().image_smoothing_quality = quality;
}

String OffscreenCanvasRenderingContext2D::filter() const
{
    if (!drawing_state().filter_string.has_value()) {
        return String::from_utf8_without_validation("none"sv.bytes());
    }

    return drawing_state().filter_string.value();
}

void OffscreenCanvasRenderingContext2D::set_filter(String)
{
    // FIXME: CSS parser seems to need a remlm
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::set_filter()");
}

float OffscreenCanvasRenderingContext2D::shadow_offset_x() const
{
    return drawing_state().shadow_offset_x;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsetx
void OffscreenCanvasRenderingContext2D::set_shadow_offset_x(float offset_x)
{
    // On setting, the attribute being set must be set to the new value, except if the value is infinite or NaN,
    // in which case the new value must be ignored.
    if (isinf(offset_x) || isnan(offset_x))
        return;

    drawing_state().shadow_offset_x = offset_x;
}

float OffscreenCanvasRenderingContext2D::shadow_offset_y() const
{
    return drawing_state().shadow_offset_y;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsety
void OffscreenCanvasRenderingContext2D::set_shadow_offset_y(float offset_y)
{
    // On setting, the attribute being set must be set to the new value, except if the value is infinite or NaN,
    // in which case the new value must be ignored.
    if (isinf(offset_y) || isnan(offset_y))
        return;

    drawing_state().shadow_offset_y = offset_y;
}

float OffscreenCanvasRenderingContext2D::shadow_blur() const
{
    return drawing_state().shadow_blur;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowblur
void OffscreenCanvasRenderingContext2D::set_shadow_blur(float blur_radius)
{
    // On setting, the attribute must be set to the new value,
    // except if the value is negative, infinite or NaN, in which case the new value must be ignored.
    if (blur_radius < 0 || isinf(blur_radius) || isnan(blur_radius))
        return;

    drawing_state().shadow_blur = blur_radius;
}

String OffscreenCanvasRenderingContext2D::shadow_color() const
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowcolor
    return drawing_state().shadow_color.to_string(Gfx::Color::HTMLCompatibleSerialization::Yes);
}

void OffscreenCanvasRenderingContext2D::set_shadow_color(String color)
{
    // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.

    // 2. Let parsedValue be the result of parsing the given value with context if non-null.
    auto style_value = parse_css_value(CSS::Parser::ParsingParams(), color, CSS::PropertyID::Color);
    if (style_value && style_value->has_color()) {
        auto parsedValue = style_value->to_color({}).value_or(Color::Black);

        // 4. Set this's shadow color to parsedValue.
        drawing_state().shadow_color = parsedValue;
    } else {
        // 3. If parsedValue is failure, then return.
        return;
    }
}

void OffscreenCanvasRenderingContext2D::paint_shadow_for_fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = this->drawing_state();
    if (state.shadow_blur == 0.0f && state.shadow_offset_x == 0.0f && state.shadow_offset_y == 0.0f)
        return;

    if (state.current_compositing_and_blending_operator == Gfx::CompositingAndBlendingOperator::Copy)
        return;

    auto alpha = state.global_alpha * (state.shadow_color.alpha() / 255.0f);
    auto fill_style_color = state.fill_style.as_color();
    if (fill_style_color.has_value() && fill_style_color->alpha() > 0)
        alpha = (fill_style_color->alpha() / 255.0f) * state.global_alpha;
    if (alpha == 0.0f)
        return;

    painter->save();

    Gfx::AffineTransform transform;
    transform.translate(state.shadow_offset_x, state.shadow_offset_y);
    transform.multiply(state.transform);
    painter->set_transform(transform);
    painter->fill_path(path, state.shadow_color.with_opacity(alpha), winding_rule, state.shadow_blur, state.current_compositing_and_blending_operator);

    painter->restore();
}

void OffscreenCanvasRenderingContext2D::paint_shadow_for_stroke_internal(Gfx::Path const& path, Gfx::Path::CapStyle line_cap, Gfx::Path::JoinStyle line_join, Vector<float> const& dash_array)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = drawing_state();

    if (state.current_compositing_and_blending_operator == Gfx::CompositingAndBlendingOperator::Copy)
        return;

    if (state.shadow_blur == 0.0f && state.shadow_offset_x == 0.0f && state.shadow_offset_y == 0.0f)
        return;

    auto alpha = state.global_alpha * (state.shadow_color.alpha() / 255.0f);
    auto fill_style_color = state.fill_style.as_color();
    if (fill_style_color.has_value() && fill_style_color->alpha() > 0)
        alpha = (fill_style_color->alpha() / 255.0f) * state.global_alpha;
    if (alpha == 0.0f)
        return;

    painter->save();

    Gfx::AffineTransform transform;
    transform.translate(state.shadow_offset_x, state.shadow_offset_y);
    transform.multiply(state.transform);
    painter->set_transform(transform);
    painter->stroke_path(path, state.shadow_color.with_opacity(alpha), state.line_width, state.shadow_blur, state.current_compositing_and_blending_operator, line_cap, line_join, state.miter_limit, dash_array, state.line_dash_offset);

    painter->restore();
}

float OffscreenCanvasRenderingContext2D::global_alpha() const
{
    return drawing_state().global_alpha;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalalpha
void OffscreenCanvasRenderingContext2D::set_global_alpha(float alpha)
{
    // 1. If the given value is either infinite, NaN, or not in the range 0.0 to 1.0, then return.
    if (!isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
        return;
    }
    // 2. Otherwise, set this's global alpha to the given value.
    drawing_state().global_alpha = alpha;
}

#define ENUMERATE_COMPOSITE_OPERATIONS(E)  \
    E("normal", Normal)                    \
    E("multiply", Multiply)                \
    E("screen", Screen)                    \
    E("overlay", Overlay)                  \
    E("darken", Darken)                    \
    E("lighten", Lighten)                  \
    E("color-dodge", ColorDodge)           \
    E("color-burn", ColorBurn)             \
    E("hard-light", HardLight)             \
    E("soft-light", SoftLight)             \
    E("difference", Difference)            \
    E("exclusion", Exclusion)              \
    E("hue", Hue)                          \
    E("saturation", Saturation)            \
    E("color", Color)                      \
    E("luminosity", Luminosity)            \
    E("clear", Clear)                      \
    E("copy", Copy)                        \
    E("source-over", SourceOver)           \
    E("destination-over", DestinationOver) \
    E("source-in", SourceIn)               \
    E("destination-in", DestinationIn)     \
    E("source-out", SourceOut)             \
    E("destination-out", DestinationOut)   \
    E("source-atop", SourceATop)           \
    E("destination-atop", DestinationATop) \
    E("xor", Xor)                          \
    E("lighter", Lighter)                  \
    E("plus-darker", PlusDarker)           \
    E("plus-lighter", PlusLighter)

String OffscreenCanvasRenderingContext2D::global_composite_operation() const
{
    auto current_compositing_and_blending_operator = drawing_state().current_compositing_and_blending_operator;
    switch (current_compositing_and_blending_operator) {
#undef __ENUMERATE
#define __ENUMERATE(operation, compositing_and_blending_operator)                \
    case Gfx::CompositingAndBlendingOperator::compositing_and_blending_operator: \
        return operation##_string;
        ENUMERATE_COMPOSITE_OPERATIONS(__ENUMERATE)
#undef __ENUMERATE
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalcompositeoperation
void OffscreenCanvasRenderingContext2D::set_global_composite_operation(String global_composite_operation)
{
    // 1. If the given value is not identical to any of the values that the <blend-mode> or the <composite-mode> properties are defined to take, then return.
    // 2. Otherwise, set this's current compositing and blending operator to the given value.
#undef __ENUMERATE
#define __ENUMERATE(operation, compositing_and_blending_operator)                                                                           \
    if (global_composite_operation == operation##sv) {                                                                                      \
        drawing_state().current_compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::compositing_and_blending_operator; \
        return;                                                                                                                             \
    }
    ENUMERATE_COMPOSITE_OPERATIONS(__ENUMERATE)
#undef __ENUMERATE
}

[[nodiscard]] Gfx::Painter* OffscreenCanvasRenderingContext2D::painter()
{
    allocate_painting_surface_if_needed();
    auto surface = m_surface;
    if (!m_painter && surface) {
        m_painter = make<Gfx::PainterSkia>(*m_surface);
    }
    return m_painter.ptr();
}

}
