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
    // The provider of the image the box shows: the one it owns once it is handed it, else its element's, which a box
    // that owns a provider it has yet to be handed may not have.
    ImageProvider const* image_provider_if_any() const;
    ImageProvider& image_provider()
    {
        return const_cast<ImageProvider&>(const_cast<Box const&>(*this).image_provider());
    }
    // The provider a box owns belongs to the layout arena's host tables, which delete it with the
    // box's row.
    void set_owned_image_provider(NonnullOwnPtr<ImageProvider>);

    void set_replaced_box_can_have_children(bool value) { set_flag(RustFFI::NodeFlag::ReplacedBoxCanHaveChildren, value); }

    virtual ~Box() override;

    Box(DOM::Document&, BindToPreparedArenaSlot, Compositing::RustFFI::NodeSlotId, RustFFI::NodeKind);

private:
    virtual bool is_box() const final { return true; }

    ImageProvider* owned_image_provider() const;
};

template<>
inline bool Node::fast_is<Box>() const { return is_box(); }

}
