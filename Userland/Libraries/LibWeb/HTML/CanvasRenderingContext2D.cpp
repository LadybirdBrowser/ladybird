/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibGfx/DeprecatedPainter.h>
#include <LibGfx/Quad.h>
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
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(CanvasRenderingContext2D);

JS::NonnullGCPtr<CanvasRenderingContext2D> CanvasRenderingContext2D::create(JS::Realm& realm, HTMLCanvasElement& element)
{
    return realm.heap().allocate<CanvasRenderingContext2D>(realm, realm, element);
}

CanvasRenderingContext2D::CanvasRenderingContext2D(JS::Realm& realm, HTMLCanvasElement& element)
    : PlatformObject(realm)
    , CanvasPath(static_cast<Bindings::PlatformObject&>(*this), *this)
    , m_element(element)
{
}

CanvasRenderingContext2D::~CanvasRenderingContext2D() = default;

void CanvasRenderingContext2D::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    set_prototype(&Bindings::ensure_web_prototype<Bindings::CanvasRenderingContext2DPrototype>(realm, "CanvasRenderingContext2D"_fly_string));
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

JS::NonnullGCPtr<HTMLCanvasElement> CanvasRenderingContext2D::canvas_for_binding() const
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

    auto const* bitmap = image.visit([](auto const& source) -> Gfx::Bitmap const* { return source->bitmap(); });
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
        painter->draw_bitmap(destination_rect, *bitmap, source_rect.to_rounded<int>(), scaling_mode, drawing_state().global_alpha);
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
    if (!canvas_element().bitmap()) {
        if (!canvas_element().create_bitmap())
            return nullptr;
        canvas_element().document().invalidate_display_list();
        m_painter = Gfx::Painter::create(*canvas_element().bitmap());
    }
    return m_painter.ptr();
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

    auto& state = drawing_state();

    if (auto color = state.stroke_style.as_color(); color.has_value()) {
        painter->stroke_path(path, color->with_opacity(state.global_alpha), state.line_width);
    } else {
        painter->stroke_path(path, state.stroke_style.to_gfx_paint_style(), state.line_width, state.global_alpha);
    }

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

    auto path_to_fill = path;
    path_to_fill.close_all_subpaths();
    auto& state = this->drawing_state();
    if (auto color = state.fill_style.as_color(); color.has_value()) {
        painter->fill_path(path_to_fill, color->with_opacity(state.global_alpha), winding_rule);
    } else {
        painter->fill_path(path_to_fill, state.fill_style.to_gfx_paint_style(), state.global_alpha, winding_rule);
    }

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

WebIDL::ExceptionOr<JS::NonnullGCPtr<ImageData>> CanvasRenderingContext2D::create_image_data(int width, int height, Optional<ImageDataSettings> const& settings) const
{
    return ImageData::create(realm(), width, height, settings);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-getimagedata
WebIDL::ExceptionOr<JS::GCPtr<ImageData>> CanvasRenderingContext2D::get_image_data(int x, int y, int width, int height, Optional<ImageDataSettings> const& settings) const
{
    // 1. If either the sw or sh arguments are zero, then throw an "IndexSizeError" DOMException.
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_fly_string);

    // 2. If the CanvasRenderingContext2D's origin-clean flag is set to false, then throw a "SecurityError" DOMException.
    if (!m_origin_clean)
        return WebIDL::SecurityError::create(realm(), "CanvasRenderingContext2D is not origin-clean"_fly_string);

    // 3. Let imageData be a new ImageData object.
    // 4. Initialize imageData given sw, sh, settings set to settings, and defaultColorSpace set to this's color space.
    auto image_data = TRY(ImageData::create(realm(), width, height, settings));

    // NOTE: We don't attempt to create the underlying bitmap here; if it doesn't exist, it's like copying only transparent black pixels (which is a no-op).
    if (!canvas_element().bitmap())
        return image_data;
    auto const& bitmap = *canvas_element().bitmap();

    // 5. Let the source rectangle be the rectangle whose corners are the four points (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh).
    auto source_rect = Gfx::Rect { x, y, width, height };
    auto source_rect_intersected = source_rect.intersected(bitmap.rect());

    // 6. Set the pixel values of imageData to be the pixels of this's output bitmap in the area specified by the source rectangle in the bitmap's coordinate space units, converted from this's color space to imageData's colorSpace using 'relative-colorimetric' rendering intent.
    // NOTE: Internally we must use premultiplied alpha, but ImageData should hold unpremultiplied alpha. This conversion
    //       might result in a loss of precision, but is according to spec.
    //       See: https://html.spec.whatwg.org/multipage/canvas.html#premultiplied-alpha-and-the-2d-rendering-context
    ASSERT(bitmap.alpha_type() == Gfx::AlphaType::Premultiplied);
    ASSERT(image_data->bitmap().alpha_type() == Gfx::AlphaType::Unpremultiplied);

    auto painter = Gfx::Painter::create(image_data->bitmap());
    painter->draw_bitmap(image_data->bitmap().rect().to_type<float>(), bitmap, source_rect_intersected, Gfx::ScalingMode::NearestNeighbor, drawing_state().global_alpha);

    // 7. Set the pixels values of imageData for areas of the source rectangle that are outside of the output bitmap to transparent black.
    // NOTE: No-op, already done during creation.

    // 8. Return imageData.
    return image_data;
}

