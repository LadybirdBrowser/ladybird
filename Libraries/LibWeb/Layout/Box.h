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

    Box(DOM::Document&, GC::Ptr<DOM::Node>, CSS::LayoutStyle, RustFFI::NodeKind = RustFFI::NodeKind::Box);
    Box(DOM::Document&, BindToPreparedArenaSlot, RustFFI::NodeSlotId, RustFFI::NodeKind);

private:
    CSS::SizeWithAspectRatio compute_auto_content_box_size() const;

    virtual bool is_box() const final { return true; }

    OwnPtr<ImageProvider> m_owned_image_provider;
};

template<>
inline bool Node::fast_is<Box>() const { return is_box(); }

}
