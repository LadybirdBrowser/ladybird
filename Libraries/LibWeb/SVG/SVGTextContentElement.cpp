/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMPointReadOnly.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGTextContentElement.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::SVG {

SVGTextContentElement::SVGTextContentElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

// The glyph cell of every addressable character within this element, indexed as the SVG DOM text methods index them.
// https://svgwg.org/svg2-draft/text.html#TermAddressableCharacter
// A character that is addressable by text positioning attributes and SVG DOM text methods.
// Characters discarded during layout such as collapsed white space characters are not addressable, neither are
// characters within an element with a value of none for the display property.
// Layout commits one cell per code unit of the character data in each rendered text content element's subtree, in
// document order, after processing its white space. An element that isn't rendered has no committed box, and so no
// cells at all.
Vector<Layout::RustFFI::FfiSvgTextCharacterCell> SVGTextContentElement::character_cells()
{
    Layout::ForcedReadScope read { document() };
    document().update_layout_if_needed_for_node(*this, DOM::UpdateLayoutReason::SVGTextContentElementTextQuery);
    Vector<Layout::RustFFI::FfiSvgTextCharacterCell> cells;
    auto const* layout_node = this->layout_node(read);
    if (!layout_node)
        return cells;
    if (Painting::has_committed_box(*layout_node))
        cells = Painting::svg_text_character_cells(*layout_node);
    return cells;
}

// Steps 3 to 5 of getSubStringLength(), for the characters whose index lies in [charnum, charnum + nchars).
// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getSubStringLength
static float sub_string_length(ReadonlySpan<Layout::RustFFI::FfiSvgTextCharacterCell> cells, size_t charnum, size_t nchars)
{
    // 3. Let length be a length in user units, initialized to 0.
    float length = 0;

    // 4. For each addressable character in the DOM within this element that has an index such that
    //    charnum ≤ index < (charnum + nchars):
    // NB: Both callers hand over a charnum no greater than the number of cells, so the subtraction can't wrap, and
    //     clamping nchars first keeps the sum from overflowing on a 32-bit size_t.
    auto end = charnum + min(nchars, cells.size() - charnum);
    for (size_t index = charnum; index < end; ++index) {
        //  1. If the character corresponds to a typographic character and it is the first character in document order
        //     to correspond to that typographic character, then:
        if (!cells[index].starts_typographic_character)
            continue;
        //    1. Add the advance of the typographic character to length, adjusted for any font kerning in effect.
        //    2. If the letter-spacing or word-spacing properties contributed space just after the typographic
        //       character, then add that space to length.
        // NB: A cell's width is its typographic character's advance, plus any letter-spacing and word-spacing layout
        //     put just after it.
        length += cells[index].width;
    }

    // 5. Return length.
    return length;
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getNumberOfChars
WebIDL::ExceptionOr<WebIDL::Long> SVGTextContentElement::get_number_of_chars()
{
    // The getNumberOfChars method returns the total number of addressable characters available for rendering within
    // the current element, regardless of whether they will be rendered. When getNumberOfChars() is called, the
    // following steps are run:
    // 1. Let node be the element or node upon which this method was called
    // 2. If node is a DOM text node, return the length of the text content of node, after normalizing whitespace
    //    according to the value of the white-space property on its parent element.
    // 3. If node is an Element:
    //    - If the element is not rendered (e.g., because the display property has the used value none), then return 0;
    //    - Otherwise, set count to 0, and for each child of node:
    //      - Recursively call this algorithm and add the returned value to count.
    //      Return count.
    // 4. For all other node types (e.g., DOM comments), return 0.
    // The cells layout committed are exactly those characters, one cell per code unit of normalized text.
    return static_cast<WebIDL::Long>(character_cells().size());
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getComputedTextLength
WebIDL::ExceptionOr<float> SVGTextContentElement::get_computed_text_length()
{
    // The getComputedTextLength method is used to compute a "length" for the text within the element. When
    // getComputedTextLength() is called, the following steps are run:
    auto cells = character_cells();

    // 1. Let count be the value that would be returned if the getNumberOfChars method were called on this element.
    auto count = cells.size();

    // 2. Let length be the value that would be returned if the getSubStringLength method were called on this element,
    //    passing 0 and count as arguments.
    // AD-HOC: getSubStringLength(0, 0) throws an IndexSizeError, since an element without characters assigns no index
    //         at all. An element without text has a length of 0 instead, so the characters are summed directly.
    auto length = sub_string_length(cells, 0, count);

    // 3. Return length.
    return length;
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getSubStringLength
WebIDL::ExceptionOr<float> SVGTextContentElement::get_sub_string_length(WebIDL::UnsignedLong charnum, WebIDL::UnsignedLong nchars)
{
    // The getSubStringLength method is used to compute the formatted text advance distance for a substring of text
    // within the element. When getSubStringLength(charnum, nchars) is called, the following steps are run:

    // 1. Assign an index to each addressable character in the DOM within this element, where the first character has
    //    index 0.
    auto cells = character_cells();

    // 2. If charnum is greater than the highest index assigned to a character or if nchars is negative, then throw an
    //    IndexSizeError.
    // NB: nchars is an unsigned long, so it's never negative.
    if (charnum >= cells.size())
        return WebIDL::IndexSizeError::create("Character index out of range"_utf16);

    // 3. to 5.
    return sub_string_length(cells, charnum, nchars);
}

WebIDL::ExceptionOr<GC::Ref<Geometry::DOMPoint>> SVGTextContentElement::get_start_position_of_char(WebIDL::UnsignedLong charnum)
{
    // The getStartPositionOfChar method is used to get the position of a typographic character after text layout has
    // been performed. When getStartPositionOfChar(charnum) is called, the following steps are run:
    auto cells = character_cells();

    // 1. Let cluster be the result of finding the typographic character for the character at index charnum within the
    //    current element.
    // 2. If cluster is null, then throw an IndexSizeError.
    // NB: Every addressable character here corresponds to the typographic character whose cell it carries, so only an
    //     index past the last character finds nothing.
    if (charnum >= cells.size())
        return WebIDL::IndexSizeError::create("Character index out of range"_utf16);
    auto const& cluster = cells[charnum];

    // 3. Let p be the alignment point of the typographic character that correspond to the character at index charnum,
    //    in the coordinate system of the current element.
    // FIXME: The glyph cells of text on a path are measured before the glyphs are laid along the path, so the point
    //        isn't on the path.
    // 4. Return a newly created, detached SVGPoint object representing the point p.
    return Geometry::DOMPoint::create(cluster.x, cluster.alignment_point_y);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getEndPositionOfChar
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMPoint>> SVGTextContentElement::get_end_position_of_char(WebIDL::UnsignedLong charnum)
{
    // The getEndPositionOfChar method is used to get the trailing position of a typographic character after text
    // layout has been performed. When getEndPositionOfChar(charnum) is called, the following steps are run:
    auto cells = character_cells();

    // 1. Let cluster be the result of finding the typographic character for the character at index charnum within the
    //    current element.
    // 2. If cluster is null, then throw an IndexSizeError.
    if (charnum >= cells.size())
        return WebIDL::IndexSizeError::create("Character index out of range"_utf16);
    auto const& cluster = cells[charnum];

    // 3. Let p be the alignment point of cluster that correspond to the character at index charnum, in the coordinate
    //    system of the current element.
    // 4. Let direction be a unit vector in the direction of the cluster's advance. [...]
    // FIXME: Every advance is taken to run along the x axis, which holds for the left-to-right horizontal text that
    //        layout shapes, off a path.
    // 5. Let advance be cluster's advance.
    // 6. Set p to p + advance · direction.
    // 7. Return a newly created, detached SVGPoint object representing the point p.
    return Geometry::DOMPoint::create(cluster.x + cluster.width, cluster.alignment_point_y);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getExtentOfChar
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMRect>> SVGTextContentElement::get_extent_of_char(WebIDL::UnsignedLong charnum)
{
    // The getExtentOfChar method is used to compute a tight bounding box of the glyph cell that corresponds to a given
    // typographic character. When getExtentOfChar(charnum) is called, the following steps are run:
    auto cells = character_cells();

    // 1. Let cluster be the result of finding the typographic character for the character at index charnum within the
    //    current element.
    // 2. If cluster is null, then throw an IndexSizeError.
    // NB: Every addressable character here corresponds to the typographic character whose cell it carries, so finding
    //     it never has to move past charnum, and only an index past the last character finds nothing.
    if (charnum >= cells.size())
        return WebIDL::IndexSizeError::create("Character index out of range"_utf16);
    auto const& cluster = cells[charnum];

    // 3. Let quad be the potentially rotated rectangle in the current element's coordinate system that is the glyph
    //    cell for cluster.
    // 4. Let rect be the rectangle that forms the tightest bounding box around quad in the current element's
    //    coordinate system.
    // FIXME: The glyph cells of text on a path are measured before the glyphs are laid along the path, so they are
    //        neither moved onto it nor rotated along it.
    Gfx::FloatRect rect { cluster.x, cluster.y, cluster.width, cluster.height };

    // 5. Return a newly created DOMRect object representing the rectangle rect.
    return Geometry::DOMRect::create(rect);
}

void SVGTextContentElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // The attributes adjust the advances of the element's characters, so the text has to be laid out with their new
    // values.
    if (old_value != value && name.is_one_of(AttributeNames::textLength, AttributeNames::lengthAdjust))
        set_needs_layout_update(DOM::SetNeedsLayoutReason::StyleChange);
}

SVGTextContentElement::ParsedTextLength SVGTextContentElement::parsed_text_length() const
{
    CSS::Parser::ParsingParams const parsing_params { document() };
    ParsedTextLength parsed;

    // https://svgwg.org/svg2-draft/text.html#TextElementTextLengthAttribute
    // A negative value is an error (see Error processing).
    auto raw_text_length = get_attribute_value(AttributeNames::textLength);
    if (auto style_value = parse_css_type(parsing_params, raw_text_length, CSS::ValueType::LengthPercentage)) {
        if (auto value = SVGLengthValue::from_style_value(*style_value); value.has_value() && value->kind() != SVGLengthValue::Kind::Number && value->value() >= 0)
            parsed.text_length = value;
    }
    if (!parsed.text_length.has_value()) {
        if (auto style_value = parse_css_type(parsing_params, raw_text_length, CSS::ValueType::Number)) {
            if (auto value = SVGLengthValue::from_style_value(*style_value); value.has_value() && value->kind() == SVGLengthValue::Kind::Number && value->value() >= 0)
                parsed.text_length = value;
        }
    }

    // https://svgwg.org/svg2-draft/text.html#TextElementLengthAdjustAttribute
    // lengthAdjust = "spacing | spacingAndGlyphs"
    parsed.adjusts_glyphs = get_attribute_value(AttributeNames::lengthAdjust) == "spacingAndGlyphs"sv;
    return parsed;
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__lengthAdjust
GC::Ref<SVGAnimatedEnumeration> SVGTextContentElement::length_adjust()
{
    static constexpr Array keywords {
        SVGAnimatedEnumeration::Keyword { "spacing"sv, LENGTHADJUST_SPACING },
        SVGAnimatedEnumeration::Keyword { "spacingAndGlyphs"sv, LENGTHADJUST_SPACINGANDGLYPHS },
    };
    return svg_animated_enumeration_for_attribute(AttributeNames::lengthAdjust, keywords, LENGTHADJUST_SPACING);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getCharNumAtPosition
WebIDL::Long SVGTextContentElement::get_char_num_at_position(Bindings::DOMPointInit const& point)
{
    // The getCharNumAtPosition method is used to find which character caused a text glyph to be rendered at a given
    // position in the coordinate system. Because the relationship between characters and glyphs is not one-to-one, only
    // the first character of the relevant typographic character is returned. When getCharNumAtPosition(point) is
    // called, the following steps are run:
    // 1. Assign an index to each character in the DOM within this element, where the first character has index 0.
    // NB: Each addressable character has a cell, in order.
    auto cells = character_cells();

    // 2. Let last be the highest index assigned to a character.
    // 3. Let charnum be 0.
    // 4. Let result be -1.
    WebIDL::Long result = -1;

    // 5. While charnum < last:
    // NB: The steps never advance charnum, so this visits every character, including the one at index last.
    for (size_t charnum = 0; charnum < cells.size(); ++charnum) {
        // 1. If the character at index charnum corresponds to a typographic character and it is the first character in
        //    document order to correspond to that typographic character, and point in this element's coordinate system
        //    is within the glyph cell for the typographic character, then set result to charnum.
        auto const& cell = cells[charnum];
        if (!cell.starts_typographic_character)
            continue;
        Gfx::FloatRect glyph_cell { cell.x, cell.y, cell.width, cell.height };
        if (glyph_cell.contains(static_cast<float>(point.x), static_cast<float>(point.y)))
            result = static_cast<WebIDL::Long>(charnum);
    }

    // 6. Return result.
    return result;
}

}