void CanvasRenderingContext2D::put_image_data(ImageData const& image_data, float x, float y)
{
    if (auto* painter = this->painter()) {
        auto dst_rect = Gfx::FloatRect(x, y, image_data.width(), image_data.height());
        painter->draw_bitmap(dst_rect, image_data.bitmap(), image_data.bitmap().rect(), Gfx::ScalingMode::NearestNeighbor, 1.0f);
        did_draw(dst_rect);
    }
}

// https://html.spec.whatwg.org/multipage/canvas.html#reset-the-rendering-context-to-its-default-state
void CanvasRenderingContext2D::reset_to_default_state()
{
    auto* bitmap = canvas_element().bitmap();

    // 1. Clear canvas's bitmap to transparent black.
    if (bitmap) {
        painter()->clear_rect(bitmap->rect().to_type<float>(), Color::Transparent);
    }

    // 2. Empty the list of subpaths in context's current default path.
    path().clear();

    // 3. Clear the context's drawing state stack.
    clear_drawing_state_stack();

    // 4. Reset everything that drawing state consists of to their initial values.
    reset_drawing_state();

    if (bitmap)
        did_draw(bitmap->rect().to_type<float>());
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-measuretext
JS::NonnullGCPtr<TextMetrics> CanvasRenderingContext2D::measure_text(StringView text)
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

    auto glyph_run = Gfx::shape_text(anchor, replaced_text.code_points(), *font, Gfx::GlyphRun::TextType::Ltr);

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
        [](JS::Handle<HTMLImageElement> const& image_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // FIXME: If image's current request's state is broken, then throw an "InvalidStateError" DOMException.

            // If image is not fully decodable, then return bad.
            if (!image_element->bitmap())
                return { CanvasImageSourceUsability::Bad };

            // If image has an intrinsic width or intrinsic height (or both) equal to zero, then return bad.
            if (image_element->bitmap()->width() == 0 || image_element->bitmap()->height() == 0)
                return { CanvasImageSourceUsability::Bad };
            return Optional<CanvasImageSourceUsability> {};
        },
        // FIXME: Don't duplicate this for HTMLImageElement and SVGImageElement.
        [](JS::Handle<SVG::SVGImageElement> const& image_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // FIXME: If image's current request's state is broken, then throw an "InvalidStateError" DOMException.

            // If image is not fully decodable, then return bad.
            if (!image_element->bitmap())
                return { CanvasImageSourceUsability::Bad };

            // If image has an intrinsic width or intrinsic height (or both) equal to zero, then return bad.
            if (image_element->bitmap()->width() == 0 || image_element->bitmap()->height() == 0)
                return { CanvasImageSourceUsability::Bad };
            return Optional<CanvasImageSourceUsability> {};
        },

        [](JS::Handle<HTML::HTMLVideoElement> const& video_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image's readyState attribute is either HAVE_NOTHING or HAVE_METADATA, then return bad.
            if (video_element->ready_state() == HTML::HTMLMediaElement::ReadyState::HaveNothing || video_element->ready_state() == HTML::HTMLMediaElement::ReadyState::HaveMetadata) {
                return { CanvasImageSourceUsability::Bad };
            }
            return Optional<CanvasImageSourceUsability> {};
        },

        // HTMLCanvasElement
        // FIXME: OffscreenCanvas
        [](JS::Handle<HTMLCanvasElement> const& canvas_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image has either a horizontal dimension or a vertical dimension equal to zero, then throw an "InvalidStateError" DOMException.
            if (canvas_element->width() == 0 || canvas_element->height() == 0)
                return WebIDL::InvalidStateError::create(canvas_element->realm(), "Canvas width or height is zero"_fly_string);
            return Optional<CanvasImageSourceUsability> {};
        },

        // ImageBitmap
        // FIXME: VideoFrame
        [](JS::Handle<ImageBitmap> const& image_bitmap) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            if (image_bitmap->is_detached())
                return WebIDL::InvalidStateError::create(image_bitmap->realm(), "Image bitmap is detached"_fly_string);
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
        [](JS::Handle<HTMLImageElement> const&) {
            // FIXME: image's current request's image data is CORS-cross-origin.
            return false;
        },
        [](JS::Handle<SVG::SVGImageElement> const&) {
            // FIXME: image's current request's image data is CORS-cross-origin.
            return false;
        },
        [](JS::Handle<HTML::HTMLVideoElement> const&) {
            // FIXME: image's media data is CORS-cross-origin.
            return false;
        },
        // HTMLCanvasElement
        [](OneOf<JS::Handle<HTMLCanvasElement>, JS::Handle<ImageBitmap>> auto const&) {
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

}
