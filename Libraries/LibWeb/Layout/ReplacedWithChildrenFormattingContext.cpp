/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/ReplacedWithChildrenFormattingContext.h>

namespace Web::Layout {

ReplacedWithChildrenFormattingContext::ReplacedWithChildrenFormattingContext(LayoutState& state, LayoutMode layout_mode, Box const& box, FormattingContext* parent)
    : FormattingContext(Type::ReplacedWithChildren, layout_mode, state, box, parent)
{
}

void ReplacedWithChildrenFormattingContext::run(AvailableSpace const& available_space)
{
    auto& root_state = m_state.get_mutable(context_box());
    auto content_width = root_state.content_width();

    // Mark the replaced element as having definite dimensions when the parent FC has
    // computed them from intrinsic size, so children with percentage sizes can resolve.
    auto natural_size = context_box().natural_size();
    if (natural_size.has_width())
        root_state.set_has_definite_width(true);
    if (natural_size.has_height())
        root_state.set_has_definite_height(true);

    // For height, use the parent-set content height if it's been resolved (e.g. explicit height
    // or intrinsic height), otherwise use the available space from the parent formatting context.
    auto child_available_height = root_state.has_definite_height()
        ? AvailableSize::make_definite(root_state.content_height())
        : available_space.height;

    auto child_available_space = AvailableSpace(
        AvailableSize::make_definite(content_width),
        child_available_height);

    // The TreeBuilder wraps shadow DOM children in an anonymous BlockContainer.
    // Delegate layout to a BFC for that wrapper.
    auto const* wrapper = context_box().first_child_of_type<BlockContainer>();
    if (!wrapper)
        return;

    auto& wrapper_state = m_state.get_mutable(*wrapper);
    wrapper_state.set_content_width(content_width);
    wrapper_state.set_content_offset({ 0, 0 });

    auto bfc = make<BlockFormattingContext>(m_state, m_layout_mode, *wrapper, this);
    bfc->run(child_available_space);

    m_automatic_content_width = content_width;
    m_automatic_content_height = bfc->automatic_content_height();

    wrapper_state.set_content_height(m_automatic_content_height);

    bfc->parent_context_did_dimension_child_root_box();
}

CSSPixels ReplacedWithChildrenFormattingContext::automatic_content_width() const
{
    return m_automatic_content_width;
}

CSSPixels ReplacedWithChildrenFormattingContext::automatic_content_height() const
{
    return m_automatic_content_height;
}

void ReplacedWithChildrenFormattingContext::parent_context_did_dimension_child_root_box()
{
    layout_absolutely_positioned_children();
}

}
