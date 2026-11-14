/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a tree build reads of an element's style record, read from its groups in one go.

use crate::css::computed_value_views::ComputedValuesView;
use crate::layout::FfiDisplay;
use crate::layout::node_data::NodeKind;

/// See the module documentation.
#[derive(Clone, Copy)]
pub(crate) struct StyleBoxSummary {
    /// The computed `display`.
    pub(crate) display: FfiDisplay,
    /// The computed `content-visibility`.
    pub(crate) content_visibility: u8,
    /// The computed `appearance`.
    pub(crate) appearance: u8,
    /// See [`super::box_facts_of_style`].
    pub(crate) box_facts: u32,
    /// See [`crate::layout::layout_node_arena::planned_record_style_facts`].
    record_style_facts: u32,
    /// Whether the style names a counter in `counter-reset`, `counter-increment` or `counter-set`, or is a list
    /// item's, which increments `list-item`.
    pub(crate) may_change_counters: bool,
    /// Whether the style replaces the element's contents with a single image.
    pub(crate) content_is_single_image: bool,
    /// See [`crate::painting::style_queries::has_style_containment`].
    pub(crate) has_style_containment: bool,
}

impl StyleBoxSummary {
    /// The summary of the style `style` views.
    pub(crate) fn of(style: ComputedValuesView<'_>) -> Self {
        let display = style.display();
        Self {
            display,
            content_visibility: style.content_visibility(),
            appearance: style.appearance(),
            box_facts: super::box_facts_of_style(style),
            record_style_facts: crate::layout::layout_node_arena::planned_record_style_facts(style),
            may_change_counters: !style.counter_properties_are_none() || display.is_list_item(),
            content_is_single_image: style.content_is_single_image(),
            has_style_containment: crate::painting::style_queries::has_style_containment(style),
        }
    }

    /// The facts of `planned_style_fact` a row of `kind` takes from the style.
    pub(crate) fn style_facts(&self, kind: NodeKind) -> u32 {
        crate::layout::layout_node_arena::planned_style_facts_for_kind(self.record_style_facts, kind)
    }
}
