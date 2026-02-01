/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/StringBuilder.h>
#include <AK/UnicodeUtils.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Locale.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/TextPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(TextNode);

TextNode::TextNode(DOM::Document& document, DOM::Text& text)
    : Node(document, &text)
{
}

TextNode::~TextNode() = default;

// https://w3c.github.io/mathml-core/#new-text-transform-values
static Utf16String apply_math_auto_text_transform(Utf16String const& string)
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

    StringBuilder builder { StringBuilder::Mode::UTF16, string.length_in_code_units() };

    for (auto code_point : string)
        builder.append_code_point(map_code_point_to_italic(code_point));

    return builder.to_utf16_string();
}

static Utf16String apply_text_transform(Utf16String const& string, CSS::TextTransform text_transform, Optional<StringView> const& locale)
{
    switch (text_transform) {
    case CSS::TextTransform::Uppercase:
        return string.to_uppercase(locale);
    case CSS::TextTransform::Lowercase:
        return string.to_lowercase(locale);
    case CSS::TextTransform::None:
        return string;
    case CSS::TextTransform::MathAuto:
        return apply_math_auto_text_transform(string);
    case CSS::TextTransform::Capitalize:
        return string.to_titlecase(locale, TrailingCodePointTransformation::PreserveExisting);
    case CSS::TextTransform::FullSizeKana:
        dbgln("FIXME: Implement text-transform full-size-kana");
        return string;
    case CSS::TextTransform::FullWidth:
        return string.to_fullwidth();
    }

    VERIFY_NOT_REACHED();
}

void TextNode::invalidate_text_for_rendering()
{
    m_text_for_rendering = {};
    m_grapheme_segmenter.clear();
    m_line_segmenter.clear();
}

Utf16String const& TextNode::text_for_rendering() const
{
    if (!m_text_for_rendering.has_value())
        const_cast<TextNode*>(this)->compute_text_for_rendering();
    return *m_text_for_rendering;
}

void TextNode::compute_text_for_rendering()
{
    if (dom_node().is_password_input()) {
        m_text_for_rendering = Utf16String::repeated(u'●', dom_node().data().length_in_code_points());
        return;
    }

    // Apply text-transform
    // FIXME: This can generate more code points than there were before; we need to find a better way to map the
    //        resulting paintable fragments' offsets into the original text node data.
    //        See: https://github.com/LadybirdBrowser/ladybird/issues/6177
    auto parent_element = dom_node().parent_element();
    auto const maybe_lang = parent_element ? parent_element->lang() : Optional<String> {};
    auto const lang = maybe_lang.has_value() ? maybe_lang.value() : Optional<StringView> {};
    auto text = apply_text_transform(dom_node().data(), computed_values().text_transform(), lang);

    // The logic below deals with converting whitespace characters. If we don't have them, return early.
    if (text.is_empty() || !any_of(text, is_ascii_space)) {
        m_text_for_rendering = move(text);
        return;
    }

    // https://drafts.csswg.org/css-text-4/#white-space-phase-1
    bool convert_newlines = false;
    bool convert_tabs = false;

    // If white-space-collapse is set to collapse or preserve-breaks, white space characters are considered collapsible
    // and are processed by performing the following steps:
    auto white_space_collapse = computed_values().white_space_collapse();
    if (first_is_one_of(white_space_collapse, CSS::WhiteSpaceCollapse::Collapse, CSS::WhiteSpaceCollapse::PreserveBreaks)) {
        // 1. FIXME: Any sequence of collapsible spaces and tabs immediately preceding or following a segment break is removed.

        // 2. Collapsible segment breaks are transformed for rendering according to the segment break transformation
        //    rules.
        {
            // https://drafts.csswg.org/css-text-4/#line-break-transform
            // FIXME: When white-space-collapse is not collapse, segment breaks are not collapsible. For values other than
            // collapse or preserve-spaces (which transforms them into spaces), segment breaks are instead transformed
            // into a preserved line feed (U+000A).

            // When white-space-collapse is collapse, segment breaks are collapsible, and are collapsed as follows:
            if (white_space_collapse == CSS::WhiteSpaceCollapse::Collapse) {
                // 1. FIXME: First, any collapsible segment break immediately following another collapsible segment break is
                //    removed.

                // 2. FIXME: Then any remaining segment break is either transformed into a space (U+0020) or removed depending
                //    on the context before and after the break. The rules for this operation are UA-defined in this
                //    level.
                convert_newlines = true;
            }
        }

        // 3. Every collapsible tab is converted to a collapsible space (U+0020).
        convert_tabs = true;

        // 4. Any collapsible space immediately following another collapsible space—even one outside the boundary of the
        //    inline containing that space, provided both spaces are within the same inline formatting context—is
        //    collapsed to have zero advance width. (It is invisible, but retains its soft wrap opportunity, if any.)
        // AD-HOC: This is handled by TextNode::ChunkIterator by removing the space.
    }

    // If white-space-collapse is set to preserve-spaces, each tab and segment break is converted to a space.
    if (white_space_collapse == CSS::WhiteSpaceCollapse::PreserveSpaces) {
        convert_tabs = true;
        convert_newlines = true;
    }

    // AD-HOC: Prevent allocating a StringBuilder for a single space/newline/tab.
    if (text == " "sv || (convert_tabs && text == "\t"sv) || (convert_newlines && text == "\n"sv)) {
        m_text_for_rendering = " "_utf16;
        return;
    }

    // AD-HOC: It's important to not change the amount of code units in the resulting transformed text, so ChunkIterator
    //         can pass views to this string with associated code unit offsets that still match the original text.
    if (convert_newlines || convert_tabs) {
        StringBuilder text_builder { StringBuilder::Mode::UTF16, text.length_in_code_units() };
        for (auto code_point : text) {
            if ((convert_newlines && code_point == '\n') || (convert_tabs && code_point == '\t'))
                code_point = ' ';
            text_builder.append_code_point(code_point);
        }
        text = text_builder.to_utf16_string();
    }

    m_text_for_rendering = move(text);
}

