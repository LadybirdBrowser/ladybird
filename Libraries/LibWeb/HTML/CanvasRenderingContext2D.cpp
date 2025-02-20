/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/Rect.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/Bindings/CanvasRenderingContext2DPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
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

GC_DEFINE_ALLOCATOR(CanvasRenderingContext2D);

GC::Ref<CanvasRenderingContext2D> CanvasRenderingContext2D::create(JS::Realm& realm, HTMLCanvasElement& element)
{
    return realm.create<CanvasRenderingContext2D>(realm, element);
}

CanvasRenderingContext2D::CanvasRenderingContext2D(JS::Realm& realm, HTMLCanvasElement& element)
    : PlatformObject(realm)
    , CanvasPath(static_cast<Bindings::PlatformObject&>(*this), *this)
    , m_element(element)
    , m_size(element.bitmap_size_for_canvas())
{
}

CanvasRenderingContext2D::~CanvasRenderingContext2D() = default;

void CanvasRenderingContext2D::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    set_prototype(&Bindings::ensure_web_prototype<Bindings::CanvasRenderingContext2DPrototype>(realm, "CanvasRenderingContext2D"_string));
}

void CanvasRenderingContext2D::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

HTMLCanvasElement& CanvasRenderingContext2D::canvas_element()
{
    return *m_element;
}

HTMLCanvasElement const& CanvasRenderingContext2D::canvas_element() const
{
    return *m_element;
}

GC::Ref<HTMLCanvasElement> CanvasRenderingContext2D::canvas_for_binding() const
{
    return *m_element;
}

Gfx::Path CanvasRenderingContext2D::rect_path(float x, float y, float width, float height)
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

void CanvasRenderingContext2D::fill_rect(float x, float y, float width, float height)
{
    fill_internal(rect_path(x, y, width, height), Gfx::WindingRule::EvenOdd);
}

void CanvasRenderingContext2D::clear_rect(float x, float y, float width, float height)
{
    if (auto* painter = this->painter()) {
        auto rect = Gfx::FloatRect(x, y, width, height);
        painter->clear_rect(rect, Color::Transparent);
        did_draw(rect);
    }
}

void CanvasRenderingContext2D::stroke_rect(float x, float y, float width, float height)
{
    stroke_internal(rect_path(x, y, width, height));
}

// 4.12.5.1.14 Drawing images, https://html.spec.whatwg.org/multipage/canvas.html#drawing-images
WebIDL::ExceptionOr<void> CanvasRenderingContext2D::draw_image_internal(CanvasImageSource const& image, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(source_x) || !isfinite(source_y) || !isfinite(source_width) || !isfinite(source_height) || !isfinite(destination_x) || !isfinite(destination_y) || !isfinite(destination_width) || !isfinite(destination_height))
        return {};

    // 2. Let usability be the result of checking the usability of image.
    auto usability = TRY(check_usability_of_image(image));

    // 3. If usability is bad, then return (without drawing anything).
    if (usability == CanvasImageSourceUsability::Bad)
        return {};

    auto bitmap = image.visit(
        [](GC::Root<HTMLImageElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return source->immutable_bitmap();
        },
        [](GC::Root<SVG::SVGImageElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return source->current_image_bitmap();
        },
        [](GC::Root<HTMLCanvasElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            auto surface = source->surface();
            if (!surface)
                return {};
            return Gfx::ImmutableBitmap::create_snapshot_from_painting_surface(*surface);
        },
        [](GC::Root<HTMLVideoElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> { return Gfx::ImmutableBitmap::create(*source->bitmap()); },
        [](GC::Root<ImageBitmap> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(*source->bitmap());
        });
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
        scaling_mode = Gfx::ScalingMode::BilinearBlend;
    }

    if (auto* painter = this->painter()) {
        painter->draw_bitmap(destination_rect, *bitmap, source_rect.to_rounded<int>(), scaling_mode, drawing_state().filters, drawing_state().global_alpha, drawing_state().current_compositing_and_blending_operator);
        did_draw(destination_rect);
    }

    // 7. If image is not origin-clean, then set the CanvasRenderingContext2D's origin-clean flag to false.
    if (image_is_not_origin_clean(image))
        m_origin_clean = false;

    return {};
}

void CanvasRenderingContext2D::did_draw(Gfx::FloatRect const&)
{
    // FIXME: Make use of the rect to reduce the invalidated area when possible.
    if (!canvas_element().paintable())
        return;
    canvas_element().paintable()->set_needs_display(InvalidateDisplayList::No);
}

