/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NumericLimits.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

enum class RequireExistingPaintable : u8 {
    No,
    Yes,
};

struct LineBoxFragmentCoordinate {
    size_t line_box_index { 0 };
    size_t fragment_index { 0 };
};

class WEB_API Box : public NodeWithStyle {
    LAYOUT_NODE(Box, NodeWithStyle);

public:
    RefPtr<Painting::Paintable const> paintable_box() const;
    RefPtr<Painting::Paintable> paintable_box();

    // A partial relayout boundary is a box whose subtree can be re-laid out in
    // isolation: its own used size and position are guaranteed not to change
    // when layout is invalidated somewhere inside its subtree.
    bool is_partial_relayout_boundary(RequireExistingPaintable = RequireExistingPaintable::Yes) const;

    // https://www.w3.org/TR/css-images-3/#natural-dimensions
    virtual CSS::SizeWithAspectRatio natural_size() const { return {}; }

    // When computed width/height is auto, auto_content_box_size gives the fallback content-box size for
    // elements whose used size is determined by natural dimensions, attributes, or defaults other than
    // the generic UA fallback (300x150). Any returned aspect ratio comes from natural dimensions (when
    // available) or may be computed from fallback sizing. Don't confuse this with the CSS preferred
    // aspect ratio.
    CSS::SizeWithAspectRatio auto_content_box_size() const;

    // https://www.w3.org/TR/css-sizing-4/#preferred-aspect-ratio
    Optional<CSSPixelFraction> preferred_aspect_ratio() const;
    bool has_preferred_aspect_ratio() const { return preferred_aspect_ratio().has_value(); }

    RustFFI::FfiReplacedContentFacts build_replaced_content_facts_for_arena() const;

    virtual ~Box() override;

    virtual void did_set_content_size() { }

    virtual RefPtr<Painting::Paintable> create_paintable() const override;

    bool has_saved_abspos_layout_inputs() const { return has_flag(RustFFI::NodeFlag::HasSavedAbsposLayoutInputs); }
    bool saved_abspos_cb_derives_from_own_computed_values() const { return has_flag(RustFFI::NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues); }
    bool saved_abspos_alignment_derives_from_own_computed_values() const { return has_flag(RustFFI::NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues); }

    // Whether an absolutely or fixed positioned descendant of this box has its containing
    // block outside this box's subtree, so the descendant's layout escapes the subtree.
    // Re-derived whenever containing block pointers are recomputed.
    bool abspos_descendant_escapes() const { return has_flag(RustFFI::NodeFlag::AbsposDescendantEscapes); }
    void set_abspos_descendant_escapes(bool value) { set_flag(RustFFI::NodeFlag::AbsposDescendantEscapes, value); }

    void set_default_scroll_shift(WeakPtr<Node> anchor, bool compensates_for_horizontal_scroll, bool compensates_for_vertical_scroll)
    {
        m_default_scroll_shift_anchor = move(anchor);
        set_flag(RustFFI::NodeFlag::CompensatesForHorizontalScroll, compensates_for_horizontal_scroll);
        set_flag(RustFFI::NodeFlag::CompensatesForVerticalScroll, compensates_for_vertical_scroll);
    }
    Node* default_scroll_shift_anchor() const { return m_default_scroll_shift_anchor.ptr(); }
    bool compensates_for_horizontal_scroll() const { return has_flag(RustFFI::NodeFlag::CompensatesForHorizontalScroll); }
    bool compensates_for_vertical_scroll() const { return has_flag(RustFFI::NodeFlag::CompensatesForVerticalScroll); }

    void reset_cached_intrinsic_sizes()
    {
        auto& epoch = node_data().intrinsic_cache_epoch;
        if (epoch != NumericLimits<u16>::max())
            ++epoch;
    }

    Box(DOM::Document&, DOM::Node*, NonnullRefPtr<CSS::ComputedValues const>);

protected:
    virtual CSS::SizeWithAspectRatio compute_auto_content_box_size() const { return natural_size(); }

private:
    virtual bool is_box() const final { return true; }

    WeakPtr<Node> m_default_scroll_shift_anchor;
};

template<>
inline bool Node::fast_is<Box>() const { return is_box(); }

}
