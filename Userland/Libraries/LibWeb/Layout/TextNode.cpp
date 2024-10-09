/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/StringBuilder.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/TextPaintable.h>

namespace Web::Layout {

JS_DEFINE_ALLOCATOR(TextNode);

TextNode::TextNode(DOM::Document& document, DOM::Text& text)
    : Node(document, &text)
{
}

TextNode::~TextNode() = default;

static bool is_all_whitespace(StringView string)
{
    for (size_t i = 0; i < string.length(); ++i) {
        if (!is_ascii_space(string[i]))
            return false;
    }
    return true;
}

// https://w3c.github.io/mathml-core/#new-text-transform-values
static String apply_math_auto_text_transform(String const& string)
{

    // https://w3c.github.io/mathml-core/#italic-mappings
    auto map_code_point_to_italic = [](u32 code_point) -> u32 {
        switch (code_point) {
        case 0x0041:
            return 0x1D434;
        case 0x0042:
            return 0x1D435;
        case 0x0043:
            return 0x1D436;
        case 0x0044:
            return 0x1D437;
        case 0x0045:
            return 0x1D438;
        case 0x0046:
            return 0x1D439;
        case 0x0047:
            return 0x1D43A;
        case 0x0048:
            return 0x1D43B;
        case 0x0049:
            return 0x1D43C;
        case 0x004A:
            return 0x1D43D;
        case 0x004B:
            return 0x1D43E;
        case 0x004C:
            return 0x1D43F;
        case 0x004D:
            return 0x1D440;
        case 0x004E:
            return 0x1D441;
        case 0x004F:
            return 0x1D442;
        case 0x0050:
            return 0x1D443;
        case 0x0051:
            return 0x1D444;
        case 0x0052:
            return 0x1D445;
        case 0x0053:
            return 0x1D446;
        case 0x0054:
            return 0x1D447;
        case 0x0055:
            return 0x1D448;
        case 0x0056:
            return 0x1D449;
        case 0x0057:
            return 0x1D44A;
        case 0x0058:
            return 0x1D44B;
        case 0x0059:
            return 0x1D44C;
        case 0x005A:
            return 0x1D44D;
        case 0x0061:
            return 0x1D44E;
        case 0x0062:
            return 0x1D44F;
        case 0x0063:
            return 0x1D450;
        case 0x0064:
            return 0x1D451;
        case 0x0065:
            return 0x1D452;
        case 0x0066:
            return 0x1D453;
        case 0x0067:
            return 0x1D454;
        case 0x0068:
            return 0x0210E;
        case 0x0069:
            return 0x1D456;
        case 0x006A:
            return 0x1D457;
        case 0x006B:
            return 0x1D458;
        case 0x006C:
            return 0x1D459;
        case 0x006D:
            return 0x1D45A;
        case 0x006E:
            return 0x1D45B;
        case 0x006F:
            return 0x1D45C;
        case 0x0070:
            return 0x1D45D;
        case 0x0071:
            return 0x1D45E;
        case 0x0072:
            return 0x1D45F;
        case 0x0073:
            return 0x1D460;
        case 0x0074:
            return 0x1D461;
        case 0x0075:
            return 0x1D462;
        case 0x0076:
            return 0x1D463;
        case 0x0077:
            return 0x1D464;
        case 0x0078:
            return 0x1D465;
        case 0x0079:
            return 0x1D466;
        case 0x007A:
            return 0x1D467;
        case 0x0131:
            return 0x1D6A4;
        case 0x0237:
            return 0x1D6A5;
        case 0x0391:
            return 0x1D6E2;
        case 0x0392:
            return 0x1D6E3;
        case 0x0393:
            return 0x1D6E4;
        case 0x0394:
            return 0x1D6E5;
        case 0x0395:
            return 0x1D6E6;
        case 0x0396:
            return 0x1D6E7;
        case 0x0397:
            return 0x1D6E8;
        case 0x0398:
            return 0x1D6E9;
        case 0x0399:
            return 0x1D6EA;
        case 0x039A:
            return 0x1D6EB;
        case 0x039B:
            return 0x1D6EC;
        case 0x039C:
            return 0x1D6ED;
        case 0x039D:
            return 0x1D6EE;
        case 0x039E:
            return 0x1D6EF;
        case 0x039F:
            return 0x1D6F0;
        case 0x03A0:
            return 0x1D6F1;
        case 0x03A1:
            return 0x1D6F2;
        case 0x03F4:
            return 0x1D6F3;
        case 0x03A3:
            return 0x1D6F4;
        case 0x03A4:
            return 0x1D6F5;
        case 0x03A5:
            return 0x1D6F6;
        case 0x03A6:
            return 0x1D6F7;
        case 0x03A7:
            return 0x1D6F8;
        case 0x03A8:
            return 0x1D6F9;
        case 0x03A9:
            return 0x1D6FA;
        case 0x2207:
            return 0x1D6FB;
        case 0x03B1:
            return 0x1D6FC;
        case 0x03B2:
            return 0x1D6FD;
        case 0x03B3:
            return 0x1D6FE;
        case 0x03B4:
            return 0x1D6FF;
        case 0x03B5:
            return 0x1D700;
        case 0x03B6:
            return 0x1D701;
        case 0x03B7:
            return 0x1D702;
        case 0x03B8:
            return 0x1D703;
        case 0x03B9:
            return 0x1D704;
        case 0x03BA:
            return 0x1D705;
        case 0x03BB:
            return 0x1D706;
        case 0x03BC:
            return 0x1D707;
        case 0x03BD:
            return 0x1D708;
        case 0x03BE:
            return 0x1D709;
        case 0x03BF:
            return 0x1D70A;
        case 0x03C0:
            return 0x1D70B;
        case 0x03C1:
            return 0x1D70C;
        case 0x03C2:
            return 0x1D70D;
        case 0x03C3:
            return 0x1D70E;
        case 0x03C4:
            return 0x1D70F;
        case 0x03C5:
            return 0x1D710;
        case 0x03C6:
            return 0x1D711;
        case 0x03C7:
            return 0x1D712;
        case 0x03C8:
            return 0x1D713;
        case 0x03C9:
            return 0x1D714;
        case 0x2202:
            return 0x1D715;
        case 0x03F5:
            return 0x1D716;
        case 0x03D1:
            return 0x1D717;
        case 0x03F0:
            return 0x1D718;
        case 0x03D5:
            return 0x1D719;
        case 0x03F1:
            return 0x1D71A;
        case 0x03D6:
            return 0x1D71B;
        default:
            return code_point;
        }
    };

    StringBuilder builder(string.bytes().size());

    for (auto code_point : string.code_points())
        builder.append_code_point(map_code_point_to_italic(code_point));

    return MUST(builder.to_string());
}

static ErrorOr<String> apply_text_transform(String const& string, CSS::TextTransform text_transform)
{
    switch (text_transform) {
    case CSS::TextTransform::Uppercase:
        return string.to_uppercase();
    case CSS::TextTransform::Lowercase:
        return string.to_lowercase();
    case CSS::TextTransform::None:
        return string;
    case CSS::TextTransform::MathAuto:
        return apply_math_auto_text_transform(string);
    case CSS::TextTransform::Capitalize: {
        return string.to_titlecase({}, TrailingCodePointTransformation::PreserveExisting);
    }
    case CSS::TextTransform::FullSizeKana:
    case CSS::TextTransform::FullWidth:
        // FIXME: Implement these!
        return string;
    }

    VERIFY_NOT_REACHED();
}

void TextNode::invalidate_text_for_rendering()
{
    m_text_for_rendering = {};
    m_grapheme_segmenter.clear();
}

String const& TextNode::text_for_rendering() const
{
    if (!m_text_for_rendering.has_value())
        const_cast<TextNode*>(this)->compute_text_for_rendering();
    return *m_text_for_rendering;
}

// NOTE: This collapses whitespace into a single ASCII space if the CSS white-space property tells us to.
void TextNode::compute_text_for_rendering()
{
    bool collapse = [](CSS::WhiteSpace white_space) {
        switch (white_space) {
        case CSS::WhiteSpace::Normal:
        case CSS::WhiteSpace::Nowrap:
        case CSS::WhiteSpace::PreLine:
            return true;
        case CSS::WhiteSpace::Pre:
        case CSS::WhiteSpace::PreWrap:
            return false;
        }
        VERIFY_NOT_REACHED();
    }(computed_values().white_space());

    if (dom_node().is_editable() && !dom_node().is_uninteresting_whitespace_node())
        collapse = false;

    auto data = apply_text_transform(dom_node().data(), computed_values().text_transform()).release_value_but_fixme_should_propagate_errors();

    auto data_view = data.bytes_as_string_view();

    if (dom_node().is_password_input()) {
        m_text_for_rendering = MUST(String::repeated('*', data_view.length()));
        return;
    }

    if (!collapse || data.is_empty()) {
        m_text_for_rendering = data;
        return;
    }

    // NOTE: A couple fast returns to avoid unnecessarily allocating a StringBuilder.
    if (data_view.length() == 1) {
        if (is_ascii_space(data_view[0])) {
            static String s_single_space_string = " "_string;
            m_text_for_rendering = s_single_space_string;
        } else {
            m_text_for_rendering = data;
        }
        return;
    }

    bool contains_space = false;
    for (auto c : data_view) {
        if (is_ascii_space(c)) {
            contains_space = true;
            break;
        }
    }
    if (!contains_space) {
        m_text_for_rendering = data;
        return;
    }

    StringBuilder builder(data_view.length());
    size_t index = 0;

    auto skip_over_whitespace = [&index, &data_view] {
        while (index < data_view.length() && is_ascii_space(data_view[index]))
            ++index;
    };

    while (index < data_view.length()) {
        if (is_ascii_space(data_view[index])) {
            builder.append(' ');
            ++index;
            skip_over_whitespace();
        } else {
            builder.append(data_view[index]);
            ++index;
        }
    }

    m_text_for_rendering = MUST(builder.to_string());
}

Unicode::Segmenter& TextNode::grapheme_segmenter() const
{
    if (!m_grapheme_segmenter) {
        m_grapheme_segmenter = document().grapheme_segmenter().clone();
        m_grapheme_segmenter->set_segmented_text(text_for_rendering());
    }

    return *m_grapheme_segmenter;
}

TextNode::ChunkIterator::ChunkIterator(TextNode const& text_node, bool wrap_lines, bool respect_linebreaks)
    : m_wrap_lines(wrap_lines)
    , m_respect_linebreaks(respect_linebreaks)
    , m_utf8_view(text_node.text_for_rendering())
    , m_font_cascade_list(text_node.computed_values().font_list())
    , m_grapheme_segmenter(text_node.grapheme_segmenter())
{
}

static Gfx::GlyphRun::TextType text_type_for_code_point(u32 code_point)
{
    switch (Unicode::bidirectional_class(code_point)) {
    case Unicode::BidiClass::WhiteSpaceNeutral:

    case Unicode::BidiClass::BlockSeparator:
    case Unicode::BidiClass::SegmentSeparator:
    case Unicode::BidiClass::CommonNumberSeparator:
    case Unicode::BidiClass::DirNonSpacingMark:

    case Unicode::BidiClass::ArabicNumber:
    case Unicode::BidiClass::EuropeanNumber:
    case Unicode::BidiClass::EuropeanNumberSeparator:
    case Unicode::BidiClass::EuropeanNumberTerminator:
        return Gfx::GlyphRun::TextType::ContextDependent;

    case Unicode::BidiClass::BoundaryNeutral:
    case Unicode::BidiClass::OtherNeutral:
    case Unicode::BidiClass::FirstStrongIsolate:
    case Unicode::BidiClass::PopDirectionalFormat:
    case Unicode::BidiClass::PopDirectionalIsolate:
        return Gfx::GlyphRun::TextType::Common;

    case Unicode::BidiClass::LeftToRight:
    case Unicode::BidiClass::LeftToRightEmbedding:
    case Unicode::BidiClass::LeftToRightIsolate:
    case Unicode::BidiClass::LeftToRightOverride:
        return Gfx::GlyphRun::TextType::Ltr;

    case Unicode::BidiClass::RightToLeft:
    case Unicode::BidiClass::RightToLeftArabic:
    case Unicode::BidiClass::RightToLeftEmbedding:
    case Unicode::BidiClass::RightToLeftIsolate:
    case Unicode::BidiClass::RightToLeftOverride:
        return Gfx::GlyphRun::TextType::Rtl;

    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<TextNode::Chunk> TextNode::ChunkIterator::next()
{
    if (!m_peek_queue.is_empty())
        return m_peek_queue.take_first();
    return next_without_peek();
}

Optional<TextNode::Chunk> TextNode::ChunkIterator::peek(size_t count)
{
    while (m_peek_queue.size() <= count) {
        auto next = next_without_peek();
        if (!next.has_value())
            return {};
        m_peek_queue.append(*next);
    }

    return m_peek_queue[count];
}

Optional<TextNode::Chunk> TextNode::ChunkIterator::next_without_peek()
{
    if (m_current_index >= m_utf8_view.byte_length())
        return {};

    auto current_code_point = [this]() {
        return *m_utf8_view.iterator_at_byte_offset_without_validation(m_current_index);
    };
    auto next_grapheme_boundary = [this]() {
        return m_grapheme_segmenter.next_boundary(m_current_index).value_or(m_utf8_view.byte_length());
    };

    auto code_point = current_code_point();
    auto start_of_chunk = m_current_index;

    Gfx::Font const& font = m_font_cascade_list.font_for_code_point(code_point);
    auto text_type = text_type_for_code_point(code_point);

    while (m_current_index < m_utf8_view.byte_length()) {
        code_point = current_code_point();

        if (&font != &m_font_cascade_list.font_for_code_point(code_point)) {
            if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, font, text_type); result.has_value())
                return result.release_value();
        }

        if (m_respect_linebreaks && code_point == '\n') {
            // Newline encountered, and we're supposed to preserve them.
            // If we have accumulated some code points in the current chunk, commit them now and continue with the newline next time.
            if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, font, text_type); result.has_value())
                return result.release_value();

            // Otherwise, commit the newline!
            m_current_index = next_grapheme_boundary();
            auto result = try_commit_chunk(start_of_chunk, m_current_index, true, font, text_type);
            VERIFY(result.has_value());
            return result.release_value();
        }

