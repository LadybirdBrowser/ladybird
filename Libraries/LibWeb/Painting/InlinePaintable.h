/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/PaintableWithLines.h>

namespace Web::Painting {

// The paintable of an inline box that participates in an inline formatting context (e.g. a
// <span>, possibly fragmented across lines). It owns no fragments: the fragments and the
// per-line pieces it paints live on the containing block's PaintableWithLines, and its own
// geometry is the union of those pieces.
class InlinePaintable final : public Paintable {
public:
    static NonnullRefPtr<InlinePaintable> create(Layout::NodeWithStyleAndBoxModelMetrics const&);
    virtual ~InlinePaintable() override;
    virtual StringView class_name() const override { return "InlinePaintable"sv; }

    virtual void reset_for_relayout() override;

    PaintableWithLines const* inline_root() const;

    void set_piece_indices(Vector<u32> piece_indices) { m_piece_indices = move(piece_indices); }

    template<typename Callback>
    void for_each_piece(Callback callback) const
    {
        auto const* root = inline_root();
        if (!root)
            return;
        auto const& pieces = root->inline_box_pieces();
        for (auto piece_index : m_piece_indices)
            callback(pieces[piece_index]);
    }

    template<typename Callback>
    void for_each_piece_fragment(Callback callback) const
    {
        auto const* root = inline_root();
        if (!root)
            return;
        auto const& fragments = root->fragments();
        for_each_piece([&](InlineBoxPiece const& piece) {
            for (u32 fragment_index = piece.first_fragment_index; fragment_index < piece.first_fragment_index + piece.fragment_count; ++fragment_index)
                callback(piece, fragments[fragment_index]);
        });
    }

    void set_local_box_unions(CSSPixelRect padding_box_union, CSSPixelRect border_box_union)
    {
        m_local_padding_box_union = padding_box_union;
        m_local_border_box_union = border_box_union;
    }

    // Whether this box paints its own foreground (fragments and caret) instead of the
    // containing block: it forms a group that content must be recorded inside.
    bool is_self_painting() const { return has_stacking_context() || is_positioned(); }

    // Only meaningful while is_self_painting(); assigned by the containing block's
    // PaintableWithLines::assign_fragment_ownership().
    void set_fragment_ownership_filter(PaintableWithLines::FragmentOwnershipFilter filter) { m_fragment_ownership_filter = move(filter); }
    PaintableWithLines::FragmentOwnershipFilter const& fragment_ownership_filter() const { return m_fragment_ownership_filter; }

    CSSPixelRect absolute_piece_border_box_rect(InlineBoxPiece const&) const;
    CSSPixelRect piece_padding_box_rect(InlineBoxPiece const&, CSSPixelRect const& border_box_rect) const;
    CSSPixelRect piece_content_box_rect(InlineBoxPiece const&, CSSPixelRect const& border_box_rect) const;
    virtual CSSPixelPoint box_type_agnostic_position() const override;

    bool has_content() const;

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;
    virtual void record_hit_test_items(DisplayListRecordingContext&, PaintPhase) const override;
    virtual bool foreground_paints_descendant_content() const override { return true; }
    virtual void set_needs_repaint(InvalidateDisplayList = InvalidateDisplayList::Yes) override;

private:
    explicit InlinePaintable(Layout::NodeWithStyleAndBoxModelMetrics const&);

    [[nodiscard]] virtual bool is_inline_paintable() const final { return true; }

    virtual CSSPixelRect compute_absolute_padding_box_rect() const override;
    virtual CSSPixelRect compute_absolute_border_box_rect() const override;

    BorderRadiiData piece_border_radii_data(InlineBoxPiece const&) const;
    bool has_content_pieces() const;
    void paint_empty_editable_cursor(DisplayListRecordingContext&) const;

    Vector<u32> m_piece_indices;
    CSSPixelRect m_local_padding_box_union;
    CSSPixelRect m_local_border_box_union;
    PaintableWithLines::FragmentOwnershipFilter m_fragment_ownership_filter;
};

template<>
inline bool Paintable::fast_is<InlinePaintable>() const { return is_inline_paintable(); }

}