Gfx::Painter* CanvasRenderingContext2D::painter()
{
    allocate_painting_surface_if_needed();
    auto surface = canvas_element().surface();
    if (!m_painter && surface) {
        canvas_element().document().invalidate_display_list();
        m_painter = make<Gfx::PainterSkia>(*canvas_element().surface());
    }
    return m_painter.ptr();
}

void CanvasRenderingContext2D::set_size(Gfx::IntSize const& size)
{
    if (m_size == size)
        return;
    m_size = size;
    m_surface = nullptr;
}

void CanvasRenderingContext2D::allocate_painting_surface_if_needed()
{
    if (m_surface || m_size.is_empty())
        return;
    auto skia_backend_context = canvas_element().navigable()->traversable_navigable()->skia_backend_context();
    m_surface = Gfx::PaintingSurface::create_with_size(skia_backend_context, canvas_element().bitmap_size_for_canvas(), Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
}

Gfx::Path CanvasRenderingContext2D::text_path(StringView text, float x, float y, Optional<double> max_width)
{
    if (max_width.has_value() && max_width.value() <= 0)
        return {};

    auto& drawing_state = this->drawing_state();
    auto font = current_font();

    Gfx::Path path;
    path.move_to({ x, y });
    path.text(Utf8View { text }, *font);

    auto text_width = path.bounding_box().width();
    Gfx::AffineTransform transform = {};

    // https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm:
    // 6. If maxWidth was provided and the hypothetical width of the inline box in the hypothetical line box
    // is greater than maxWidth CSS pixels, then change font to have a more condensed font (if one is
    // available or if a reasonably readable one can be synthesized by applying a horizontal scale
    // factor to the font) or a smaller font, and return to the previous step.
    if (max_width.has_value() && text_width > float(*max_width)) {
        auto horizontal_scale = float(*max_width) / text_width;
        transform = Gfx::AffineTransform {}.scale({ horizontal_scale, 1 });
        text_width *= horizontal_scale;
    }

    // Apply text align
    // FIXME: CanvasTextAlign::Start and CanvasTextAlign::End currently do not nothing for right-to-left languages:
    //        https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textalign-start
    // Default alignment of draw_text is left so do nothing by CanvasTextAlign::Start and CanvasTextAlign::Left
    if (drawing_state.text_align == Bindings::CanvasTextAlign::Center) {
        transform = Gfx::AffineTransform {}.set_translation({ -text_width / 2, 0 }).multiply(transform);
    }
    if (drawing_state.text_align == Bindings::CanvasTextAlign::End || drawing_state.text_align == Bindings::CanvasTextAlign::Right) {
        transform = Gfx::AffineTransform {}.set_translation({ -text_width, 0 }).multiply(transform);
    }

    // Apply text baseline
    // FIXME: Implement CanvasTextBasline::Hanging, Bindings::CanvasTextAlign::Alphabetic and Bindings::CanvasTextAlign::Ideographic for real
    //        right now they are just handled as textBaseline = top or bottom.
    //        https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textbaseline-hanging
    // Default baseline of draw_text is top so do nothing by CanvasTextBaseline::Top and CanvasTextBasline::Hanging
    if (drawing_state.text_baseline == Bindings::CanvasTextBaseline::Middle) {
        transform = Gfx::AffineTransform {}.set_translation({ 0, font->pixel_size() / 2 }).multiply(transform);
    }
    if (drawing_state.text_baseline == Bindings::CanvasTextBaseline::Top || drawing_state.text_baseline == Bindings::CanvasTextBaseline::Hanging) {
        transform = Gfx::AffineTransform {}.set_translation({ 0, font->pixel_size() }).multiply(transform);
    }

    return path.copy_transformed(transform);
}

void CanvasRenderingContext2D::fill_text(StringView text, float x, float y, Optional<double> max_width)
{
    fill_internal(text_path(text, x, y, max_width), Gfx::WindingRule::Nonzero);
}

void CanvasRenderingContext2D::stroke_text(StringView text, float x, float y, Optional<double> max_width)
{
    stroke_internal(text_path(text, x, y, max_width));
}

void CanvasRenderingContext2D::begin_path()
{
    path().clear();
}

void CanvasRenderingContext2D::stroke_internal(Gfx::Path const& path)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    paint_shadow_for_stroke_internal(path);

    auto& state = drawing_state();

    // FIXME: Honor state's line_cap, line_join, miter_limit, dash_list, and line_dash_offset.
    painter->stroke_path(path, state.stroke_style.to_gfx_paint_style(), state.filters, state.line_width, state.global_alpha, state.current_compositing_and_blending_operator);

    did_draw(path.bounding_box());
}

