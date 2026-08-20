/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/Forward.h>

namespace Web::Layout {

// Non-spec infrastructure shared by find-in-page and text-fragment matching. Matching policy is
// deliberately left to callers.
class SearchableText {
public:
    enum class Searchability : u8 {
        Searchable,
        Revealable,
    };

    enum class Boundary : u8 {
        None,
        Block,
    };

    struct Segment {
        GC::Weak<DOM::Text> dom_node;
        size_t start_offset { 0 };
        Searchability searchability { Searchability::Searchable };
        Boundary boundary_before { Boundary::None };
        bool is_inert { false };
    };

    struct RenderedTextOffset {
        size_t dom_start_offset { 0 };
        size_t dom_end_offset { 0 };
    };

    struct Block {
        Utf16String text;
        Vector<Segment> segments;
        Vector<RenderedTextOffset> offset_mapping;
    };

    static SearchableText collect(Viewport&);

    Vector<Block> const& blocks() const { return m_blocks; }
    Vector<Block> const& find_in_page_blocks() const { return m_find_in_page_blocks.has_value() ? *m_find_in_page_blocks : m_blocks; }
    Optional<GC::Ref<DOM::Range>> range_from_offsets(Block const&, size_t start_offset, size_t end_offset) const;

private:
    explicit SearchableText(Vector<Block> blocks, Optional<Vector<Block>> find_in_page_blocks)
        : m_blocks(move(blocks))
        , m_find_in_page_blocks(move(find_in_page_blocks))
    {
    }

    Vector<Block> m_blocks;
    Optional<Vector<Block>> m_find_in_page_blocks;
};

class SearchableTextCache {
public:
    explicit SearchableTextCache(Viewport& viewport)
        : m_viewport(viewport)
    {
    }

    SearchableText const& get();
    void invalidate()
    {
        m_searchable_text.clear();
    }

private:
    Viewport& m_viewport;
    Optional<SearchableText> m_searchable_text;
};

}
