/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/UnicodeUtils.h>
#include <AK/Utf16StringBuilder.h>
#include <LibUnicode/Bidi.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Locale.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/InlinePaintable.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Layout {

TextNode::TextNode(DOM::Document& document, DOM::Text& text)
    : Node(document, &text)
{
    enroll_for_arena_text_content_sync();
    update_produces_line_box_fragment_when_empty_flag();
}

TextNode::TextNode(DOM::Document& document, DOM::Text& text, AttachToDOMNode attach_to_dom_node)
    : Node(document, &text, attach_to_dom_node)
{
    enroll_for_arena_text_content_sync();
    update_produces_line_box_fragment_when_empty_flag();
}

TextNode::TextNode(DOM::Document& document)
    : Node(document, nullptr)
{
    enroll_for_arena_text_content_sync();
}

bool TextNode::update_produces_line_box_fragment_when_empty_flag()
{
    // Text controls and editing hosts rely on their text node producing a zero-width fragment even
    // when it has no text: the fragment keeps the line box alive with real font metrics, giving the
    // caret an anchor to paint at and the control its baseline. Stamping this as a node flag keeps
    // layout itself unaware of editing state.
    auto produces_line_box_fragment_when_empty = [&] {
        auto const* dom_text = this->dom_text();
        if (!dom_text)
            return false;
        if (auto const* shadow_root = as_if<DOM::ShadowRoot>(dom_text->root())) {
            if (as_if<HTML::FormAssociatedTextControlElement>(shadow_root->host()))
                return true;
        }
        return dom_text->parent() && dom_text->parent()->is_editing_host();
    }();
    if (has_flag(RustFFI::NodeFlag::ProducesLineBoxFragmentWhenEmpty) == produces_line_box_fragment_when_empty)
        return false;
    set_flag(RustFFI::NodeFlag::ProducesLineBoxFragmentWhenEmpty, produces_line_box_fragment_when_empty);
    return true;
}

TextNode::~TextNode() = default;

GC::Ptr<DOM::Element const> TextNode::parent_element_for_text_transform() const
{
    return dom_node().parent_element();
}

bool TextNode::is_password_input() const
{
    return dom_node().is_password_input();
}

GeneratedTextNode::GeneratedTextNode(DOM::Document& document, Utf16String text)
    : TextNode(document)
    , m_text(move(text))
{
}

GeneratedTextNode::~GeneratedTextNode() = default;

GC::Ptr<DOM::Element const> GeneratedTextNode::parent_element_for_text_transform() const
{
    if (is_generated_for_pseudo_element())
        return pseudo_element_generator();
    if (auto const* parent = this->parent(); parent && parent->is_generated_for_pseudo_element())
        return parent->pseudo_element_generator();
    return nullptr;
}

TextSliceNode::TextSliceNode(DOM::Document& document, DOM::Text& text, AttachToDOMNode attach_to_dom_node, size_t dom_start_offset, size_t dom_length)
    : TextNode(document, text, attach_to_dom_node)
    , m_dom_start_offset(dom_start_offset)
    , m_dom_length_in_code_units(dom_length)
{
}

TextSliceNode::~TextSliceNode() = default;

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

    Utf16StringBuilder builder { string.length_in_code_units() };

    for (auto code_point : string)
        builder.append_code_point(map_code_point_to_italic(code_point));

    return builder.to_string();
}

static Utf16String apply_text_transform(Utf16String const& string, CSS::TextTransform text_transform, Optional<Utf16View> const& locale)
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

TextNode::TextForRenderingCacheKey TextNode::create_text_for_rendering_cache_key() const
{
    auto text_transform = parent()->computed_values().text_transform();
    Optional<Utf16String> lang;
    if (first_is_one_of(text_transform, CSS::TextTransform::Uppercase, CSS::TextTransform::Lowercase, CSS::TextTransform::Capitalize)) {
        if (auto parent_element = parent_element_for_text_transform())
            lang = parent_element->lang();
    }

    return {
        .text_transform = text_transform,
        .white_space_collapse = parent()->computed_values().white_space_collapse(),
        .lang = move(lang),
        .is_password_input = is_password_input(),
        .dom_start_offset = dom_start_offset(),
        .dom_length = dom_length(),
    };
}

void TextNode::invalidate_text_for_rendering()
{
    m_text_dependent_cache = {};
    m_arena_text_content_in_sync = false;
    enroll_for_arena_text_content_sync();
}

Utf16String const& TextNode::text_for_rendering() const
{
    return ensure_text_dependent_cache().text_for_rendering;
}

TextNode::TextDependentCache const& TextNode::ensure_text_dependent_cache() const
{
    auto key = create_text_for_rendering_cache_key();
    if (!m_text_dependent_cache.has_value() || m_text_dependent_cache->key != key) {
        auto text_for_rendering = compute_text_for_rendering(key);
        m_text_dependent_cache = TextDependentCache {
            .key = move(key),
            .text_for_rendering = move(text_for_rendering),
            .grapheme_segmenter = {},
        };
        m_arena_text_content_in_sync = false;
        enroll_for_arena_text_content_sync();
    }
    return *m_text_dependent_cache;
}