        if (m_wrap_lines) {
            if (text_type != text_type_for_code_point(code_point)) {
                if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, font, text_type); result.has_value()) {
                    return result.release_value();
                }
            }

            if (is_ascii_space(code_point)) {
                // Whitespace encountered, and we're allowed to break on whitespace.
                // If we have accumulated some code points in the current chunk, commit them now and continue with the whitespace next time.
                if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, font, text_type); result.has_value()) {
                    return result.release_value();
                }

                // Otherwise, commit the whitespace!
                m_current_index = next_grapheme_boundary();
                if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, font, text_type); result.has_value())
                    return result.release_value();
                continue;
            }
        }

        m_current_index = next_grapheme_boundary();
    }

    if (start_of_chunk != m_utf8_view.byte_length()) {
        // Try to output whatever's left at the end of the text node.
        if (auto result = try_commit_chunk(start_of_chunk, m_utf8_view.byte_length(), false, font, text_type); result.has_value())
            return result.release_value();
    }

    return {};
}

Optional<TextNode::Chunk> TextNode::ChunkIterator::try_commit_chunk(size_t start, size_t end, bool has_breaking_newline, Gfx::Font const& font, Gfx::GlyphRun::TextType text_type) const
{
    if (auto byte_length = end - start; byte_length > 0) {
        auto chunk_view = m_utf8_view.substring_view(start, byte_length);
        return Chunk {
            .view = chunk_view,
            .font = font,
            .start = start,
            .length = byte_length,
            .has_breaking_newline = has_breaking_newline,
            .is_all_whitespace = is_all_whitespace(chunk_view.as_string()),
            .text_type = text_type,
        };
    }

    return {};
}

JS::GCPtr<Painting::Paintable> TextNode::create_paintable() const
{
    return Painting::TextPaintable::create(*this, text_for_rendering());
}

}