Unicode::Segmenter& TextNode::grapheme_segmenter() const
{
    if (!m_grapheme_segmenter) {
        auto const& text = text_for_rendering();
        // Fast path: For ASCII text, every character is its own grapheme.
        // We can use a trivial segmenter that avoids all ICU overhead.
        if (text.is_ascii()) {
            m_grapheme_segmenter = Unicode::Segmenter::create_for_ascii_grapheme(text.length_in_code_units());
        } else {
            m_grapheme_segmenter = document().grapheme_segmenter().clone();
            m_grapheme_segmenter->set_segmented_text(text);
        }
    }

    return *m_grapheme_segmenter;
}

Unicode::Segmenter& TextNode::line_segmenter() const
{
    if (!m_line_segmenter) {
        m_line_segmenter = document().line_segmenter().clone();
        m_line_segmenter->set_segmented_text(text_for_rendering());
    }

    return *m_line_segmenter;
}

static bool is_interword_space(u32 code_point)
{
    return code_point == 0x0020 || code_point == 0x00A0;
}

TextNode::ChunkIterator::ChunkIterator(TextNode const& text_node, bool should_wrap_lines, bool should_respect_linebreaks)
    : ChunkIterator(text_node, text_node.text_for_rendering(), text_node.grapheme_segmenter(), text_node.line_segmenter(), text_node.computed_values().word_break(), should_wrap_lines, should_respect_linebreaks)
{
}

TextNode::ChunkIterator::ChunkIterator(TextNode const& text_node, Utf16View const& text,
    Unicode::Segmenter& grapheme_segmenter, Unicode::Segmenter& line_segmenter, CSS::WordBreak word_break,
    bool should_wrap_lines, bool should_respect_linebreaks)
    : m_should_wrap_lines(should_wrap_lines)
    , m_should_respect_linebreaks(should_respect_linebreaks)
    , m_view(text)
    , m_font_cascade_list(text_node.computed_values().font_list())
    , m_grapheme_segmenter(grapheme_segmenter)
    , m_line_segmenter(line_segmenter)
    , m_word_break(word_break)
{
    m_should_collapse_whitespace = first_is_one_of(text_node.computed_values().white_space_collapse(), CSS::WhiteSpaceCollapse::Collapse, CSS::WhiteSpaceCollapse::PreserveBreaks);
}

