/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf8View.h>
#include <LibGfx/TextLayout.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

class LineBoxFragment;

class TextNode final : public Node {
    GC_CELL(TextNode, Node);
    GC_DECLARE_ALLOCATOR(TextNode);

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
        bool has_breaking_tab { false };
        bool is_all_whitespace { false };
        Gfx::GlyphRun::TextType text_type;
    };

    class ChunkIterator {
    public:
        ChunkIterator(TextNode const&, bool wrap_lines, bool respect_linebreaks);

        Optional<Chunk> next();
        Optional<Chunk> peek(size_t);

    private:
        Optional<Chunk> next_without_peek();
        Optional<Chunk> try_commit_chunk(size_t start, size_t end, bool has_breaking_newline, bool has_breaking_tab, Gfx::Font const&, Gfx::GlyphRun::TextType) const;

        bool const m_wrap_lines;
        bool const m_respect_linebreaks;
        Utf8View m_utf8_view;
        Gfx::FontCascadeList const& m_font_cascade_list;

        Unicode::Segmenter& m_grapheme_segmenter;
        size_t m_current_index { 0 };

        Vector<Chunk> m_peek_queue;
    };

    void invalidate_text_for_rendering();
    void compute_text_for_rendering();

    Unicode::Segmenter& grapheme_segmenter() const;

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_text_node() const final { return true; }

    Optional<String> m_text_for_rendering;
    mutable OwnPtr<Unicode::Segmenter> m_grapheme_segmenter;
};

template<>
inline bool Node::fast_is<TextNode>() const { return is_text_node(); }

}
