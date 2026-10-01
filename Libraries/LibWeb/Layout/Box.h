/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NumericLimits.h>
#include <AK/OwnPtr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

class WEB_API Box : public NodeWithStyle {
public:
    // A partial relayout boundary is a box whose subtree can be re-laid out in
    // isolation: its own used size and position are guaranteed not to change
    // when layout is invalidated somewhere inside its subtree.
    bool is_partial_relayout_boundary() const;

    ImageProvider const& image_provider() const;
    ImageProvider& image_provider()
    {
        return const_cast<ImageProvider&>(const_cast<Box const&>(*this).image_provider());
    }
    void set_owned_image_provider(NonnullOwnPtr<ImageProvider>);
    void notify_owned_image_provider_of_detach();

    void set_replaced_box_can_have_children(bool value) { set_flag(RustFFI::NodeFlag::ReplacedBoxCanHaveChildren, value); }

    virtual ~Box() override;

    Box(DOM::Document&, GC::Ptr<DOM::Node>, CSS::LayoutStyle, RustFFI::NodeKind = RustFFI::NodeKind::Box);
    Box(DOM::Document&, BindToPreparedArenaSlot, Compositing::RustFFI::NodeSlotId, RustFFI::NodeKind);

private:
    virtual bool is_box() const final { return true; }

    OwnPtr<ImageProvider> m_owned_image_provider;
};

template<>
inline bool Node::fast_is<Box>() const { return is_box(); }

}
