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
#include <LibWeb/Layout/RenderedText.h>

namespace Web::Layout {

// Non-spec infrastructure for text-fragment matching. Matching policy is deliberately left to
// callers.
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
    };

    struct Block {
        Utf16String text;
        Vector<Segment> segments;
        Vector<RenderedTextOffset> offset_mapping;
    };

    static SearchableText collect(Viewport&);

    Vector<Block> const& blocks() const { return m_blocks; }
    Optional<DOM::BoundaryPoint> boundary_point_at_offset(Block const&, size_t offset, bool is_end) const;

private:
    explicit SearchableText(Vector<Block> blocks)
        : m_blocks(move(blocks))
    {
    }

    Vector<Block> m_blocks;
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
