/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a document's style engine holds on the heap, by owner.
//!
//! Each owner is a store of the engine, or one kind of thing several stores share: the records,
//! their group payloads and chunks, and the style values. A shared allocation is counted once, by
//! the first owner the report reaches it from, so the owners the report walks first are the ones
//! that hold the most of what others share: the initial style before the records sharing its
//! chunks, the records before the slots holding them. See [`crate::heap_size`] for what a count
//! is.

use crate::heap_size::{HeapSize, HeapSizer};
use std::sync::Arc;

macro_rules! memory_owners {
    ($($owner:ident => $name:literal,)*) => {
        /// An owner of style memory.
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        #[repr(u8)]
        pub(crate) enum MemoryOwner {
            $($owner,)*
        }

        impl MemoryOwner {
            pub(crate) const ALL: &[MemoryOwner] = &[$(MemoryOwner::$owner,)*];
            pub(crate) const COUNT: usize = Self::ALL.len();

            pub(crate) fn name(self) -> &'static str {
                match self {
                    $(MemoryOwner::$owner => $name,)*
                }
            }
        }
    };
}

memory_owners! {
    Initial => "initial",
    RuleStore => "ruleStore",
    RuleIndex => "ruleIndex",
    UserAgentIndex => "userAgentIndex",
    SheetSegments => "sheetSegments",
    SharedDeclarations => "sharedDeclarations",
    Invalidation => "invalidation",
    Records => "records",
    Payloads => "payloads",
    GroupValueFacts => "groupValueFacts",
    CustomProperties => "customProperties",
    RetainedSlots => "retainedSlots",
    RecordCells => "recordCells",
    Installed => "installed",
    Projection => "projection",
    ProjectionGenerations => "projectionGenerations",
    HostTree => "hostTree",
    ElementDeclarations => "elementDeclarations",
    HeldRecordMaps => "heldRecordMaps",
    LayoutStyleColumns => "layoutStyleColumns",
    OtherMaps => "otherMaps",
    WorkerCaches => "workerCaches",
    StyleValues => "styleValues",
    AnimationEffects => "animationEffects",
    FontResolutions => "fontResolutions",
    LiveScratch => "liveScratch",
    CssAnimations => "cssAnimations",
    HostMemo => "hostMemo",
}

/// A memory report under way, and then its result.
#[derive(Default)]
pub(crate) struct MemoryReport {
    pub(crate) sizer: HeapSizer,
    bytes: [usize; MemoryOwner::COUNT],
    /// The elements the document holds.
    pub(crate) elements: usize,
    /// The elements that hold a style record.
    pub(crate) styled_elements: usize,
}

impl MemoryReport {
    /// A report of what `sizer` has not counted.
    pub(crate) fn past(sizer: HeapSizer) -> Self {
        Self {
            sizer,
            ..Self::default()
        }
    }

    pub(crate) fn add(&mut self, owner: MemoryOwner, bytes: usize) {
        self.bytes[owner as usize] += bytes;
    }

    /// Counts `value`'s heap toward `owner`.
    pub(crate) fn count<T: HeapSize + ?Sized>(&mut self, owner: MemoryOwner, value: &T) {
        let bytes = value.heap_size(&mut self.sizer);
        self.add(owner, bytes);
    }

    pub(crate) fn bytes(&self, owner: MemoryOwner) -> usize {
        self.bytes[owner as usize]
    }

    /// Ends the report, moving the style values counted aside to their owner.
    pub(crate) fn finish(mut self) -> Self {
        let style_value_bytes = std::mem::take(&mut self.sizer.style_value_bytes);
        self.add(MemoryOwner::StyleValues, style_value_bytes);
        self
    }
}

impl super::StyleEngine {
    /// What the engine holds on the heap, by owner, with the layout arena's style columns.
    pub(crate) fn memory_report(&self, arena: &crate::layout::LayoutNodeArena) -> MemoryReport {
        self.memory_report_into(MemoryReport::default(), arena)
    }

