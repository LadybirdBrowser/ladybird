/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibCompositing/RustFFI.h>
#include <LibCompositing/TestBuilderRustFFI.h>

namespace Compositing {

VisualContextTreeTestBuilder::VisualContextTreeTestBuilder()
    : m_builder(Compositing::RustFFI::visual_context_tree_test_builder_create())
{
}

VisualContextTreeTestBuilder::~VisualContextTreeTestBuilder()
{
    Compositing::RustFFI::visual_context_tree_test_builder_destroy(m_builder);
}

EffectNodeIndex VisualContextTreeTestBuilder::append_image_filter(u64 image_id, Gfx::IntRect rect)
{
    return EffectNodeIndex { RustFFI::visual_context_tree_test_builder_append_image_filter(m_builder, image_id, rect) };
}

NonnullRefPtr<DisplayList> VisualContextTreeTestBuilder::filled_rect_display_list(AccumulatedVisualContextTree const& tree, ContextRef context, Gfx::IntRect rect, Gfx::Color color)
{
    return DisplayList::adopt_rust_command_storage(tree, RustFFI::visual_context_tree_test_builder_filled_rect(rect, color, context));
}

NonnullRefPtr<DisplayList> VisualContextTreeTestBuilder::rounded_rect_ring_display_list(AccumulatedVisualContextTree const& tree, ContextRef context, FillRoundedRectRing const& ring)
{
    return DisplayList::adopt_rust_command_storage(tree, RustFFI::visual_context_tree_test_builder_rounded_rect_ring(ring, context));
}

AccumulatedVisualContextTree VisualContextTreeTestBuilder::finish()
{
    VERIFY(m_builder);
    auto const* retained_tree = Compositing::RustFFI::visual_context_tree_test_builder_finish(exchange(m_builder, nullptr));
    return AccumulatedVisualContextTree::adopt_rust_handle(retained_tree);
}

}
