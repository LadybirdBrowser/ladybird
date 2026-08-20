/*
 * Copyright (c) 2022-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableFragment.h>

namespace Web::Painting {

class HitTestDisplayList;
class InlinePaintable;

InlinePaintable const* nearest_self_painting_inline_box(Layout::Node const&);

class PaintableWithLines : public Paintable {
public:
    static NonnullRefPtr<PaintableWithLines> create(Layout::BlockContainer const&);
    virtual ~PaintableWithLines() override;

    virtual void reset_for_relayout() override;

    Vector<PaintableFragment> const& fragments() const { return m_fragments; }
    u32 index_of_fragment(PaintableFragment const& fragment) const
    {
        VERIFY(&fragment >= m_fragments.data() && &fragment < m_fragments.data() + m_fragments.size());
        return static_cast<u32>(&fragment - m_fragments.data());
    }
    Vector<PaintableFragment>& fragments() { return m_fragments; }

    Vector<LineRecord> const& lines() const { return m_lines; }
    void set_lines(Vector<LineRecord> lines) { m_lines = move(lines); }

    Vector<InlineBoxPiece> const& inline_box_pieces() const { return m_inline_box_pieces; }
    Vector<InlineBoxPiece>& inline_box_pieces() { return m_inline_box_pieces; }
    void set_inline_box_pieces(Vector<InlineBoxPiece> pieces) { m_inline_box_pieces = move(pieces); }

    void add_fragment(PaintableFragment::Fields fields)
    {
        m_fragments.empend(*this, move(fields));
    }
    void reset_fragment_selection_states()
    {
        for (auto& fragment : m_fragments)
            fragment.set_selection_state(SelectionState::None);
    }

    void assign_inline_box_geometry();

    Vector<PaintableFragment::FragmentSpan, 4> render_spans_for_paint(u64 paint_generation_id, ReadonlySpan<u32> owned_fragment_indices) const;

    struct CaretPaint {
        CSSPixelRect rect;
        Color color;
    };
    Optional<CaretPaint> resolve_caret_paint(InlinePaintable const* owner) const;

    // Caret rect for a cursor parked on this paintable's DOM node at the given child offset, e.g. on an empty line
    // rendered by a <br> child or in an empty editable element.
    CSSPixelRect caret_rect_for_child_offset(size_t offset) const;

    struct EmptyLineCaretItem {
        bool is_line_break_boundary { false };
        size_t caret_offset { 0 };
        size_t line_index { 0 };
        CSSPixelRect rect;
    };
    void for_each_empty_line_caret_item(Function<void(EmptyLineCaretItem const&)> const&) const;

protected:
    PaintableWithLines(Layout::BlockContainer const&);

private:
    Optional<PaintableFragment const&> fragment_at_position(DOM::Position const&) const;
    Optional<CSSPixelRect> empty_line_caret_rect(DOM::Position const&) const;

    // A caret target for a line box with no fragments (e.g. a blank line in a textarea).
    struct EmptyLineCaretTarget {
        size_t offset { 0 };
        size_t line_index { 0 };
        CSSPixelRect rect;
    };
    Vector<EmptyLineCaretTarget> empty_line_caret_targets() const;

    Vector<PaintableFragment> m_fragments;
    Vector<LineRecord> m_lines;
    Vector<InlineBoxPiece> m_inline_box_pieces;

    mutable Optional<u64> m_text_fragment_properties_paint_generation_id;
};

}