void CanvasRenderingContext2D::stroke()
{
    stroke_internal(path());
}

void CanvasRenderingContext2D::stroke(Path2D const& path)
{
    stroke_internal(path.path());
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

void CanvasRenderingContext2D::fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    paint_shadow_for_fill_internal(path, winding_rule);

    auto path_to_fill = path;
    path_to_fill.close_all_subpaths();
    auto& state = this->drawing_state();
    painter->fill_path(path_to_fill, state.fill_style.to_gfx_paint_style(), state.filters, state.global_alpha, state.current_compositing_and_blending_operator, winding_rule);

    did_draw(path_to_fill.bounding_box());
}

void CanvasRenderingContext2D::fill(StringView fill_rule)
{
    fill_internal(path(), parse_fill_rule(fill_rule));
}

void CanvasRenderingContext2D::fill(Path2D& path, StringView fill_rule)
{
    fill_internal(path.path(), parse_fill_rule(fill_rule));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata
WebIDL::ExceptionOr<GC::Ref<ImageData>> CanvasRenderingContext2D::create_image_data(int width, int height, Optional<ImageDataSettings> const& settings) const
{
    // 1. If one or both of sw and sh are zero, then throw an "IndexSizeError" DOMException.
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_string);

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

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata-imagedata
WebIDL::ExceptionOr<GC::Ref<ImageData>> CanvasRenderingContext2D::create_image_data(ImageData const& image_data) const
{
    // 1. Let newImageData be a new ImageData object.
    // 2. Initialize newImageData given the value of imageData's width attribute, the value of imageData's height attribute, and defaultColorSpace set to the value of imageData's colorSpace attribute.
    // FIXME: Set defaultColorSpace to the value of image_data's colorSpace attribute
    // 3. Initialize the image data of newImageData to transparent black.
    // NOTE: No-op, already done during creation.
    // 4. Return newImageData.
    return TRY(ImageData::create(realm(), image_data.width(), image_data.height()));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-getimagedata
WebIDL::ExceptionOr<GC::Ptr<ImageData>> CanvasRenderingContext2D::get_image_data(int x, int y, int width, int height, Optional<ImageDataSettings> const& settings) const
{
    // 1. If either the sw or sh arguments are zero, then throw an "IndexSizeError" DOMException.
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_string);

    // 2. If the CanvasRenderingContext2D's origin-clean flag is set to false, then throw a "SecurityError" DOMException.
    if (!m_origin_clean)
        return WebIDL::SecurityError::create(realm(), "CanvasRenderingContext2D is not origin-clean"_string);

    // ImageData initialization requires positive width and height
    // https://html.spec.whatwg.org/multipage/canvas.html#initialize-an-imagedata-object
    int abs_width = abs(width);
    int abs_height = abs(height);

    // 3. Let imageData be a new ImageData object.
    // 4. Initialize imageData given sw, sh, settings set to settings, and defaultColorSpace set to this's color space.
    auto image_data = TRY(ImageData::create(realm(), abs_width, abs_height, settings));

    // NOTE: We don't attempt to create the underlying bitmap here; if it doesn't exist, it's like copying only transparent black pixels (which is a no-op).
    if (!canvas_element().surface())
        return image_data;
    auto const snapshot = Gfx::ImmutableBitmap::create_snapshot_from_painting_surface(*canvas_element().surface());

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
    ASSERT(snapshot->alpha_type() == Gfx::AlphaType::Premultiplied);
    ASSERT(image_data->bitmap().alpha_type() == Gfx::AlphaType::Unpremultiplied);

    auto painter = Gfx::Painter::create(image_data->bitmap());
    painter->draw_bitmap(image_data->bitmap().rect().to_type<float>(), *snapshot, source_rect_intersected, Gfx::ScalingMode::NearestNeighbor, {}, drawing_state().global_alpha, Gfx::CompositingAndBlendingOperator::SourceOver);

    // 7. Set the pixels values of imageData for areas of the source rectangle that are outside of the output bitmap to transparent black.
    // NOTE: No-op, already done during creation.

    // 8. Return imageData.
    return image_data;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-putimagedata-short
void CanvasRenderingContext2D::put_image_data(ImageData const& image_data, float x, float y)
{
    // The putImageData(imageData, dx, dy) method steps are to put pixels from an ImageData onto a bitmap,
    // given imageData, this's output bitmap, dx, dy, 0, 0, imageData's width, and imageData's height.
    // FIXME: "put pixels from an ImageData onto a bitmap" is a spec algorithm.
    //        https://html.spec.whatwg.org/multipage/canvas.html#dom-context2d-putimagedata-common
    if (auto* painter = this->painter()) {
        auto dst_rect = Gfx::FloatRect(x, y, image_data.width(), image_data.height());
        painter->draw_bitmap(dst_rect, Gfx::ImmutableBitmap::create(image_data.bitmap()), image_data.bitmap().rect(), Gfx::ScalingMode::NearestNeighbor, drawing_state().filters, 1.0f, Gfx::CompositingAndBlendingOperator::SourceOver);
        did_draw(dst_rect);
    }
}

// https://html.spec.whatwg.org/multipage/canvas.html#reset-the-rendering-context-to-its-default-state
void CanvasRenderingContext2D::reset_to_default_state()
{
    auto surface = canvas_element().surface();

    // 1. Clear canvas's bitmap to transparent black.
    if (surface) {
        painter()->clear_rect(surface->rect().to_type<float>(), Color::Transparent);
    }

    // 2. Empty the list of subpaths in context's current default path.
    path().clear();

    // 3. Clear the context's drawing state stack.
    clear_drawing_state_stack();

    // 4. Reset everything that drawing state consists of to their initial values.
    reset_drawing_state();

    if (surface)
        did_draw(surface->rect().to_type<float>());
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-measuretext
GC::Ref<TextMetrics> CanvasRenderingContext2D::measure_text(StringView text)
{
    // The measureText(text) method steps are to run the text preparation
    // algorithm, passing it text and the object implementing the CanvasText
    // interface, and then using the returned inline box must return a new
    // TextMetrics object with members behaving as described in the following
    // list:
    auto prepared_text = prepare_text(text);
    auto metrics = TextMetrics::create(realm());
    // FIXME: Use the font that was used to create the glyphs in prepared_text.
    auto font = current_font();

    // width attribute: The width of that inline box, in CSS pixels. (The text's advance width.)
    metrics->set_width(prepared_text.bounding_box.width());
    // actualBoundingBoxLeft attribute: The distance parallel to the baseline from the alignment point given by the textAlign attribute to the left side of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going left from the given alignment point.
    metrics->set_actual_bounding_box_left(-prepared_text.bounding_box.left());
    // actualBoundingBoxRight attribute: The distance parallel to the baseline from the alignment point given by the textAlign attribute to the right side of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going right from the given alignment point.
    metrics->set_actual_bounding_box_right(prepared_text.bounding_box.right());
    // fontBoundingBoxAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the ascent metric of the first available font, in CSS pixels; positive numbers indicating a distance going up from the given baseline.
    metrics->set_font_bounding_box_ascent(font->baseline());
    // fontBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the descent metric of the first available font, in CSS pixels; positive numbers indicating a distance going down from the given baseline.
    metrics->set_font_bounding_box_descent(prepared_text.bounding_box.height() - font->baseline());
    // actualBoundingBoxAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the top of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going up from the given baseline.
    metrics->set_actual_bounding_box_ascent(font->baseline());
    // actualBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the bottom of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going down from the given baseline.
    metrics->set_actual_bounding_box_descent(prepared_text.bounding_box.height() - font->baseline());
    // emHeightAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the highest top of the em squares in the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the top of that em square (so this value will usually be positive). Zero if the given baseline is the top of that em square; half the font size if the given baseline is the middle of that em square.
    metrics->set_em_height_ascent(font->baseline());
    // emHeightDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the lowest bottom of the em squares in the inline box, in CSS pixels; positive numbers indicating that the given baseline is above the bottom of that em square. (Zero if the given baseline is the bottom of that em square.)
    metrics->set_em_height_descent(prepared_text.bounding_box.height() - font->baseline());
    // hangingBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the hanging baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the hanging baseline. (Zero if the given baseline is the hanging baseline.)
    metrics->set_hanging_baseline(font->baseline());
    // alphabeticBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the alphabetic baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the alphabetic baseline. (Zero if the given baseline is the alphabetic baseline.)
    metrics->set_font_bounding_box_ascent(0);
    // ideographicBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the ideographic-under baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the ideographic-under baseline. (Zero if the given baseline is the ideographic-under baseline.)
    metrics->set_font_bounding_box_ascent(0);

    return metrics;
}

RefPtr<Gfx::Font const> CanvasRenderingContext2D::current_font()
{
    // When font style value is empty load default font
    if (!drawing_state().font_style_value) {
        set_font("10px sans-serif"sv);
    }

    // Get current loaded font
    return drawing_state().current_font;
}

// https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm
CanvasRenderingContext2D::PreparedText CanvasRenderingContext2D::prepare_text(ByteString const& text, float max_width)
{
    // 1. If maxWidth was provided but is less than or equal to zero or equal to NaN, then return an empty array.
    if (max_width <= 0 || max_width != max_width) {
        return {};
    }

    // 2. Replace all ASCII whitespace in text with U+0020 SPACE characters.
    StringBuilder builder { text.length() };
    for (auto c : text) {
        builder.append(Infra::is_ascii_whitespace(c) ? ' ' : c);
    }
    auto replaced_text = MUST(builder.to_string());

    // 3. Let font be the current font of target, as given by that object's font attribute.
    auto font = current_font();

    // 4. Apply the appropriate step from the following list to determine the value of direction:
    //   4.1. If the target object's direction attribute has the value "ltr": Let direction be 'ltr'.
    //   4.2. If the target object's direction attribute has the value "rtl": Let direction be 'rtl'.
    //   4.3. If the target object's font style source object is an element: Let direction be the directionality of the target object's font style source object.
    //   4.4. If the target object's font style source object is a Document with a non-null document element: Let direction be the directionality of the target object's font style source object's document element.
    //   4.5. Otherwise: Let direction be 'ltr'.
    // FIXME: Once we have CanvasTextDrawingStyles, implement directionality.

    // 5. Form a hypothetical infinitely-wide CSS line box containing a single inline box containing the text text, with its CSS properties set as follows:
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
    size_t height = font->pixel_size();

    // 6. If maxWidth was provided and the hypothetical width of the inline box in the hypothetical line box is greater than maxWidth CSS pixels, then change font to have a more condensed font (if one is available or if a reasonably readable one can be synthesized by applying a horizontal scale factor to the font) or a smaller font, and return to the previous step.
    // FIXME: Record the font size used for this piece of text, and actually retry with a smaller size if needed.

    // 7. The anchor point is a point on the inline box, and the physical alignment is one of the values left, right, and center. These variables are determined by the textAlign and textBaseline values as follows:
    // Horizontal position:
    //   7.1. If textAlign is left, if textAlign is start and direction is 'ltr' or if textAlign is end and direction is 'rtl': Let the anchor point's horizontal position be the left edge of the inline box, and let physical alignment be left.
    //   7.2. If textAlign is right, if textAlign is end and direction is 'ltr' or if textAlign is start and direction is 'rtl': Let the anchor point's horizontal position be the right edge of the inline box, and let physical alignment be right.
    //   7.3. If textAlign is center: Let the anchor point's horizontal position be half way between the left and right edges of the inline box, and let physical alignment be center.
    // Vertical position:
    //   7.4. If textBaseline is top: Let the anchor point's vertical position be the top of the em box of the first available font of the inline box.
    //   7.5. If textBaseline is hanging: Let the anchor point's vertical position be the hanging baseline of the first available font of the inline box.
    //   7.6. If textBaseline is middle: Let the anchor point's vertical position be half way between the bottom and the top of the em box of the first available font of the inline box.
    //   7.7. If textBaseline is alphabetic: Let the anchor point's vertical position be the alphabetic baseline of the first available font of the inline box.
    //   7.8. If textBaseline is ideographic: Let the anchor point's vertical position be the ideographic-under baseline of the first available font of the inline box.
    //   7.9. If textBaseline is bottom: Let the anchor point's vertical position be the bottom of the em box of the first available font of the inline box.
    // FIXME: Once we have CanvasTextDrawingStyles, handle the alignment and baseline.
    Gfx::FloatPoint anchor { 0, 0 };
    auto physical_alignment = Gfx::TextAlignment::CenterLeft;

    auto glyph_run = Gfx::shape_text(anchor, 0, replaced_text.code_points(), *font, Gfx::GlyphRun::TextType::Ltr, {});

    // 8. Let result be an array constructed by iterating over each glyph in the inline box from left to right (if any), adding to the array, for each glyph, the shape of the glyph as it is in the inline box, positioned on a coordinate space using CSS pixels with its origin is at the anchor point.
    PreparedText prepared_text { glyph_run, physical_alignment, { 0, 0, static_cast<int>(glyph_run->width()), static_cast<int>(height) } };

    // 9. Return result, physical alignment, and the inline box.
    return prepared_text;
}

void CanvasRenderingContext2D::clip_internal(Gfx::Path& path, Gfx::WindingRule winding_rule)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    path.close_all_subpaths();
    painter->clip(path, winding_rule);
}

void CanvasRenderingContext2D::clip(StringView fill_rule)
{
    clip_internal(path(), parse_fill_rule(fill_rule));
}

void CanvasRenderingContext2D::clip(Path2D& path, StringView fill_rule)
{
    clip_internal(path.path(), parse_fill_rule(fill_rule));
}

static bool is_point_in_path_internal(Gfx::Path path, double x, double y, StringView fill_rule)
{
    return path.contains(Gfx::FloatPoint(x, y), parse_fill_rule(fill_rule));
}

bool CanvasRenderingContext2D::is_point_in_path(double x, double y, StringView fill_rule)
{
    return is_point_in_path_internal(path(), x, y, fill_rule);
}

bool CanvasRenderingContext2D::is_point_in_path(Path2D const& path, double x, double y, StringView fill_rule)
{
    return is_point_in_path_internal(path.path(), x, y, fill_rule);
}

// https://html.spec.whatwg.org/multipage/canvas.html#check-the-usability-of-the-image-argument
WebIDL::ExceptionOr<CanvasImageSourceUsability> check_usability_of_image(CanvasImageSource const& image)
{
    // 1. Switch on image:
    auto usability = TRY(image.visit(
        // HTMLOrSVGImageElement
        [](GC::Root<HTMLImageElement> const& image_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // FIXME: If image's current request's state is broken, then throw an "InvalidStateError" DOMException.

            // If image is not fully decodable, then return bad.
            if (!image_element->immutable_bitmap())
                return { CanvasImageSourceUsability::Bad };

            // If image has an intrinsic width or intrinsic height (or both) equal to zero, then return bad.
            if (image_element->immutable_bitmap()->width() == 0 || image_element->immutable_bitmap()->height() == 0)
                return { CanvasImageSourceUsability::Bad };
            return Optional<CanvasImageSourceUsability> {};
        },
        // FIXME: Don't duplicate this for HTMLImageElement and SVGImageElement.
        [](GC::Root<SVG::SVGImageElement> const& image_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // FIXME: If image's current request's state is broken, then throw an "InvalidStateError" DOMException.

            // If image is not fully decodable, then return bad.
            if (!image_element->current_image_bitmap())
                return { CanvasImageSourceUsability::Bad };

            // If image has an intrinsic width or intrinsic height (or both) equal to zero, then return bad.
            if (image_element->current_image_bitmap()->width() == 0 || image_element->current_image_bitmap()->height() == 0)
                return { CanvasImageSourceUsability::Bad };
            return Optional<CanvasImageSourceUsability> {};
        },

        [](GC::Root<HTML::HTMLVideoElement> const& video_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image's readyState attribute is either HAVE_NOTHING or HAVE_METADATA, then return bad.
            if (video_element->ready_state() == HTML::HTMLMediaElement::ReadyState::HaveNothing || video_element->ready_state() == HTML::HTMLMediaElement::ReadyState::HaveMetadata) {
                return { CanvasImageSourceUsability::Bad };
            }
            return Optional<CanvasImageSourceUsability> {};
        },

        // HTMLCanvasElement
        // FIXME: OffscreenCanvas
        [](GC::Root<HTMLCanvasElement> const& canvas_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image has either a horizontal dimension or a vertical dimension equal to zero, then throw an "InvalidStateError" DOMException.
            if (canvas_element->width() == 0 || canvas_element->height() == 0)
                return WebIDL::InvalidStateError::create(canvas_element->realm(), "Canvas width or height is zero"_string);
            return Optional<CanvasImageSourceUsability> {};
        },

        // ImageBitmap
        // FIXME: VideoFrame
        [](GC::Root<ImageBitmap> const& image_bitmap) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            if (image_bitmap->is_detached())
                return WebIDL::InvalidStateError::create(image_bitmap->realm(), "Image bitmap is detached"_string);
            return Optional<CanvasImageSourceUsability> {};
        }));
    if (usability.has_value())
        return usability.release_value();

    // 2. Return good.
    return { CanvasImageSourceUsability::Good };
}

// https://html.spec.whatwg.org/multipage/canvas.html#the-image-argument-is-not-origin-clean
bool image_is_not_origin_clean(CanvasImageSource const& image)
{
    // An object image is not origin-clean if, switching on image's type:
    return image.visit(
        // HTMLOrSVGImageElement
        [](GC::Root<HTMLImageElement> const&) {
            // FIXME: image's current request's image data is CORS-cross-origin.
            return false;
        },
        [](GC::Root<SVG::SVGImageElement> const&) {
            // FIXME: image's current request's image data is CORS-cross-origin.
            return false;
        },
        [](GC::Root<HTML::HTMLVideoElement> const&) {
            // FIXME: image's media data is CORS-cross-origin.
            return false;
        },
        // HTMLCanvasElement
        [](OneOf<GC::Root<HTMLCanvasElement>, GC::Root<ImageBitmap>> auto const&) {
            // FIXME: image's bitmap's origin-clean flag is false.
            return false;
        });
}

bool CanvasRenderingContext2D::image_smoothing_enabled() const
{
    return drawing_state().image_smoothing_enabled;
}

void CanvasRenderingContext2D::set_image_smoothing_enabled(bool enabled)
{
    drawing_state().image_smoothing_enabled = enabled;
}

Bindings::ImageSmoothingQuality CanvasRenderingContext2D::image_smoothing_quality() const
{
    return drawing_state().image_smoothing_quality;
}

void CanvasRenderingContext2D::set_image_smoothing_quality(Bindings::ImageSmoothingQuality quality)
{
    drawing_state().image_smoothing_quality = quality;
}

float CanvasRenderingContext2D::global_alpha() const
{
    return drawing_state().global_alpha;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalalpha
void CanvasRenderingContext2D::set_global_alpha(float alpha)
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

String CanvasRenderingContext2D::global_composite_operation() const
{
    auto current_compositing_and_blending_operator = drawing_state().current_compositing_and_blending_operator;
    switch (current_compositing_and_blending_operator) {
#undef __ENUMERATE
#define __ENUMERATE(operation, compositing_and_blending_operator)                \
    case Gfx::CompositingAndBlendingOperator::compositing_and_blending_operator: \
        return #operation##_string;
        ENUMERATE_COMPOSITE_OPERATIONS(__ENUMERATE)
#undef __ENUMERATE
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalcompositeoperation
void CanvasRenderingContext2D::set_global_composite_operation(String global_composite_operation)
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

float CanvasRenderingContext2D::shadow_offset_x() const
{
    return drawing_state().shadow_offset_x;
}

void CanvasRenderingContext2D::set_shadow_offset_x(float offsetX)
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsetx
    drawing_state().shadow_offset_x = offsetX;
}

float CanvasRenderingContext2D::shadow_offset_y() const
{
    return drawing_state().shadow_offset_y;
}

void CanvasRenderingContext2D::set_shadow_offset_y(float offsetY)
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsety
    drawing_state().shadow_offset_y = offsetY;
}

float CanvasRenderingContext2D::shadow_blur() const
{
    return drawing_state().shadow_blur;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowblur
void CanvasRenderingContext2D::set_shadow_blur(float blur_radius)
{
    // On setting, the attribute must be set to the new value,
    // except if the value is negative, infinite or NaN, in which case the new value must be ignored.
    if (blur_radius < 0 || isinf(blur_radius) || isnan(blur_radius))
        return;

    drawing_state().shadow_blur = blur_radius;
}

String CanvasRenderingContext2D::shadow_color() const
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowcolor
    return drawing_state().shadow_color.to_string(Gfx::Color::HTMLCompatibleSerialization::Yes);
}

void CanvasRenderingContext2D::set_shadow_color(String color)
{
    // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.

    // 2. Let parsedValue be the result of parsing the given value with context if non-null.
    auto style_value = parse_css_value(CSS::Parser::ParsingParams(), color, CSS::PropertyID::Color);
    if (style_value && style_value->has_color()) {
        auto parsedValue = style_value->to_color(OptionalNone());

        // 4. Set this's shadow color to parsedValue.
        drawing_state().shadow_color = parsedValue;
    } else {
        // 3. If parsedValue is failure, then return.
        return;
    }
}
void CanvasRenderingContext2D::paint_shadow_for_fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto path_to_fill = path;
    path_to_fill.close_all_subpaths();

    auto& state = this->drawing_state();

    if (state.current_compositing_and_blending_operator == Gfx::CompositingAndBlendingOperator::Copy)
        return;

    painter->save();

    Gfx::AffineTransform transform;
    transform.translate(state.shadow_offset_x, state.shadow_offset_y);
    painter->set_transform(transform);
    painter->fill_path(path_to_fill, state.shadow_color.with_opacity(state.global_alpha), winding_rule, state.shadow_blur, state.current_compositing_and_blending_operator);

    painter->restore();

    did_draw(path_to_fill.bounding_box());
}

void CanvasRenderingContext2D::paint_shadow_for_stroke_internal(Gfx::Path const& path)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = drawing_state();

    if (state.current_compositing_and_blending_operator == Gfx::CompositingAndBlendingOperator::Copy)
        return;

    painter->save();

    Gfx::AffineTransform transform;
    transform.translate(state.shadow_offset_x, state.shadow_offset_y);
    painter->set_transform(transform);
    painter->stroke_path(path, state.shadow_color.with_opacity(state.global_alpha), state.line_width, state.shadow_blur, state.current_compositing_and_blending_operator);

    painter->restore();

    did_draw(path.bounding_box());
}

