/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class WEB_API StackingContext final
    : public RefCounted<StackingContext>
    , public Weakable<StackingContext> {
    friend class ViewportPaintable;

public:
    static NonnullRefPtr<StackingContext> create(PaintableBox&, RefPtr<StackingContext> parent, size_t index_in_tree_order);

    RefPtr<StackingContext> parent() { return m_parent.strong_ref(); }
    RefPtr<StackingContext const> parent() const { return m_parent.strong_ref(); }

    PaintableBox const& paintable_box() const
    {
        auto paintable = m_paintable.strong_ref();
        VERIFY(paintable);
        return *paintable;
    }

    enum class StackingContextPaintPhase {
        BackgroundAndBorders,
        Floats,
        BackgroundAndBordersForInlineLevelAndReplaced,
        Foreground,
    };

    static void paint_node_as_stacking_context(Paintable const&, DisplayListRecordingContext&);
    static void paint_descendants(DisplayListRecordingContext&, Paintable const&, StackingContextPaintPhase);
    static void paint_svg(DisplayListRecordingContext&, PaintableBox const&, PaintPhase);
    void paint(DisplayListRecordingContext&) const;

    [[nodiscard]] TraversalDecision hit_test(CSSPixelPoint, HitTestType, Function<TraversalDecision(HitTestResult)> const& callback) const;

    void dump(StringBuilder&, int indent = 0) const;

    void sort();

    void set_last_paint_generation_id(u64 generation_id);

private:
    StackingContext(PaintableBox&, RefPtr<StackingContext> parent, size_t index_in_tree_order);

    WeakPtr<PaintableBox> m_paintable;
    WeakPtr<StackingContext> m_parent;
    Vector<NonnullRefPtr<StackingContext>> m_children;
    size_t m_index_in_tree_order { 0 };
    Optional<u64> m_last_paint_generation_id;

    Vector<WeakPtr<PaintableBox>> m_positioned_descendants_and_stacking_contexts_with_stack_level_0;
    Vector<WeakPtr<PaintableBox>> m_non_positioned_floating_descendants;

    static void paint_child(DisplayListRecordingContext&, StackingContext const&);
    void paint_internal(DisplayListRecordingContext&) const;
};

}