static Gfx::GlyphRun::TextType text_type_for_code_point(u32 code_point)
{
    // Fast path for ASCII using a lookup table.
    // Each ASCII character has a statically known bidi class.
    if (code_point < 0x80) {
        using enum Gfx::GlyphRun::TextType;
        // clang-format off
        static constexpr auto L = Ltr;
        static constexpr auto C = Common;
        static constexpr auto X = ContextDependent;
        static constexpr Gfx::GlyphRun::TextType ascii_text_types[128] = {
            // 0x00-0x0F: Control characters (BN=Common, S/B/WS=ContextDependent)
            C, C, C, C, C, C, C, C, C, X, X, X, X, X, C, C,
            // 0x10-0x1F: Control characters
            C, C, C, C, C, C, C, C, C, C, C, C, X, X, X, X,
            // 0x20-0x2F: Space and punctuation
            X, C, C, X, X, X, C, C, C, C, C, X, X, X, X, X,
            // 0x30-0x3F: Digits and punctuation
            X, X, X, X, X, X, X, X, X, X, X, C, C, C, C, C,
            // 0x40-0x4F: @ and uppercase letters
            C, L, L, L, L, L, L, L, L, L, L, L, L, L, L, L,
            // 0x50-0x5F: Uppercase letters and punctuation
            L, L, L, L, L, L, L, L, L, L, L, C, C, C, C, C,
            // 0x60-0x6F: ` and lowercase letters
            C, L, L, L, L, L, L, L, L, L, L, L, L, L, L, L,
            // 0x70-0x7F: Lowercase letters and punctuation
            L, L, L, L, L, L, L, L, L, L, L, C, C, C, C, C,
        };
        // clang-format on
        return ascii_text_types[code_point];
    }

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

TextNode::Chunk TextNode::ChunkIterator::create_empty_chunk()
{
    return TextNode::Chunk {
        .view = {},
        .font = m_font_cascade_list.first(),
        .is_all_whitespace = true,
        .text_type = Gfx::GlyphRun::TextType::Common,
    };
}

bool TextNode::ChunkIterator::is_at_line_break_opportunity() const
{
    auto has_break_all_class = [](u32 code_point) {
        return first_is_one_of(Unicode::line_break_class(code_point),
            Unicode::LineBreakClass::Alphabetic,
            Unicode::LineBreakClass::Numeric,
            Unicode::LineBreakClass::ComplexContext,
            Unicode::LineBreakClass::Ideographic);
    };

    auto has_keep_all_class = [](u32 code_point) {
        return first_is_one_of(Unicode::line_break_class(code_point),
            Unicode::LineBreakClass::Alphabetic,
            Unicode::LineBreakClass::Numeric,
            Unicode::LineBreakClass::Ambiguous,
            Unicode::LineBreakClass::Ideographic);
    };

    auto get_previous_code_point = [this]() -> Optional<u32> {
        if (m_current_index == 0)
            return {};
        size_t current_index = m_current_index;
        auto previous_code_point = m_view.previous_code_point_at(current_index);
        while (Unicode::line_break_class(previous_code_point) == Unicode::LineBreakClass::CombiningMark && current_index > 0)
            previous_code_point = m_view.previous_code_point_at(current_index);
        return previous_code_point;
    };

    if (!m_should_wrap_lines)
        return false;

    auto is_at_line_segmenter_boundary = [this]() {
        auto boundary = m_line_segmenter.next_boundary(m_current_index, Unicode::Segmenter::Inclusive::Yes);
        return boundary.has_value() && boundary.value() == m_current_index;
    };

    // https://drafts.csswg.org/css-text-4/#word-break-property
    // This property specifies soft wrap opportunities between and within “words”, i.e. where it is “normal” and
    // permissible to break lines of text. It focuses on breaks between letters, and does not define whether and how
    // soft wrap opportunities are created by white space and other space separators (though auto-phrase may suppress
    // some), nor around punctuation.
    switch (m_word_break) {
    case CSS::WordBreak::Normal:
        // https://drafts.csswg.org/css-text-4/#valdef-word-break-normal
        // Words break according to their customary rules, as described above. Korean, which commonly exhibits two
        // different behaviors, allows breaks between any two consecutive Hangul/Hanja. For Ethiopic, which also
        // exhibits two different behaviors, such breaks within words are not allowed.
    case CSS::WordBreak::BreakWord:
        // https://drafts.csswg.org/css-text-4/#valdef-word-break-break-word
        // For compatibility with legacy content, the word-break property also supports a deprecated break-word
        // keyword. When specified, this has the same effect as word-break: normal and overflow-wrap: anywhere,
        // regardless of the actual value of the overflow-wrap property.
        return is_at_line_segmenter_boundary();
    case CSS::WordBreak::BreakAll: {
        // https://drafts.csswg.org/css-text-4/#valdef-word-break-break-all
        // Breaking is allowed within “words”: specifically, in addition to soft wrap opportunities allowed for normal,
        // any typographic letter units (and any typographic character units resolving to the NU (“numeric”),
        // AL (“alphabetic”), or SA (“Southeast Asian”) line breaking classes [UAX14]) are instead treated as ID
        // (“ideographic characters”) for the purpose of line-breaking. Hyphenation is not applied.
        if (m_current_index >= m_view.length_in_code_units())
            return false;
        auto previous_code_point = get_previous_code_point();
        if (previous_code_point.has_value() && has_break_all_class(*previous_code_point) && has_break_all_class(m_view.code_point_at(m_current_index)))
            return true;
        return is_at_line_segmenter_boundary();
    }
    case CSS::WordBreak::KeepAll: {
        // https://drafts.csswg.org/css-text-4/#valdef-word-break-keep-all
        // Breaking is forbidden within “words”: implicit soft wrap opportunities between typographic letter units
        // (or other typographic character units belonging to the NU, AL, AI, or ID Unicode line breaking classes [UAX14])
        // are suppressed, i.e. breaks are prohibited between pairs of such characters (regardless of line-break
        // settings other than anywhere) except where opportunities exist due to §6.1.1.1 Lexical Word Breaking.
        // Otherwise this option is equivalent to normal. In this style, sequences of CJK characters do not break.
        if (m_current_index >= m_view.length_in_code_units())
            return false;
        auto previous_code_point = get_previous_code_point();
        if (previous_code_point.has_value() && has_keep_all_class(*previous_code_point) && has_keep_all_class(m_view.code_point_at(m_current_index)))
            return false;
        return is_at_line_segmenter_boundary();
    }
    }
    VERIFY_NOT_REACHED();
}

Gfx::Font const& TextNode::ChunkIterator::font_for_space(size_t at_index) const
{
    // 1. Prefer the last non-whitespace font in this node/run.
    if (m_last_non_whitespace_font && !m_last_non_whitespace_font->is_emoji_font())
        return *m_last_non_whitespace_font;

    // 2. Look ahead to the next non-space to infer the base font of this run.
    for (size_t i = at_index; i < m_view.length_in_code_units();) {
        auto cp = m_view.code_point_at(i);
        if (!is_interword_space(cp) && cp != '\t' && cp != '\n') {
            auto const& font = m_font_cascade_list.font_for_code_point(cp);
            if (!font.is_emoji_font())
                return font;
            // Text is coming from an emoji face; we'll fall back to (3).
            break;
        }
        i = m_grapheme_segmenter.next_boundary(i).value_or(m_view.length_in_code_units());
    }

    // 3. No text around (leading/trailing/all spaces) — pick the first *text* face in the cascade.
    return m_font_cascade_list.first_text_face();
}

Optional<TextNode::Chunk> TextNode::ChunkIterator::next_without_peek()
{
    if (m_current_index >= m_view.length_in_code_units())
        return {};

    auto current_code_point = [this] {
        return m_view.code_point_at(m_current_index);
    };
    auto next_grapheme_boundary = [this] {
        return m_grapheme_segmenter.next_boundary(m_current_index).value_or(m_view.length_in_code_units());
    };

    // https://drafts.csswg.org/css-text-4/#collapsible-white-space
    auto is_collapsible = [this](u32 code_point) {
        return m_should_collapse_whitespace && is_ascii_space(code_point);
    };

    auto code_point = current_code_point();
    auto can_break_at_current_position = is_at_line_break_opportunity();
    auto start_of_chunk = m_current_index;

    auto const& expected_font_for = [&](u32 cp) -> Gfx::Font const& {
        return is_interword_space(cp)
            ? font_for_space(m_current_index)
            : m_font_cascade_list.font_for_code_point(cp);
    };

    auto const& font = expected_font_for(current_code_point());
    auto text_type = text_type_for_code_point(code_point);

    auto broken_on_tab = false;

    while (m_current_index < m_view.length_in_code_units()) {
        code_point = current_code_point();

        if (code_point == '\t') {
            if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, broken_on_tab, false, font, text_type); result.has_value())
                return result.release_value();

            broken_on_tab = true;
            // consume any consecutive tabs
            while (m_current_index < m_view.length_in_code_units() && current_code_point() == '\t')
                m_current_index = next_grapheme_boundary();
            can_break_at_current_position = is_at_line_break_opportunity();
        }

        auto const& expected_font = expected_font_for(code_point);

        if (&font != &expected_font) {
            if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, broken_on_tab, can_break_at_current_position, font, text_type); result.has_value())
                return result.release_value();
        }

        if (m_should_respect_linebreaks && code_point == '\n') {
            // Newline encountered, and we're supposed to preserve them.
            // If we have accumulated some code points in the current chunk, commit them now and continue with the newline next time.
            if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, broken_on_tab, false, font, text_type); result.has_value())
                return result.release_value();

            // Otherwise, commit the newline!
            m_current_index = next_grapheme_boundary();
            auto result = try_commit_chunk(start_of_chunk, m_current_index, true, broken_on_tab, false, font, text_type);
            VERIFY(result.has_value());
            return result.release_value();
        }

        // If both this code point and the previous code point are collapsible, skip code points until we're at a non-
        // collapsible code point.
        if (is_collapsible(code_point) && m_current_index > 0 && is_collapsible(m_view.code_point_at(m_current_index - 1))) {
            auto result = try_commit_chunk(start_of_chunk, m_current_index, false, broken_on_tab, false, font, text_type);

            while (m_current_index < m_view.length_in_code_units() && is_collapsible(current_code_point()))
                m_current_index = next_grapheme_boundary();
            can_break_at_current_position = is_at_line_break_opportunity();

            if (result.has_value())
                return result.release_value();
        }

        if (m_should_wrap_lines) {
            if (text_type != text_type_for_code_point(code_point)) {
                if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, broken_on_tab, can_break_at_current_position, font, text_type); result.has_value())
                    return result.release_value();
            }

            if (is_ascii_space(code_point)) {
                // Whitespace encountered, and we're allowed to break on whitespace.
                // If we have accumulated some code points in the current chunk, commit them now and continue with the whitespace next time.
                if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, broken_on_tab, false, font, text_type); result.has_value())
                    return result.release_value();

                // Otherwise, commit the whitespace!
                m_current_index = next_grapheme_boundary();
                can_break_at_current_position = is_at_line_break_opportunity();
                auto const& space_font = font_for_space(m_current_index);
                if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, broken_on_tab, false, space_font, text_type); result.has_value())
                    return result.release_value();
                continue;
            }

            if (can_break_at_current_position) {
                if (auto result = try_commit_chunk(start_of_chunk, m_current_index, false, broken_on_tab, true, font, text_type); result.has_value())
                    return result.release_value();
            }
        }

        m_current_index = next_grapheme_boundary();
        can_break_at_current_position = is_at_line_break_opportunity();
    }

    if (start_of_chunk != m_view.length_in_code_units()) {
        // Try to output whatever's left at the end of the text node.
        if (auto result = try_commit_chunk(start_of_chunk, m_view.length_in_code_units(), false, broken_on_tab, false, font, text_type); result.has_value())
            return result.release_value();
    }

    return {};
}

Optional<TextNode::Chunk> TextNode::ChunkIterator::try_commit_chunk(size_t start, size_t end, bool has_breaking_newline, bool has_breaking_tab, bool can_break_after, Gfx::Font const& font, Gfx::GlyphRun::TextType text_type) const
{
    if (auto length_in_code_units = end - start; length_in_code_units > 0) {
        auto chunk_view = m_view.substring_view(start, length_in_code_units);
        auto is_all_whitespace = chunk_view.is_ascii_whitespace();
        if (!is_all_whitespace)
            m_last_non_whitespace_font = font;
        return Chunk {
            .view = chunk_view,
            .font = font,
            .start = start,
            .length = length_in_code_units,
            .has_breaking_newline = has_breaking_newline,
            .has_breaking_tab = has_breaking_tab,
            .is_all_whitespace = is_all_whitespace,
            .can_break_after = can_break_after,
            .text_type = text_type,
        };
    }

    return {};
}

GC::Ptr<Painting::Paintable> TextNode::create_paintable() const
{
    return Painting::TextPaintable::create(*this);
}

}
