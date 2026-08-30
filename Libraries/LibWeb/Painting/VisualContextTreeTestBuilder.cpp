/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/VisualContextTreeTestBuilder.h>

namespace Web::Painting {

VisualContextTreeTestBuilder::VisualContextTreeTestBuilder()
    : m_builder(Layout::RustFFI::visual_context_tree_test_builder_create())
{
}

VisualContextTreeTestBuilder::~VisualContextTreeTestBuilder()
{
    Layout::RustFFI::visual_context_tree_test_builder_destroy(m_builder);
}

SpatialNodeIndex VisualContextTreeTestBuilder::append_transform(SpatialNodeIndex parent, Gfx::FloatMatrix4x4 const& matrix, Gfx::FloatPoint origin)
{
    return SpatialNodeIndex { Layout::RustFFI::visual_context_tree_test_builder_append_transform(m_builder, parent.value(), matrix, origin) };
}

SpatialNodeIndex VisualContextTreeTestBuilder::append_scroll(SpatialNodeIndex parent)
{
    return SpatialNodeIndex { Layout::RustFFI::visual_context_tree_test_builder_append_scroll(m_builder, parent.value()) };
}

SpatialNodeIndex VisualContextTreeTestBuilder::append_sticky(SpatialNodeIndex parent, StickyConstraints const& constraints)
{
    Layout::RustFFI::FfiTestStickyConstraints ffi_constraints {
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
    return SpatialNodeIndex { Layout::RustFFI::visual_context_tree_test_builder_append_sticky(m_builder, parent.value(), ffi_constraints) };
}

FrameNodeIndex VisualContextTreeTestBuilder::append_clip_frame(FrameNodeIndex parent, SpatialNodeIndex spatial, Gfx::FloatRect rect, Gfx::CornerRadii corner_radii, ClipMode mode)
{
    return FrameNodeIndex { Layout::RustFFI::visual_context_tree_test_builder_append_clip_frame(m_builder, parent.value(), spatial.value(), rect, corner_radii, mode) };
}

FrameNodeIndex VisualContextTreeTestBuilder::append_clip_path_frame(FrameNodeIndex parent, SpatialNodeIndex spatial, Gfx::Path const& path, Gfx::IntRect bounding_rect, Gfx::WindingRule fill_rule)
{
    auto path_bytes = path.serialize_to_bytes();
    return FrameNodeIndex { Layout::RustFFI::visual_context_tree_test_builder_append_clip_path_frame(m_builder, parent.value(), spatial.value(), path_bytes.data(), path_bytes.size(), bounding_rect, fill_rule) };
}

FrameNodeIndex VisualContextTreeTestBuilder::append_effects_frame(FrameNodeIndex parent, SpatialNodeIndex spatial, float opacity, Gfx::CompositingAndBlendingOperator blend_mode)
{
    return FrameNodeIndex { Layout::RustFFI::visual_context_tree_test_builder_append_effects_frame(m_builder, parent.value(), spatial.value(), opacity, blend_mode) };
}

AccumulatedVisualContextTree VisualContextTreeTestBuilder::finish_with_version(u64 version)
{
    VERIFY(m_builder);
    Layout::RustFFI::visual_context_tree_test_builder_set_version(m_builder, version);
    return finish();
}

AccumulatedVisualContextTree VisualContextTreeTestBuilder::finish()
{
    VERIFY(m_builder);
    auto const* retained_tree = Layout::RustFFI::visual_context_tree_test_builder_finish(exchange(m_builder, nullptr));
    return AccumulatedVisualContextTree::adopt_rust_handle(retained_tree);
}

}
