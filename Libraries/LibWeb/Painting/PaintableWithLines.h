/*
 * Copyright (c) 2022-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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
    virtual StringView class_name() const override { return "PaintableWithLines"sv; }

    virtual void reset_for_relayout() override;

    Vector<PaintableFragment> const& fragments() const { return m_fragments; }
    Vector<PaintableFragment>& fragments() { return m_fragments; }

    Vector<LineRecord> const& lines() const { return m_lines; }
    void set_lines(Vector<LineRecord> lines) { m_lines = move(lines); }

    Vector<InlineBoxPiece> const& inline_box_pieces() const { return m_inline_box_pieces; }
    Vector<InlineBoxPiece>& inline_box_pieces() { return m_inline_box_pieces; }
    void set_inline_box_pieces(Vector<InlineBoxPiece> pieces) { m_inline_box_pieces = move(pieces); }

    void add_fragment(Layout::LineBoxFragment const& fragment, u32 line_index)
    {
        m_fragments.empend(*this, fragment, line_index);
    }
    void reset_fragment_selection_states()
    {
        for (auto& fragment : m_fragments)
            fragment.set_selection_state(SelectionState::None);
    }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;
    virtual void record_hit_test_items(DisplayListRecordingContext&, PaintPhase) const override;
    virtual bool foreground_paints_descendant_content() const override { return true; }
    static void paint_text_fragment_debug_highlight(DisplayListRecordingContext&, PaintableFragment const&);

    void assign_inline_box_geometry();

    struct FragmentRange {
        u32 begin { 0 };
        u32 end { 0 };
    };
    struct FragmentOwnershipFilter {
        bool include_everything { false };
        // Sorted by begin and disjoint, so ownership of ascending fragment indices resolves
        // with monotonically advancing cursors.
        Vector<FragmentRange, 4> included;
        Vector<FragmentRange, 4> excluded;

        template<typename Callback>
        void for_each_owned_fragment_index(size_t fragment_count, Callback const& callback) const
        {
            size_t excluded_cursor = 0;
            auto is_excluded = [&](size_t index) {
                while (excluded_cursor < excluded.size() && excluded[excluded_cursor].end <= index)
                    ++excluded_cursor;
                return excluded_cursor < excluded.size() && index >= excluded[excluded_cursor].begin;
            };
            auto visit_range = [&](size_t begin, size_t end) {
                for (size_t index = begin; index < end; ++index) {
                    if (!is_excluded(index))
                        callback(index);
                }
            };
            if (include_everything) {
                visit_range(0, fragment_count);
                return;
            }
            for (auto const& range : included)
                visit_range(range.begin, range.end);
        }
    };

    // Whether a box is self-painting depends on the stacking context tree, so this runs
    // whenever that tree is rebuilt.
    void assign_fragment_ownership();

    void paint_fragments_foreground(DisplayListRecordingContext&, FragmentOwnershipFilter const&) const;

    // Paints the caret when it sits in a fragment owned by `owner`; the block itself
    // (owner == nullptr) also handles blank lines and empty editable elements.
    void paint_cursor(DisplayListRecordingContext&, InlinePaintable const* owner) const;

    // Caret rect for a cursor parked on this paintable's DOM node at the given child offset, e.g. on an empty line
    // rendered by a <br> child or in an empty editable element.
    CSSPixelRect caret_rect_for_child_offset(size_t offset) const;

protected:
    PaintableWithLines(Layout::BlockContainer const&);

private:
    [[nodiscard]] virtual bool is_paintable_with_lines() const final { return true; }

    Optional<PaintableFragment const&> fragment_at_position(DOM::Position const&) const;
    Optional<CSSPixelRect> empty_line_caret_rect(DOM::Position const&) const;

    // A caret target for a line box with no fragments (e.g. a blank line in a textarea).
    struct EmptyLineCaretTarget {
        size_t offset { 0 };
        size_t line_index { 0 };
        CSSPixelRect rect;
    };
    Vector<EmptyLineCaretTarget> empty_line_caret_targets() const;
    void record_empty_line_caret_items(HitTestDisplayList&, VisualContextIndex) const;

    Vector<PaintableFragment> m_fragments;
    Vector<LineRecord> m_lines;
    Vector<InlineBoxPiece> m_inline_box_pieces;

    FragmentOwnershipFilter m_fragment_ownership_filter {
        .include_everything = true,
        .included = {},
        .excluded = {},
    };

    mutable Optional<u64> m_text_fragment_properties_paint_generation_id;
};

}
