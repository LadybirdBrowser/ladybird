/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/BoxViews.h>

namespace Web::Layout {

TextNode::TextNode(DOM::Document& document, DOM::Text& text, AttachToDOMNode attach_to_dom_node)
    : Node(document, &text, RustFFI::NodeKind::TextNode, attach_to_dom_node)
{
    invalidate_text_for_rendering();
    update_produces_line_box_fragment_when_empty_flag();
}

TextNode::TextNode(DOM::Document& document, RustFFI::NodeKind kind)
    : Node(document, nullptr, kind)
{
    invalidate_text_for_rendering();
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
    : TextNode(document, RustFFI::NodeKind::GeneratedTextNode)
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

Utf16String TextNode::rendered_text_for_dom(bool collapse_whitespace) const
{
    Utf16String text;
    RustFFI::layout_arena_collect_rendered_text(arena_handle(), slot_id(this), collapse_whitespace, &text,
        [](void* context, RustFFI::FfiRenderedTextView view) {
            *static_cast<Utf16String*>(context) = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(view.text), view.length_in_code_units });
        });
    return text;
}

RustFFI::FfiTextSourceRange TextNode::word_range_at(size_t dom_offset) const
{
    return RustFFI::layout_arena_text_word_range(arena_handle(), slot_id(this), dom_offset);
}

void TextNode::invalidate_text_for_rendering()
{
    RustFFI::layout_arena_invalidate_text_content(arena_handle(), slot_id(this));
}

Utf16View TextNode::text_for_rendering() const
{
    auto view = RustFFI::layout_arena_text_for_rendering(arena_handle(), slot_id(this));
    return Utf16View { reinterpret_cast<char16_t const*>(view.text), view.length_in_code_units };
}

RustFFI::FfiTextSource TextNode::text_source() const
{
    Optional<Utf16View> lang;
    if (auto element = parent_element_for_text_transform())
        lang = element->lang_view();
    auto view_for = [](Utf16View view) -> RustFFI::FfiUtf16View {
        return {
            .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
            .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
            .length = view.length_in_code_units(),
        };
    };
    return {
        .text = view_for(text()),
        .locale = lang.has_value() ? view_for(*lang) : RustFFI::FfiUtf16View {},
        .has_locale = lang.has_value(),
        .is_password_input = is_password_input(),
    };
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
        if (Painting::has_committed_box(*containing_block))
            Painting::set_needs_repaint(*containing_block, should_invalidate_display_list);
    }

    if (should_invalidate_display_list == InvalidateDisplayList::Yes)
        RustFFI::layout_arena_invalidate_nearest_self_painting_inline_paint_cache(arena_handle(), slot_id(this));
}

}
