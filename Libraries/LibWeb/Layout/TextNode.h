/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/WeakPtr.h>
#include <LibGfx/TextLayout.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

class LineBoxFragment;
class GeneratedTextNode;
class TextSliceNode;

class TextNode : public Node {
    LAYOUT_NODE(TextNode, Node);

public:
    TextNode(DOM::Document&, DOM::Text&);
    virtual ~TextNode() override;

    DOM::Text const& dom_node() const { return static_cast<DOM::Text const&>(*Node::dom_node()); }
    virtual DOM::Text const* dom_text() const { return &dom_node(); }

    virtual size_t dom_start_offset() const { return 0; }

    virtual size_t dom_length() const { return text().length_in_code_units(); }

    virtual Utf16String const& text() const { return dom_node().data(); }

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
        [[nodiscard]] Gfx::Font const& font_for_space(size_t at_index, u32 space_code_point) const;

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

    struct ChunkList {
        Vector<Chunk> chunks;
        bool should_collapse_whitespace { false };
    };

    ChunkList const& chunks_for_layout(bool should_wrap_lines, bool should_respect_linebreaks) const;

    void invalidate_text_for_rendering();

    Unicode::Segmenter& grapheme_segmenter() const;
    Unicode::Segmenter& line_segmenter() const;

    virtual RefPtr<Painting::Paintable> create_paintable() const override;

protected:
    TextNode(DOM::Document&, DOM::Text&, AttachToDOMNode);
    explicit TextNode(DOM::Document&);

    virtual DOM::Element const* parent_element_for_text_transform() const;
    virtual bool is_password_input() const;

private:
    virtual bool is_text_node() const final { return true; }

    struct TextForRenderingCacheKey {
        CSS::TextTransform text_transform { CSS::TextTransform::None };
        CSS::WhiteSpaceCollapse white_space_collapse { CSS::WhiteSpaceCollapse::Collapse };
        Optional<String> lang;
        bool is_password_input { false };
        size_t dom_start_offset { 0 };
        size_t dom_length { 0 };

        bool operator==(TextForRenderingCacheKey const&) const = default;
    };

    struct ChunkCacheKey {
        bool should_wrap_lines { false };
        bool should_respect_linebreaks { false };
        CSS::WhiteSpaceCollapse white_space_collapse { CSS::WhiteSpaceCollapse::Collapse };
        CSS::WordBreak word_break { CSS::WordBreak::Normal };
        RefPtr<Gfx::FontCascadeList const> font_cascade_list;

        bool operator==(ChunkCacheKey const&) const = default;
    };

    struct ChunkCacheEntry {
        ChunkCacheKey key;
        ChunkList chunk_list;
    };

    struct TextDependentCache {
        TextForRenderingCacheKey key;
        Utf16String text_for_rendering;
        mutable OwnPtr<Unicode::Segmenter> grapheme_segmenter;
        mutable OwnPtr<Unicode::Segmenter> line_segmenter;
        mutable Optional<ChunkCacheEntry> chunk_cache;
    };

    TextForRenderingCacheKey create_text_for_rendering_cache_key() const;
    Utf16String compute_text_for_rendering(TextForRenderingCacheKey const&) const;
    TextDependentCache const& ensure_text_dependent_cache() const;

    mutable Optional<TextDependentCache> m_text_dependent_cache;
};

class GeneratedTextNode final : public TextNode {
    LAYOUT_NODE(GeneratedTextNode, TextNode);

public:
    GeneratedTextNode(DOM::Document&, Utf16String);
    virtual ~GeneratedTextNode() override;

    virtual DOM::Text const* dom_text() const override { return nullptr; }
    virtual Utf16String const& text() const override { return m_text; }

private:
    virtual DOM::Element const* parent_element_for_text_transform() const override;
    virtual bool is_password_input() const override { return false; }

    Utf16String m_text;
};

class TextSliceNode final : public TextNode {
    LAYOUT_NODE(TextSliceNode, TextNode);

public:
    TextSliceNode(DOM::Document&, DOM::Text&, AttachToDOMNode, size_t dom_start_offset, size_t dom_length);
    virtual ~TextSliceNode() override;

    virtual size_t dom_start_offset() const override { return m_dom_start_offset; }
    virtual size_t dom_length() const override { return m_dom_length_in_code_units; }

    // Only meaningful on a remainder slice. Returns the first-letter slice that renders the leading
    // sub-range of the same DOM::Text, or nullptr if first-letter is not active for this DOM::Text.
    TextSliceNode const* first_letter_slice() const { return m_first_letter_slice.ptr(); }
    TextSliceNode* first_letter_slice() { return m_first_letter_slice.ptr(); }

    void set_first_letter_slice(TextSliceNode& slice) { m_first_letter_slice = slice; }

private:
    virtual bool is_text_slice_node() const override { return true; }

    size_t m_dom_start_offset { 0 };
    size_t m_dom_length_in_code_units { 0 };
    WeakPtr<TextSliceNode> m_first_letter_slice;
};

template<>
inline bool Node::fast_is<TextNode>() const { return is_text_node(); }

template<>
inline bool Node::fast_is<TextSliceNode>() const { return is_text_slice_node(); }

}
