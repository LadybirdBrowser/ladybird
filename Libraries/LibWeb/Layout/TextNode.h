/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/Box.h>

namespace Web::Layout {

class GeneratedTextNode;
class TextSliceNode;

class TextNode : public Node {
public:
    TextNode(DOM::Document&, DOM::Text&);
    virtual ~TextNode() override;

    DOM::Text const& dom_node() const { return static_cast<DOM::Text const&>(*Node::dom_node()); }
    virtual DOM::Text const* dom_text() const { return &dom_node(); }

    virtual size_t dom_start_offset() const { return 0; }

    virtual size_t dom_length() const { return text().length_in_code_units(); }

    virtual Utf16String const& text() const { return dom_node().data(); }

    // Borrows the arena's rendered text until this node's content is republished or freed.
    Utf16View text_for_rendering() const;
    void invalidate_text_for_rendering();

    void sync_text_content_to_arena() const;

    void set_needs_repaint(InvalidateDisplayList = InvalidateDisplayList::Yes) const;

    bool update_produces_line_box_fragment_when_empty_flag();

protected:
    TextNode(DOM::Document&, DOM::Text&, AttachToDOMNode, RustFFI::NodeKind = RustFFI::NodeKind::TextNode);
    TextNode(DOM::Document&, RustFFI::NodeKind);

    virtual GC::Ptr<DOM::Element const> parent_element_for_text_transform() const;
    virtual bool is_password_input() const;

private:
    virtual bool is_text_node() const final { return true; }
};

class GeneratedTextNode final : public TextNode {
public:
    GeneratedTextNode(DOM::Document&, Utf16String);
    virtual ~GeneratedTextNode() override;

    virtual DOM::Text const* dom_text() const override { return nullptr; }
    virtual Utf16String const& text() const override { return m_text; }

private:
    virtual GC::Ptr<DOM::Element const> parent_element_for_text_transform() const override;
    virtual bool is_password_input() const override { return false; }

    Utf16String m_text;
};

class TextSliceNode final : public TextNode {
public:
    TextSliceNode(DOM::Document&, DOM::Text&, AttachToDOMNode, size_t dom_start_offset, size_t dom_length);
    virtual ~TextSliceNode() override;

    virtual size_t dom_start_offset() const override { return m_dom_start_offset; }
    virtual size_t dom_length() const override { return m_dom_length_in_code_units; }

    // Only meaningful on a remainder slice. Returns the first-letter slice that renders the leading
    // sub-range of the same DOM::Text, or nullptr if first-letter is not active for this DOM::Text.
    TextSliceNode const* first_letter_slice() const { return m_first_letter_slice.ptr(); }

    void set_first_letter_slice(TextSliceNode& slice) { m_first_letter_slice = slice; }

private:
    virtual bool is_text_slice_node() const override { return true; }

    size_t m_dom_start_offset { 0 };
    size_t m_dom_length_in_code_units { 0 };
    WeakPtr<TextSliceNode> m_first_letter_slice;
};

// Classifies a code point for direction-run splitting during text chunking:
// strong LTR/RTL, direction-neutral Common, or ContextDependent (resolved
// from surrounding runs).
Gfx::GlyphRun::TextType text_type_for_code_point(u32 code_point);

template<>
inline bool Node::fast_is<TextNode>() const { return is_text_node(); }

template<>
inline bool Node::fast_is<TextSliceNode>() const { return is_text_slice_node(); }

}
