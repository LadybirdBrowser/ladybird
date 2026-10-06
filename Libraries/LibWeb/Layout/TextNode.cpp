/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
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

}
