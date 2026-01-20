/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
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

    DOM::Text const& dom_node() const { return static_cast<DOM::Text const&>(*Node::dom_node()); }

    Utf16String const& text_for_rendering() const;

    struct Chunk {
        Utf16View view;
        NonnullRefPtr<Gfx::Font const> font;
        size_t start { 0 };
        size_t length { 0 };
        bool has_breaking_newline { false };
        bool has_breaking_tab { false };
        bool is_all_whitespace { false };
        bool can_break_after { false };
        Gfx::GlyphRun::TextType text_type;
    };

    class ChunkIterator {
    public:
        ChunkIterator(TextNode const&, bool should_wrap_lines, bool should_respect_linebreaks);
        ChunkIterator(TextNode const&, Utf16View const&, Unicode::Segmenter& grapheme_segmenter, Unicode::Segmenter& line_segmenter, CSS::WordBreak, bool should_wrap_lines, bool should_respect_linebreaks);

        bool should_wrap_lines() const { return m_should_wrap_lines; }
        bool should_respect_linebreaks() const { return m_should_respect_linebreaks; }
        bool should_collapse_whitespace() const { return m_should_collapse_whitespace; }

        Optional<Chunk> next();
        Optional<Chunk> peek(size_t);

        Chunk create_empty_chunk();

    private:
        Optional<Chunk> next_without_peek();
        Optional<Chunk> try_commit_chunk(size_t start, size_t end, bool has_breaking_newline, bool has_breaking_tab, bool can_break_after, Gfx::Font const&, Gfx::GlyphRun::TextType) const;

        [[nodiscard]] bool is_at_line_break_opportunity() const;
        [[nodiscard]] Gfx::Font const& font_for_space(size_t at_index) const;

        bool const m_should_wrap_lines;
        bool const m_should_respect_linebreaks;
        bool m_should_collapse_whitespace;
        Utf16View m_view;
        Gfx::FontCascadeList const& m_font_cascade_list;

        Unicode::Segmenter& m_grapheme_segmenter;
        Unicode::Segmenter& m_line_segmenter;
        CSS::WordBreak m_word_break;
        size_t m_current_index { 0 };

        Vector<Chunk> m_peek_queue;

        mutable RefPtr<Gfx::Font const> m_last_non_whitespace_font;
    };

    void invalidate_text_for_rendering();

    Unicode::Segmenter& grapheme_segmenter() const;
    Unicode::Segmenter& line_segmenter() const;

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_text_node() const final { return true; }

    void compute_text_for_rendering();

    Optional<Utf16String> m_text_for_rendering;
    mutable OwnPtr<Unicode::Segmenter> m_grapheme_segmenter;
    mutable OwnPtr<Unicode::Segmenter> m_line_segmenter;
};

template<>
inline bool Node::fast_is<TextNode>() const { return is_text_node(); }

}