String CanvasRenderingContext2D::filter() const
{
    if (!drawing_state().filters_string.has_value()) {
        return String::from_utf8_without_validation("none"sv.bytes());
    }

    return drawing_state().filters_string.value();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-filter
void CanvasRenderingContext2D::set_filter(String filter)
{
    drawing_state().filters.clear();

    // 1. If the given value is "none", then set this's current filter to "none" and return.
    if (filter == "none"sv) {
        drawing_state().filters_string.clear();
        return;
    }

    auto& realm = static_cast<CanvasRenderingContext2D&>(*this).realm();
    auto parser = CSS::Parser::Parser::create(CSS::Parser::ParsingParams(realm), filter);

    // 2. Let parsedValue be the result of parsing the given values as a <filter-value-list>.
    //    If any property-independent style sheet syntax like 'inherit' or 'initial' is present,
    //    then this parsing must return failure.
    auto style_value = parser.parse_as_css_value(CSS::PropertyID::Filter);

    if (style_value && style_value->is_filter_value_list()) {
        auto filter_value_list = style_value->as_filter_value_list().filter_value_list();

        drawing_state().filters.grow_capacity(filter_value_list.size());

        // Note: The layout must be updated to make sure the canvas's layout node isn't null.
        canvas_element().document().update_layout();
        auto layout_node = canvas_element().layout_node();

        // 4. Set this's current filter to the given value.
        for (auto& item : filter_value_list) {
            // FIXME: Add support for SVG filters when they get implement by the CSS parser.
            item.visit(
                [&](CSS::FilterOperation::Blur const& blur_filter) {
                    float radius = blur_filter.resolved_radius(*layout_node);
                    drawing_state().filters.append(Gfx::BlurFilter { radius });
                },
                [&](CSS::FilterOperation::Color const& color) {
                    float amount = color.resolved_amount();
                    drawing_state().filters.append(Gfx::ColorFilter { color.operation, amount });
                },
                [&](CSS::FilterOperation::HueRotate const& hue_rotate) {
                    float angle = hue_rotate.angle_degrees(*layout_node);
                    drawing_state().filters.append(Gfx::HueRotateFilter { angle });
                },
                [&](CSS::FilterOperation::DropShadow const& drop_shadow) {
                    auto resolution_context = CSS::Length::ResolutionContext::for_layout_node(*layout_node);
                    CSS::CalculationResolutionContext calculation_context {
                        .length_resolution_context = resolution_context,
                    };
                    auto zero_px = CSS::Length::make_px(0);

                    float offset_x = static_cast<float>(drop_shadow.offset_x.resolved(calculation_context).value_or(zero_px).to_px(resolution_context));
                    float offset_y = static_cast<float>(drop_shadow.offset_y.resolved(calculation_context).value_or(zero_px).to_px(resolution_context));

                    float radius = 0.0f;
                    if (drop_shadow.radius.has_value()) {
                        radius = static_cast<float>(drop_shadow.radius->resolved(calculation_context).value_or(zero_px).to_px(resolution_context));
                    };

                    auto color = drop_shadow.color.value_or(Gfx::Color { 0, 0, 0, 255 });

                    drawing_state().filters.append(Gfx::DropShadowFilter { offset_x, offset_y, radius, color });
                });
        }

        drawing_state().filters_string = move(filter);
    }

    // 3. If parsedValue is failure, then return.
}
}
