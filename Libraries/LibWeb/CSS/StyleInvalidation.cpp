/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/CSS/StyleInvalidation.h>

namespace Web::CSS {

RequiredInvalidationAfterStyleChange decode_style_invalidation(u32 packed)
{
    using enum StyleEngineFFI::FfiStyleInvalidationField;
    static_assert(ComputedValues::inherited_style_group_count <= 7);
    RequiredInvalidationAfterStyleChange result;
    result.ensure_at_least(static_cast<InvalidationLevel>(packed & to_underlying(LevelMask)));
    result.ensure_at_least(static_cast<AccumulatedVisualContextInvalidation>((packed >> to_underlying(VisualContextShift)) & to_underlying(LevelMask)));
    if (result.needs_layout_tree_rebuild())
        result.set_layout_tree_rebuild_root(static_cast<LayoutTreeRebuildRoot>((packed >> to_underlying(RebuildRootShift)) & to_underlying(LevelMask)));
    if (packed & to_underlying(RebuildStackingContext))
        result.set_needs_stacking_context_tree_rebuild();
    if (packed & to_underlying(RecalculateScrollableOverflow))
        result.set_needs_scrollable_overflow_recalculation();
    result.needs_scroll_container_resnap = packed & to_underlying(ResnapScrollContainer);
    result.recompute_descendant_styles = packed & to_underlying(RecomputeDescendants);
    auto inherited_groups = static_cast<u8>((packed >> to_underlying(InheritedGroupsShift)) & to_underlying(InheritedGroupsMask));
    for (u8 group = 0; group < ComputedValues::inherited_style_group_count; ++group) {
        if (inherited_groups & (1 << group))
            result.mark_inherited_style_group_changed(group);
    }
    result.changes_containing_block_establishment = packed & to_underlying(ChangesContainingBlock);
    result.repaint_propagated_text_decorations = packed & to_underlying(RepaintTextDecorations);
    result.non_inherited_property_inheritance_sources_changed = packed & to_underlying(NonInheritedInheritanceSource);
    return result;
}

}
