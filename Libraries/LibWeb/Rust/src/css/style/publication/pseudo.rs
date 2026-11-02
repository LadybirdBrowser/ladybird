/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

impl RetainedState {
    /// Remove backdrop and marker kinds from the candidate pseudo-element bitmask `kinds`
    /// when they cannot generate a box, regardless of their matching rules.
    /// `marker_is_live` includes any originating or pseudo-element list item the caller must consider.
    pub(super) fn filter_non_generating_pseudo_kinds(
        &self,
        node: StyleNodeID,
        mut kinds: u64,
        marker_is_live: bool,
    ) -> u64 {
        if self.projection.adjustment_facts(node) & bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER == 0 {
            kinds &= !(1 << pseudo_kind::BACKDROP);
        }
        if !marker_is_live {
            kinds &= !(1 << pseudo_kind::MARKER);
        }
        kinds
    }

    /// The pseudo-element kind an element stands for (the element a `::placeholder` or a slider part
    /// is), whose style is that pseudo-element's, cascaded from the host's rules, and the shadow
    /// host it stands for it on. The host publishes the kind with the element's facts, and an
    /// element in no shadow tree stands for nothing.
    pub(in crate::css::style) fn backed_host_pseudo_element(&self, node: StyleNodeID) -> Option<(u8, StyleNodeID)> {
        let kind = self.projection.associated_pseudo_kind(node)?;
        Some((kind, self.projection.shadow_host_of(node)?))
    }
}
