/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class WEB_API StackingContext {
    friend class ViewportPaintable;

public:
    StackingContext(PaintableBox&, StackingContext* parent, size_t index_in_tree_order);

    StackingContext* parent() { return m_parent; }
    StackingContext const* parent() const { return m_parent; }

    PaintableBox const& paintable_box() const { return *m_paintable; }

    enum class StackingContextPaintPhase {
        BackgroundAndBorders,
        Floats,
        BackgroundAndBordersForInlineLevelAndReplaced,
        Foreground,
        FocusAndOverlay,
    };

    static void paint_node_as_stacking_context(Paintable const&, DisplayListRecordingContext&);
    static void paint_descendants(DisplayListRecordingContext&, Paintable const&, StackingContextPaintPhase);
    static void paint_svg(DisplayListRecordingContext&, PaintableBox const&, PaintPhase);
    void paint(DisplayListRecordingContext&) const;

    [[nodiscard]] TraversalDecision hit_test(CSSPixelPoint, HitTestType, Function<TraversalDecision(HitTestResult)> const& callback) const;

    Gfx::AffineTransform affine_transform_matrix() const;

    void dump(StringBuilder&, int indent = 0) const;

    void sort();

    void set_last_paint_generation_id(u64 generation_id);

private:
    GC::Ref<PaintableBox> m_paintable;
    StackingContext* const m_parent { nullptr };
    Vector<StackingContext*> m_children;
    size_t m_index_in_tree_order { 0 };
    Optional<u64> m_last_paint_generation_id;

    Vector<GC::Ref<PaintableBox const>> m_positioned_descendants_and_stacking_contexts_with_stack_level_0;
    Vector<GC::Ref<PaintableBox const>> m_non_positioned_floating_descendants;

    static void paint_child(DisplayListRecordingContext&, StackingContext const&);
    void paint_internal(DisplayListRecordingContext&) const;
};

}
