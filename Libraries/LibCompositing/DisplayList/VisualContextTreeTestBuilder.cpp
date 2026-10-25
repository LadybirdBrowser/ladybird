/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCompositing/RustFFI.h>

namespace Compositing {

VisualContextTreeTestBuilder::VisualContextTreeTestBuilder()
    : m_builder(Compositing::RustFFI::visual_context_tree_test_builder_create())
{
}

VisualContextTreeTestBuilder::~VisualContextTreeTestBuilder()
{
    Compositing::RustFFI::visual_context_tree_test_builder_destroy(m_builder);
}

SpatialNodeIndex VisualContextTreeTestBuilder::append_transform(SpatialNodeIndex parent, Gfx::FloatMatrix4x4 const& matrix, Gfx::FloatPoint origin)
{
    return SpatialNodeIndex { Compositing::RustFFI::visual_context_tree_test_builder_append_transform(m_builder, parent.value(), matrix, origin) };
}

SpatialNodeIndex VisualContextTreeTestBuilder::append_scroll(SpatialNodeIndex parent)
{
    return SpatialNodeIndex { Compositing::RustFFI::visual_context_tree_test_builder_append_scroll(m_builder, parent.value()) };
}

SpatialNodeIndex VisualContextTreeTestBuilder::append_sticky(SpatialNodeIndex parent, StickyConstraints const& constraints)
{
    Compositing::RustFFI::FfiTestStickyConstraints ffi_constraints {
        .scroller = constraints.scroller.value(),
        .has_parent_sticky = constraints.parent_sticky.has_value(),
        .parent_sticky = constraints.parent_sticky.value_or(VISUAL_VIEWPORT_NODE_INDEX).value(),
        .position_relative_to_scroller = constraints.position_relative_to_scroller,
        .border_box_size = constraints.border_box_size,
        .scrollport_size = constraints.scrollport_size,
        .containing_block_region = constraints.containing_block_region,
        .needs_parent_offset_adjustment = constraints.needs_parent_offset_adjustment,
        .inset_top = constraints.inset_top,
        .inset_right = constraints.inset_right,
        .inset_bottom = constraints.inset_bottom,
        .inset_left = constraints.inset_left,
    };
    return SpatialNodeIndex { Compositing::RustFFI::visual_context_tree_test_builder_append_sticky(m_builder, parent.value(), ffi_constraints) };
}

ClipNodeIndex VisualContextTreeTestBuilder::append_clip(ClipNodeIndex parent, SpatialNodeIndex spatial, Gfx::FloatRect rect, Gfx::CornerRadii corner_radii, ClipMode mode)
{
    return ClipNodeIndex { Compositing::RustFFI::visual_context_tree_test_builder_append_clip(m_builder, parent.value(), spatial.value(), rect, corner_radii, mode) };
}

AccumulatedVisualContextTree VisualContextTreeTestBuilder::finish()
{
    VERIFY(m_builder);
    auto const* retained_tree = Compositing::RustFFI::visual_context_tree_test_builder_finish(exchange(m_builder, nullptr));
    return AccumulatedVisualContextTree::adopt_rust_handle(retained_tree);
}

}