void TextNode::enroll_for_arena_text_content_sync() const
{
    if (m_enrolled_for_arena_text_content_sync)
        return;
    m_enrolled_for_arena_text_content_sync = true;
    node_arena().enroll_text_node_for_content_sync(*this);
}

void TextNode::sync_text_content_to_arena() const
{
    ensure_text_dependent_cache();
    m_enrolled_for_arena_text_content_sync = false;
    if (m_arena_text_content_in_sync)
        return;
    auto view = m_text_dependent_cache->text_for_rendering.utf16_view();
    RustFFI::layout_arena_set_text_content(
        arena_handle(),
        slot_id(this),
        view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        view.length_in_code_units(),
        text().is_ascii_whitespace(),
        Unicode::may_require_bidi_processing(view));
    m_arena_text_content_in_sync = true;
}

Utf16String TextNode::compute_text_for_rendering(TextForRenderingCacheKey const& cache_key) const
{
    auto const& text_data = text();
    if (cache_key.is_password_input) {
        return Utf16String::repeated(u'●', text_data.length_in_code_points());
    }

    // Apply text-transform
    // FIXME: This can generate more code points than there were before; we need to find a better way to map the
    //        resulting paintable fragments' offsets into the original text node data.
    //        See: https://github.com/LadybirdBrowser/ladybird/issues/6177
    auto const lang = cache_key.lang.has_value() ? Optional<Utf16View> { cache_key.lang->utf16_view() } : Optional<Utf16View> {};
    auto text = apply_text_transform(text_data, cache_key.text_transform, lang);
    if (cache_key.dom_start_offset > 0 || cache_key.dom_length < text_data.length_in_code_units())
        text = Utf16String::from_utf16(text.utf16_view().substring_view(cache_key.dom_start_offset, cache_key.dom_length));

    // The logic below deals with converting whitespace characters. If we don't have them, return early.
    if (text.is_empty() || !any_of(text, is_ascii_space)) {
        return text;
    }

    // https://drafts.csswg.org/css-text-4/#white-space-phase-1
    bool convert_newlines = false;
    bool convert_tabs = false;

    // If white-space-collapse is set to collapse or preserve-breaks, white space characters are considered collapsible
    // and are processed by performing the following steps:
    auto white_space_collapse = cache_key.white_space_collapse;
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
        // AD-HOC: This is handled by the text chunker by removing the space.
    }

    // If white-space-collapse is set to preserve-spaces, each tab and segment break is converted to a space.
    if (white_space_collapse == CSS::WhiteSpaceCollapse::PreserveSpaces) {
        convert_tabs = true;
        convert_newlines = true;
    }

    // AD-HOC: Prevent allocating a StringBuilder for a single space/newline/tab.
    if (text == " "sv || (convert_tabs && text == "\t"sv) || (convert_newlines && text == "\n"sv)) {
        return " "_utf16;
    }

    // AD-HOC: It's important to not change the amount of code units in the resulting transformed text, so the text
    //         chunker can produce code unit offsets that still match the original text.
    if (convert_newlines || convert_tabs) {
        Utf16StringBuilder text_builder { text.length_in_code_units() };
        for (auto code_point : text) {
            if ((convert_newlines && code_point == '\n') || (convert_tabs && code_point == '\t'))
                code_point = ' ';
            text_builder.append_code_point(code_point);
        }
        text = text_builder.to_string();
    }

    return text;
}

Unicode::Segmenter& TextNode::grapheme_segmenter() const
{
    auto const& cache = ensure_text_dependent_cache();
    auto const& text = cache.text_for_rendering;
    if (!cache.grapheme_segmenter) {
        // Fast path: For ASCII text, every character is its own grapheme.
        // We can use a trivial segmenter that avoids all ICU overhead.
        if (text.is_ascii()) {
            cache.grapheme_segmenter = Unicode::Segmenter::create_for_ascii_grapheme(text.length_in_code_units());
        } else {
            cache.grapheme_segmenter = document().grapheme_segmenter().clone();
            cache.grapheme_segmenter->set_segmented_text(text);
        }
    }

    return *cache.grapheme_segmenter;
}

Gfx::GlyphRun::TextType text_type_for_code_point(u32 code_point)
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

void TextNode::set_needs_repaint(InvalidateDisplayList should_invalidate_display_list) const
{
    if (auto* containing_block = this->containing_block()) {
        if (auto paintable_box = const_cast<Box&>(*containing_block).paintable_box())
            paintable_box->set_needs_repaint(should_invalidate_display_list);
    }

    if (should_invalidate_display_list == InvalidateDisplayList::Yes) {
        if (auto const* self_painting_ancestor = Painting::nearest_self_painting_inline_box(*this))
            self_painting_ancestor->invalidate_paint_cache();
    }
}

}
