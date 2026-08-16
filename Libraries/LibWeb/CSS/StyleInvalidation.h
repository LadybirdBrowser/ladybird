/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// Ordered by increasing severity. A higher level always implies all lower levels.
enum class InvalidationLevel : u8 {
    None,
    Repaint,
    Relayout,
    RebuildLayoutTree,
};

// Ordered by increasing severity: Rebuild subsumes UpdateValues.
enum class AccumulatedVisualContextInvalidation : u8 {
    None,
    UpdateValues,
    Rebuild,
};

enum class LayoutTreeRebuildRoot : u8 {
    Self,
    SelfUnlessDocumentElementOrBody,
    Parent,
};

struct RequiredInvalidationAfterStyleChange {
    void ensure_at_least(InvalidationLevel level)
    {
        m_level = max(m_level, level);
        if (level >= InvalidationLevel::RebuildLayoutTree)
            m_layout_tree_rebuild_root = LayoutTreeRebuildRoot::Parent;
    }
    void ensure_at_least(AccumulatedVisualContextInvalidation invalidation) { m_accumulated_visual_contexts = max(m_accumulated_visual_contexts, invalidation); }

    void set_needs_stacking_context_tree_rebuild()
    {
        m_rebuild_stacking_context_tree = true;
        ensure_at_least(InvalidationLevel::Repaint);
    }

    // NB: Deliberately does not imply repaint: whether anything needs repainting is only known
    //     after the overflow has actually been re-measured and turned out to have changed.
    void set_needs_scrollable_overflow_recalculation() { m_needs_scrollable_overflow_recalculation = true; }

    [[nodiscard]] bool needs_repaint() const { return m_level >= InvalidationLevel::Repaint; }
    [[nodiscard]] bool needs_relayout() const { return m_level >= InvalidationLevel::Relayout; }
    [[nodiscard]] bool needs_layout_tree_rebuild() const { return m_level >= InvalidationLevel::RebuildLayoutTree; }
    [[nodiscard]] LayoutTreeRebuildRoot layout_tree_rebuild_root() const
    {
        VERIFY(needs_layout_tree_rebuild());
        return m_layout_tree_rebuild_root;
    }
    [[nodiscard]] bool needs_stacking_context_tree_rebuild() const { return m_rebuild_stacking_context_tree; }
    // NB: A pending relayout re-measures all scrollable overflow anyway, so this reports true only
    //     when the recalculation has to run as a separate step.
    [[nodiscard]] bool needs_scrollable_overflow_recalculation() const { return m_needs_scrollable_overflow_recalculation && !needs_relayout(); }
    [[nodiscard]] AccumulatedVisualContextInvalidation accumulated_visual_contexts() const { return m_accumulated_visual_contexts; }

    // The element's change affects rule matching for descendants, without necessarily changing inherited style.
    bool recompute_descendant_styles : 1 { false };
    // Names the inherited ComputedValues groups whose identities changed. Descendants can use the
    // exact set to avoid reading unrelated inherited groups.
    static constexpr u8 all_inherited_style_groups = (1 << 7) - 1;
    [[nodiscard]] bool inherited_style_changed() const { return m_inherited_style_groups_changed != 0; }
    [[nodiscard]] u8 inherited_style_groups_changed() const { return m_inherited_style_groups_changed; }
    void mark_inherited_style_group_changed(size_t group) { m_inherited_style_groups_changed |= 1 << group; }
    void mark_all_inherited_style_groups_changed() { m_inherited_style_groups_changed = all_inherited_style_groups; }
    // The element gained or lost a containing block for absolutely/fixed positioned
    // descendants. Containing block pointers are only recomputed by a full layout pass, so
    // partial relayout boundary qualification cannot be trusted until one runs.
    bool changes_containing_block_establishment : 1 { false };
    // A property controlling decorations originated by this box changed. Descendant boxes paint
    // the propagated decorations from these values, so their cached paint commands are stale.
    bool repaint_propagated_text_decorations : 1 { false };

    void operator|=(RequiredInvalidationAfterStyleChange const& other)
    {
        if (other.needs_layout_tree_rebuild()) {
            if (needs_layout_tree_rebuild())
                m_layout_tree_rebuild_root = max(m_layout_tree_rebuild_root, other.m_layout_tree_rebuild_root);
            else
                m_layout_tree_rebuild_root = other.m_layout_tree_rebuild_root;
        }
        m_level = max(m_level, other.m_level);
        m_accumulated_visual_contexts = max(m_accumulated_visual_contexts, other.m_accumulated_visual_contexts);
        m_rebuild_stacking_context_tree |= other.m_rebuild_stacking_context_tree;
        m_needs_scrollable_overflow_recalculation |= other.m_needs_scrollable_overflow_recalculation;
        recompute_descendant_styles |= other.recompute_descendant_styles;
        m_inherited_style_groups_changed |= other.m_inherited_style_groups_changed;
        changes_containing_block_establishment |= other.changes_containing_block_establishment;
        repaint_propagated_text_decorations |= other.repaint_propagated_text_decorations;
    }

    [[nodiscard]] bool is_none() const
    {
        return m_level == InvalidationLevel::None
            && m_accumulated_visual_contexts == AccumulatedVisualContextInvalidation::None
            && !m_needs_scrollable_overflow_recalculation
            && !recompute_descendant_styles
            && !inherited_style_changed()
            && !changes_containing_block_establishment
            && !repaint_propagated_text_decorations;
    }

    static RequiredInvalidationAfterStyleChange full()
    {
        RequiredInvalidationAfterStyleChange invalidation;
        invalidation.ensure_at_least(InvalidationLevel::RebuildLayoutTree);
        invalidation.set_needs_stacking_context_tree_rebuild();
        return invalidation;
    }

    static RequiredInvalidationAfterStyleChange rebuild_layout_tree_from(LayoutTreeRebuildRoot rebuild_root)
    {
        RequiredInvalidationAfterStyleChange invalidation;
        invalidation.m_level = InvalidationLevel::RebuildLayoutTree;
        invalidation.m_layout_tree_rebuild_root = rebuild_root;
        invalidation.set_needs_stacking_context_tree_rebuild();
        return invalidation;
    }

private:
    InvalidationLevel m_level { InvalidationLevel::None };
    AccumulatedVisualContextInvalidation m_accumulated_visual_contexts { AccumulatedVisualContextInvalidation::None };
    LayoutTreeRebuildRoot m_layout_tree_rebuild_root { LayoutTreeRebuildRoot::Parent };
    bool m_rebuild_stacking_context_tree : 1 { false };
    bool m_needs_scrollable_overflow_recalculation : 1 { false };
    u8 m_inherited_style_groups_changed { 0 };
};

RequiredInvalidationAfterStyleChange compute_property_invalidation(CSS::PropertyID, ComputedValues const& old_computed_values, ComputedValues const& new_computed_values);
RequiredInvalidationAfterStyleChange compute_property_invalidation(CSS::PropertyID, StyleValue const* old_value, StyleValue const* new_value);

}
