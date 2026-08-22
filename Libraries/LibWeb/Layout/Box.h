/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NumericLimits.h>
#include <AK/OwnPtr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

struct LineBoxFragmentCoordinate {
    size_t line_box_index { 0 };
    size_t fragment_index { 0 };
};

class WEB_API Box : public NodeWithStyle {
public:
    // A partial relayout boundary is a box whose subtree can be re-laid out in
    // isolation: its own used size and position are guaranteed not to change
    // when layout is invalidated somewhere inside its subtree.
    bool is_partial_relayout_boundary() const;

    // https://www.w3.org/TR/css-images-3/#natural-dimensions
    CSS::SizeWithAspectRatio natural_size() const;

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

    ImageProvider const& image_provider() const;
    ImageProvider& image_provider()
    {
        return const_cast<ImageProvider&>(const_cast<Box const&>(*this).image_provider());
    }
    void set_owned_image_provider(NonnullOwnPtr<ImageProvider>);

    void set_replaced_box_can_have_children(bool value) { set_flag(RustFFI::NodeFlag::ReplacedBoxCanHaveChildren, value); }

    virtual ~Box() override;

    void notify_content_navigable_of_committed_viewport();
    bool has_saved_abspos_layout_inputs() const { return has_flag(RustFFI::NodeFlag::HasSavedAbsposLayoutInputs); }
    bool has_committed_fragment_link() const { return has_flag(RustFFI::NodeFlag::HasCommittedFragmentLink); }
    bool saved_abspos_cb_derives_from_own_computed_values() const { return has_flag(RustFFI::NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues); }
    bool saved_abspos_alignment_derives_from_own_computed_values() const { return has_flag(RustFFI::NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues); }

    // Whether an absolutely or fixed positioned descendant of this box has its containing
    // block outside this box's subtree, so the descendant's layout escapes the subtree.
    // Re-derived whenever containing block pointers are recomputed.
    bool abspos_descendant_escapes() const { return has_flag(RustFFI::NodeFlag::AbsposDescendantEscapes); }
    void set_abspos_descendant_escapes(bool value) { set_flag(RustFFI::NodeFlag::AbsposDescendantEscapes, value); }

    void set_default_scroll_shift(WeakPtr<Node> anchor, bool compensates_for_horizontal_scroll, bool compensates_for_vertical_scroll);
    Node* default_scroll_shift_anchor() const { return m_default_scroll_shift_anchor.ptr(); }
    bool compensates_for_horizontal_scroll() const { return has_flag(RustFFI::NodeFlag::CompensatesForHorizontalScroll); }
    bool compensates_for_vertical_scroll() const { return has_flag(RustFFI::NodeFlag::CompensatesForVerticalScroll); }

    void reset_cached_intrinsic_sizes()
    {
        auto& epoch = node_data().intrinsic_cache_epoch;
        if (epoch != NumericLimits<u16>::max())
            ++epoch;
    }

    Box(DOM::Document&, GC::Ptr<DOM::Node>, CSS::LayoutStyle, RustFFI::NodeKind = RustFFI::NodeKind::Box);

private:
    CSS::SizeWithAspectRatio compute_auto_content_box_size() const;

    virtual bool is_box() const final { return true; }

    WeakPtr<Node> m_default_scroll_shift_anchor;
    OwnPtr<ImageProvider> m_owned_image_provider;
};

template<>
inline bool Node::fast_is<Box>() const { return is_box(); }

}
