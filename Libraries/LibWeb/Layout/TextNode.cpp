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
#include <LibWeb/DOM/InvalidationJournal.h>
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
    Painting::push_selection_pseudo_style_of_parent(*this);
}

TextNode::TextNode(DOM::Document& document, BindToPreparedArenaSlot bind, Compositing::RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
    : Node(document, bind, slot, kind)
{
    invalidate_text_for_rendering();
    // A generated text row stands for no DOM text node, and paints its selection the way its parent does.
    if (!dom_node())
        return;
    update_produces_line_box_fragment_when_empty_flag();
    Painting::push_selection_pseudo_style_of_parent(*this);
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

GeneratedTextNode::GeneratedTextNode(DOM::Document& document, Utf16String text)
    : TextNode(document, RustFFI::NodeKind::GeneratedTextNode)
    , m_text(move(text))
{
    // No DOM text node holds these characters for the style mirror to publish, so the row keeps them itself.
    RustFFI::layout_arena_set_generated_text(arena_handle(), slot_id(this), m_text.to_raw_leaked());
}

// The build stamped the row with its characters, which this layout node shares.
GeneratedTextNode::GeneratedTextNode(DOM::Document& document, BindToPreparedArenaSlot bind, Compositing::RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
    : TextNode(document, bind, slot, kind)
    , m_text(Utf16String::adopt_raw(RustFFI::layout_arena_generated_text(arena_handle(), slot)))
{
}

GeneratedTextNode::~GeneratedTextNode() = default;

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
    if (auto identity = Painting::journal_identity_of(*this))
        const_cast<DOM::Document&>(document()).invalidation_journal().note_needs_repaint(identity, should_invalidate_display_list);
    else
        Painting::apply_text_repaint_damage(*this, should_invalidate_display_list);
}

}
