/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf8View.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

class LineBoxFragment;

class TextNode final : public Node {
    JS_CELL(TextNode, Node);
    JS_DECLARE_ALLOCATOR(TextNode);

public:
    TextNode(DOM::Document&, DOM::Text&);
    virtual ~TextNode() override;

    const DOM::Text& dom_node() const { return static_cast<const DOM::Text&>(*Node::dom_node()); }

    String const& text_for_rendering() const;

    struct Chunk {
        Utf8View view;
        NonnullRefPtr<Gfx::Font> font;
        size_t start { 0 };
        size_t length { 0 };
        bool has_breaking_newline { false };
        bool is_all_whitespace { false };
    };

    class ChunkIterator {
    public:
        ChunkIterator(StringView text, bool wrap_lines, bool respect_linebreaks, Gfx::FontCascadeList const&);
        Optional<Chunk> next();

    private:
        Optional<Chunk> try_commit_chunk(Utf8View::Iterator const& start, Utf8View::Iterator const& end, bool has_breaking_newline, Gfx::Font const&) const;

        bool const m_wrap_lines;
        bool const m_respect_linebreaks;
        Utf8View m_utf8_view;
        Utf8View::Iterator m_iterator;
        Gfx::FontCascadeList const& m_font_cascade_list;
    };

    void invalidate_text_for_rendering();
    void compute_text_for_rendering();

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_text_node() const final { return true; }

    Optional<String> m_text_for_rendering;
};

template<>
inline bool Node::fast_is<TextNode>() const { return is_text_node(); }

}