    /// [`Self::memory_report`], past what `report` has counted already.
    pub(crate) fn memory_report_into(
        &self,
        mut report: MemoryReport,
        arena: &crate::layout::LayoutNodeArena,
    ) -> MemoryReport {
        use crate::heap_size::{map_bytes, set_bytes, small_vec_bytes, vec_bytes};
        let state = &self.retained;
        let bytes = state.rules.heap_size(&mut report.sizer);
        report.add(MemoryOwner::RuleStore, bytes);
        // NB: The values the declarations were written with are the blocks' own, rather than
        //     counted aside.
        let style_value_bytes = report.sizer.style_value_bytes;
        let mut bytes = state.rules.declarations_heap_size(&mut report.sizer);
        if let Some(rules) = state.rules.user_agent_rules() {
            bytes += rules.declarations_heap_size(&mut report.sizer);
        }
        bytes += std::mem::replace(&mut report.sizer.style_value_bytes, style_value_bytes) - style_value_bytes;
        report.add(MemoryOwner::SharedDeclarations, bytes);
        if let Some(rules) = state.rules.user_agent_rules()
            && report.sizer.first_sight(Arc::as_ptr(rules))
        {
            let bytes = crate::heap_size::ARC_HEADER_BYTES + size_of_val(&**rules) + rules.heap_size(&mut report.sizer);
            report.add(MemoryOwner::UserAgentIndex, bytes);
        }
        if let Some(live) = &state.live_style {
            live.report_memory(&mut report);
        }
        state.computed_group_sets.report_memory(&mut report);
        let bytes = state.custom_property_environments.heap_size(&mut report.sizer);
        report.add(MemoryOwner::CustomProperties, bytes);
        report.add(
            MemoryOwner::CustomProperties,
            state.element_custom_property_data.heap_size()
                + state.pseudo_element_custom_property_data.heap_size()
                + state
                    .pseudo_element_custom_property_data
                    .values()
                    .map(vec_bytes)
                    .sum::<usize>(),
        );
        state.projection.report_memory(&mut report);
        let bytes = state.tree.heap_size(&mut report.sizer);
        report.add(MemoryOwner::HostTree, bytes);
        let bytes = state.element_declarations.heap_size(&mut report.sizer);
        report.add(MemoryOwner::ElementDeclarations, bytes);
        let held_style_records = state.held_style_records.heap_size(&mut report.sizer);
        report.add(
            MemoryOwner::HeldRecordMaps,
            held_style_records
                + state.held_pseudo_style_records.heap_size()
                + state
                    .held_pseudo_style_records
                    .values()
                    .map(small_vec_bytes)
                    .sum::<usize>(),
        );
        report.add(MemoryOwner::LayoutStyleColumns, arena.style_column_heap_size());
        if let Some(font_resolution) = &state.font_resolution {
            report.add(MemoryOwner::FontResolutions, font_resolution.heap_size());
        }
        let bytes = state.css_defined_animations.heap_size(&mut report.sizer);
        report.add(MemoryOwner::CssAnimations, bytes);
        let bytes = state.animation_effect_descriptions.heap_size(&mut report.sizer);
        report.add(MemoryOwner::AnimationEffects, bytes);
        report.add(
            MemoryOwner::OtherMaps,
            state.keyframes_reading_parent_reset_values.heap_size()
                + map_bytes(&state.layout_style_snapshots)
                + state.counter_style_environment_identities.heap_size()
                + state.backing_elements.heap_size()
                + state.replaced_content_inputs.heap_size()
                + state.demand_records.heap_size()
                + state.transition_baselines.heap_size()
                + state.scope_roots.heap_size()
                + state.scope_by_root.heap_size()
                + set_bytes(&state.unresolved_relational_arrival_keys),
        );
        report.finish()
    }
}
