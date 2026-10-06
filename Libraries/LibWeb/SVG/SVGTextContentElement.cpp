/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
// Layout commits one cell per code unit of each rendered text content element's own character data, after processing
// its white space. An element that isn't rendered has no committed box, and so no cells at all.
// AD-HOC: Layout shapes an element's own character data as one run ahead of its text content children, so the cells
//         come in that order rather than in document order when text and child elements interleave.
Vector<Layout::RustFFI::FfiSvgTextCharacterCell> SVGTextContentElement::character_cells()
{
    Layout::ForcedReadScope read { document() };
    document().update_layout_if_needed_for_node(*this, DOM::UpdateLayoutReason::SVGTextContentElementTextQuery);
    Vector<Layout::RustFFI::FfiSvgTextCharacterCell> cells;
    auto const* layout_node = this->layout_node(read);
    if (!layout_node)
        return cells;
    auto collect = [&cells](auto& self, Layout::Node const& node) -> void {
        if (!Painting::has_committed_box(node))
            return;
        cells.extend(Painting::svg_text_character_cells(node));
        for (auto const* child = node.first_child_ptr(); child; child = child->next_sibling_ptr()) {
            switch (child->kind()) {
            case Layout::RustFFI::NodeKind::SVGTextBox:
            case Layout::RustFFI::NodeKind::SVGTextPathBox:
                self(self, *child);
                break;
            default:
                break;
            }
        }
    };
    collect(collect, *layout_node);
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
        // NB: A cell's width is its typographic character's advance.
        // FIXME: Layout applies neither letter-spacing nor word-spacing to SVG text, so there's no such space to add.
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

GC::Ref<Geometry::DOMPoint> SVGTextContentElement::get_start_position_of_char(WebIDL::UnsignedLong charnum)
{
    dbgln("(STUBBED) SVGTextContentElement::get_start_position_of_char(charnum={}). Called on: {}", charnum, debug_description());
    return Geometry::DOMPoint::create();
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

}
