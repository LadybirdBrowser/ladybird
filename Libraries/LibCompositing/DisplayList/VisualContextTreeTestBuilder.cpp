/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

void VisualContextTreeTestBuilder::finish()
{
    RustFFI::visual_context_tree_test_builder_finish(m_builder);
}

void VisualContextTreeTestBuilder::record_filled_rect(ContextRef context, Gfx::IntRect rect, Gfx::Color color)
{
    RustFFI::visual_context_tree_test_builder_filled_rect(m_builder, rect, color, context);
}

void VisualContextTreeTestBuilder::record_rounded_rect_ring(ContextRef context, FillRoundedRectRing const& ring)
{
    RustFFI::visual_context_tree_test_builder_rounded_rect_ring(m_builder, ring, context);
}

void VisualContextTreeTestBuilder::replay(RustFFI::FfiRasterPainter const& painter, i64 time_nanoseconds) const
{
    RustFFI::visual_context_tree_test_builder_replay(m_builder, time_nanoseconds, &painter);
}

}
