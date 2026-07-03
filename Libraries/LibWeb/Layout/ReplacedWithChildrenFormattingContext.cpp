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

void ReplacedWithChildrenFormattingContext::run(LayoutInput const& layout_input)
{
    auto& root_state = m_state.get_mutable(context_box());
    auto content_inline_size = root_state.content_inline_size();

    // The parent FC has already resolved this replaced box's used size (natural size, explicit size, or the default
    // object size), so both dimensions are definite for its children — shadow content sized to fill
    // (e.g. width/height: 100%) resolves against them.
    root_state.set_has_definite_inline_size(true);
    root_state.set_has_definite_block_size(true);

    auto child_available_space = AvailableSpace(
        AvailableSize::make_definite(content_inline_size),
        AvailableSize::make_definite(root_state.content_block_size()));

    // The TreeBuilder wraps shadow DOM children in an anonymous BlockContainer.
    // Delegate layout to a BFC for that wrapper.
    auto const* wrapper = context_box().first_child_of_type<BlockContainer>();
    if (!wrapper)
        return;

    auto wrapper_constraints = constraints_for_child_context(root_state, layout_input.containing_block_constraints);
    auto& wrapper_state = m_state.create(*wrapper, wrapper_constraints.percentage_basis_inline_size, wrapper_constraints.percentage_basis_block_size);
    wrapper_state.set_content_inline_size(content_inline_size);

    auto bfc = make<BlockFormattingContext>(m_state, m_layout_mode, *wrapper, this);
    bfc->run(LayoutInput { child_available_space, wrapper_constraints });

    m_automatic_content_inline_size = content_inline_size;
    m_automatic_content_block_size = bfc->automatic_content_block_size();

    wrapper_state.set_content_block_size(m_automatic_content_block_size);

    place_child(*wrapper, { 0, 0 });

    bfc->parent_context_did_dimension_child_root_box();
}

CSSPixels ReplacedWithChildrenFormattingContext::automatic_content_inline_size() const
{
    return m_automatic_content_inline_size;
}

CSSPixels ReplacedWithChildrenFormattingContext::automatic_content_block_size() const
{
    return m_automatic_content_block_size;
}

void ReplacedWithChildrenFormattingContext::parent_context_did_dimension_child_root_box()
{
    layout_absolutely_positioned_children();
}

}
