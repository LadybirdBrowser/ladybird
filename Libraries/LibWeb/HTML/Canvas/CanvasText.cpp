/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibGfx/TextLayout.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/HTML/Canvas/CanvasText.h>
#include <LibWeb/HTML/Canvas/DrawingState.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::HTML {

RefPtr<Gfx::FontCascadeList const> CanvasText::font_cascade_list()
{
    // When font style value is empty load default font
    if (!drawing_state().font_style_value) {
        this->set_font("10px sans-serif"sv);
    }

    // Get current loaded font
    return drawing_state().current_font_cascade_list;
}

[[nodiscard]] Gfx::Path CanvasText::text_path(Utf16String const& text, float x, float y, Optional<double> max_width)
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

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-filltext
void CanvasText::fill_text(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (!isfinite(x) || !isfinite(y) || (max_width.has_value() && !isfinite(max_width.value())))
        return;

    fill_internal(text_path(text, x, y, max_width), Gfx::WindingRule::Nonzero);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-stroketext
void CanvasText::stroke_text(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (!isfinite(x) || !isfinite(y) || (max_width.has_value() && !isfinite(max_width.value())))
        return;

    stroke_internal(text_path(text, x, y, max_width));
}

// https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm
PreparedText CanvasText::prepare_text(Utf16String const& text, float max_width)
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

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-measuretext
GC::Ref<TextMetrics> CanvasText::measure_text(Utf16String const& text)
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
    auto const& font_pixel_metrics = font.pixel_metrics();
    auto const ascent = font_pixel_metrics.ascent;
    auto const descent = font_pixel_metrics.descent;
    auto const hanging_baseline = ascent * 0.8f;

    float baseline_offset = 0;
    switch (drawing_state().text_baseline) {
    case Bindings::CanvasTextBaseline::Top:
        baseline_offset = ascent;
        break;
    case Bindings::CanvasTextBaseline::Hanging:
        baseline_offset = hanging_baseline;
        break;
    case Bindings::CanvasTextBaseline::Middle:
        baseline_offset = (ascent - descent) / 2.0f;
        break;
    case Bindings::CanvasTextBaseline::Alphabetic:
        baseline_offset = 0;
        break;
    case Bindings::CanvasTextBaseline::Ideographic:
    case Bindings::CanvasTextBaseline::Bottom:
        baseline_offset = -descent;
        break;
    }

    // width attribute: The width of that inline box, in CSS pixels. (The text's advance width.)
    metrics->set_width(prepared_text.bounding_box.width());
    // actualBoundingBoxLeft attribute: The distance parallel to the baseline from the alignment point given by the textAlign attribute to the left side of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going left from the given alignment point.
    metrics->set_actual_bounding_box_left(-prepared_text.bounding_box.left());
    // actualBoundingBoxRight attribute: The distance parallel to the baseline from the alignment point given by the textAlign attribute to the right side of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going right from the given alignment point.
    metrics->set_actual_bounding_box_right(prepared_text.bounding_box.right());
    // fontBoundingBoxAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the ascent metric of the first available font, in CSS pixels; positive numbers indicating a distance going up from the given baseline.
    metrics->set_font_bounding_box_ascent(ascent - baseline_offset);
    // fontBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the descent metric of the first available font, in CSS pixels; positive numbers indicating a distance going down from the given baseline.
    metrics->set_font_bounding_box_descent(descent + baseline_offset);
    // actualBoundingBoxAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the top of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going up from the given baseline.
    metrics->set_actual_bounding_box_ascent(ascent - baseline_offset);
    // actualBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the bottom of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going down from the given baseline.
    metrics->set_actual_bounding_box_descent(descent + baseline_offset);
    // emHeightAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the highest top of the em squares in the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the top of that em square (so this value will usually be positive). Zero if the given baseline is the top of that em square; half the font size if the given baseline is the middle of that em square.
    metrics->set_em_height_ascent(ascent - baseline_offset);
    // emHeightDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the lowest bottom of the em squares in the inline box, in CSS pixels; positive numbers indicating that the given baseline is above the bottom of that em square. (Zero if the given baseline is the bottom of that em square.)
    metrics->set_em_height_descent(descent + baseline_offset);
    // hangingBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the hanging baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the hanging baseline. (Zero if the given baseline is the hanging baseline.)
    metrics->set_hanging_baseline(hanging_baseline - baseline_offset);
    // alphabeticBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the alphabetic baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the alphabetic baseline. (Zero if the given baseline is the alphabetic baseline.)
    metrics->set_alphabetic_baseline(-baseline_offset);
    // ideographicBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the ideographic-under baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the ideographic-under baseline. (Zero if the given baseline is the ideographic-under baseline.)
    metrics->set_ideographic_baseline(-descent - baseline_offset);

    return metrics;
}

}
