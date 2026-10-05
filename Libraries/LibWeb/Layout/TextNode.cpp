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

// The tree build stamped the row with whether an empty text produces a line box fragment and enrolled it for content
// sync. A layout node is made whenever something first asks for it, so it does nothing but bind.
TextNode::TextNode(DOM::Document& document, BindToPreparedArenaSlot bind, Compositing::RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
    : Node(document, bind, slot, kind)
{
}

TextNode::~TextNode() = default;

// The build stamped the row with its characters, which this layout node shares.
GeneratedTextNode::GeneratedTextNode(DOM::Document& document, BindToPreparedArenaSlot bind, Compositing::RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
    : TextNode(document, bind, slot, kind)
    , m_text(Utf16String::adopt_raw(RustFFI::layout_row_generated_text(document_host(), slot)))
{
}

GeneratedTextNode::~GeneratedTextNode() = default;

Utf16String TextNode::rendered_text_for_dom(bool collapse_whitespace) const
{
    Utf16String text;
    RustFFI::layout_script_rendered_text(document_host(), slot_id(this), collapse_whitespace, &text,
        [](void* context, RustFFI::FfiRenderedTextView view) {
            *static_cast<Utf16String*>(context) = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(view.text), view.length_in_code_units });
        });
    return text;
}

RustFFI::FfiTextSourceRange TextNode::word_range_at(size_t dom_offset) const
{
    return RustFFI::layout_text_word_range(document_host(), slot_id(this), dom_offset);
}

Utf16View TextNode::text_for_rendering() const
{
    auto view = RustFFI::render_state_text_for_rendering(document_host(), slot_id(this));
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
    Painting::mark_box(*this, Painting::repaint_marks(should_invalidate_display_list));
}

}
