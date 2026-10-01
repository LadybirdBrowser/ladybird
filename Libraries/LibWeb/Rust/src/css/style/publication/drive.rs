/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::cascaded_properties::CascadedValues;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum FontDriveGoal {
    Complete,
    RootInputs,
}

/// Why a row has no record. A suspended row is not refused: it waits on a request the host
/// services between passes, and the same row resumes once that request is answered.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(in crate::css::style) enum Unanswered {
    Suspended(Suspension),
    Refused,
    /// The row inherits from a parent that holds no record yet: one the host styles in the same
    /// update. The host retries the row once it has applied the rows before it.
    AwaitsParent,
}

/// What a suspended row waits on. The request itself stays where the host's service reads it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(in crate::css::style) enum Suspension {
    /// The drive's font is not resolved yet; `FontDriveScratch::request` names it.
    Font,
}

/// A row's answer from the engine, or why there is none.
pub(in crate::css::style) type Drive<T> = Result<T, Unanswered>;

/// What the monospace font-size recascade answers for a drive target.
pub(super) enum MonospaceRecascade {
    /// The font size the recascade reaches, and whether reaching it read the viewport.
    Size(i32, bool),
    /// A length in the ancestor chain resolves against the monospace font at the size reached so
    /// far, which the font resolver has not resolved yet.
    AwaitsFont(bridge::FfiFontResolutionRequest),
}

/// A missing value on the way to a record is the engine declining the row.
pub(in crate::css::style) trait OrRefused<T> {
    fn or_refused(self) -> Drive<T>;
}

impl<T> OrRefused<T> for Option<T> {
    fn or_refused(self) -> Drive<T> {
        self.ok_or(Unanswered::Refused)
    }
}

/// A driven longhand table with the length context it was driven against, the longhand
/// evaluations it took, and the font a full drive resolved.
pub(super) type DrivenTable = (
    ComputedLonghandTable,
    crate::css::style_compute::FfiLengthResolutionContext,
    u32,
    Option<crate::css::table_group_builder::FfiFontGroupBuildInputs>,
);

/// What a partial drive answers besides a refusal.
#[expect(
    clippy::large_enum_variant,
    reason = "the driven table moves by value, as it did in an `Option`"
)]
pub(super) enum PartialDrive {
    Driven(DrivenTable),
    /// An input the drive reads for properties it did not select moved with the selection: the
    /// caller drives the record in full instead.
    DriverInputMoved,
}

/// What a full drive answers besides a refusal or a suspension.
#[expect(
    clippy::large_enum_variant,
    reason = "the driven table moves by value, as it did in an `Option`"
)]
pub(super) enum FullDrive {
    Driven(DrivenTable),
    /// The root-input probe finished the font and line-height phases; the drive is left pending
    /// for the root's own row.
    RootInputs(RootFontInputs),
}

#[derive(Default)]
pub(in crate::css::style) struct FontDriveScratch {
    request: Option<font_resolution::FontRequest>,
    pending: Option<PendingFontDrive>,
}

impl FontDriveScratch {
    /// Suspend the drive for `target` on a font: the request and the drive it suspends are set
    /// together, so a retry always knows it resumes a drive whose entry gates passed.
    fn suspend(
        &mut self,
        target: computed::ComputedStyleTarget,
        request: bridge::FfiFontResolutionRequest,
        progress: Option<DriveProgress>,
    ) -> Unanswered {
        self.request = Some(font_resolution::FontRequest::new(request));
        self.pending = Some(PendingFontDrive { target, progress });
        Unanswered::Suspended(Suspension::Font)
    }

    /// Whether the suspended drive belongs to this node's row, for the element itself or one of
    /// its pseudo-elements. A row resumes only its own drive: the one left behind by an element
    /// the record loop settled another way is not an answer to it.
    pub(in crate::css::style) fn is_pending_for(&self, node: StyleNodeID) -> bool {
        self.pending
            .as_ref()
            .is_some_and(|pending| pending.target.node() == node)
    }

    /// The font request a drive suspended on.
    pub(in crate::css::style) fn take_suspended_request(&mut self) -> font_resolution::FontRequest {
        self.request
            .take()
            .expect("a drive suspended on a font names its request")
    }

    pub(in crate::css::style) fn capacity_bytes(&self) -> u64 {
        self.pending
            .as_ref()
            .and_then(|pending| pending.progress.as_ref())
            .map_or(0, |progress| progress.table.owned_capacity_bytes())
    }
}

/// An element's record, computed while one of its pseudo-elements waits for a font, held for the
/// element's own row to resume. Its fields are private to the drive, so the record can only be
/// taken back by naming the node it was computed for.
pub(super) struct PendingElement {
    node: StyleNodeID,
    delta: RecordDelta,
}

impl PendingElement {
    pub(super) fn new(node: StyleNodeID, delta: RecordDelta) -> Self {
        Self { node, delta }
    }

    /// The record parked for `node`'s row. A row never takes up another's: the flush loop resumes
    /// a suspended row before it drives the next one.
    pub(super) fn take_for(slot: &mut Option<Self>, node: StyleNodeID) -> Option<RecordDelta> {
        let pending = slot.take()?;
        debug_assert!(pending.node == node, "a row resumed another row's pending element");
        (pending.node == node).then_some(pending.delta)
    }
}

/// A drive left for its target's retry. No parent/context borrow survives refill; the caller
/// resumes the same subject before evaluating any later canonical element.
struct PendingFontDrive {
    /// What the suspended drive belongs to. The record loop can settle that element another way
    /// before the retry comes, which leaves the drive behind; whoever is driven next must not
    /// take up someone else's table.
    target: computed::ComputedStyleTarget,
    /// What the drive completed, or nothing when the monospace recascade waited on a font before
    /// the drive began: the retry then starts the drive afresh.
    progress: Option<DriveProgress>,
}

/// The completed font phase owns its table.
struct DriveProgress {
    root_font_complete: bool,
    /// The monospace recascade's size the table already holds, which the remaining phases read.
    recascaded_font_size: Option<i32>,
    table: ComputedLonghandTable,
    results: crate::css::style_compute::FfiLonghandDriverResults,
    effective_color_scheme: i16,
    resolved_viewport_relative_length: bool,
}

impl RetainedState {
    /// The font size the monospace recascade gives a drive target: the cascaded font-size of every
    /// ancestor it inherits from, root first, walked again from a 13px default. A pseudo-element
    /// inherits from its originating element. A length the walk cannot resolve from the document's
    /// inputs alone resolves against the monospace font at the size reached so far and the line
    /// height its ancestor inherits; a `calc()` is skipped as C++ skips it. `None` when a length
    /// still does not resolve, which C++ resolves.
    pub(super) fn monospace_recascaded_font_size(
        &self,
        target: computed::ComputedStyleTarget,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
    ) -> Option<MonospaceRecascade> {
        use crate::css::computed_value_types::{STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX};
        use crate::css::css_pixels::CssPixels;
        use crate::css::style_compute::{
            FfiFontMetrics, FfiFontSizeRecascadeDocumentInputs, FfiLengthResolutionContext, FontSizeRecascadeStatus,
            recascade_font_size_batch,
        };

        let mut views = Vec::new();
        let mut ancestor = match target.is_pseudo() {
            true => Some(target.node()),
            false => self.tree.inheritance_parent(target.node()),
        };
        while let Some(node) = ancestor {
            views.push(
                self.computed_group_sets
                    .assigned_style_record(node)
                    .and_then(|record| self.computed_group_sets.style_record_view(record.raw())),
            );
            ancestor = self.tree.inheritance_parent(node);
        }
        views.reverse();
        let value_at = |index: usize| {
            views[index]
                .as_ref()
                .and_then(|view| unsafe { view.longhand_table.as_ref() })
                .map_or(std::ptr::null(), ComputedLonghandTable::raw_cascaded_font_size)
        };
        let default_size = CssPixels::from_integer(13).raw_value();
        let document_inputs = FfiFontSizeRecascadeDocumentInputs::from_document(inputs);
        let mut batch = recascade_font_size_batch(
            views.len(),
            value_at,
            0,
            default_size,
            false,
            default_size,
            document_inputs,
            std::ptr::null(),
        );
        while batch.status != FontSizeRecascadeStatus::Complete {
            let index = batch.next_index;
            // AD-HOC: C++ measures the platform's monospace font directly, where this resolves the
            //         generic `monospace` family like any other request. The two differ only where
            //         an @font-face takes the platform monospace font's name.
            let request = bridge::FfiFontResolutionRequest {
                font_family: bridge::FfiHostHandle::from_pointer(self.monospace_font_family.pointer().cast()),
                font_feature_values: [bridge::FfiHostHandle::default(); bridge::FONT_RESOLUTION_FEATURE_INPUT_COUNT],
                font_size_raw: batch.current_size_raw,
                font_slope: 0,
                font_weight: 400.0,
                font_width: 100.0,
                font_optical_sizing: 0,
                font_environment_generation: inputs.font_environment_generation,
            };
            let Some(resolved) = self
                .font_resolution
                .as_ref()
                .and_then(|resolutions| resolutions.lookup(request))
            else {
                return Some(MonospaceRecascade::AwaitsFont(request));
            };
            // The ancestor's own font is the one being recascaded; its line height is the one it
            // inherits, and the initial one is zero.
            let (line_height, inherited_font_metrics_depend_on_viewport_metrics) = index
                .checked_sub(1)
                .and_then(|parent| views[parent].as_ref())
                .map_or((0.0, false), |view| {
                    let font = unsafe {
                        view.payloads[STYLE_GROUP_INDEX_FONT]
                            .cast::<crate::css::computed_value_types::FontValues>()
                            .deref()
                    };
                    (
                        font.line_height_used.to_double(),
                        view.dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS != 0,
                    )
                });
            let subject_inline_axis_is_horizontal = views[index].as_ref().is_none_or(|view| {
                let inherited_box = unsafe {
                    view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX]
                        .cast::<crate::css::computed_values::InheritedBoxValues>()
                        .deref()
                };
                inherited_box.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB
            });
            let context = FfiLengthResolutionContext {
                viewport_width: inputs.viewport_width,
                viewport_height: inputs.viewport_height,
                font_metrics: FfiFontMetrics {
                    font_size: CssPixels::from_raw(batch.current_size_raw).to_double(),
                    x_height: drive_font_metric(resolved.x_height),
                    cap_height: drive_font_metric(resolved.ascent),
                    zero_advance: drive_font_metric(resolved.zero_advance),
                    line_height,
                },
                root_font_metrics: FfiFontMetrics {
                    font_size: inputs.root_font_size,
                    x_height: inputs.root_font_x_height,
                    cap_height: inputs.root_font_cap_height,
                    zero_advance: inputs.root_font_zero_advance,
                    line_height: inputs.root_line_height,
                },
                font_metrics_depend_on_viewport_metrics: batch.depends_on_viewport_metrics
                    || inherited_font_metrics_depend_on_viewport_metrics,
                root_font_metrics_depend_on_viewport_metrics: inputs.root_font_metrics_depend_on_viewport_metrics,
                has_container_width_basis: false,
                has_container_height_basis: false,
                container_width_basis: 0.0,
                container_height_basis: 0.0,
                container_width_basis_depends_on_viewport_metrics: false,
                container_height_basis_depends_on_viewport_metrics: false,
                subject_inline_axis_is_horizontal,
                resolved_viewport_relative_length: std::ptr::null_mut(),
            };
            batch = recascade_font_size_batch(
                views.len(),
                value_at,
                index,
                batch.current_size_raw,
                batch.depends_on_viewport_metrics,
                default_size,
                document_inputs,
                &raw const context,
            );
            // A length the context does not resolve either, such as a container-relative one, is
            // left to C++.
            if batch.status == FontSizeRecascadeStatus::NeedsCppLengthResolution {
                return None;
            }
        }
        Some(MonospaceRecascade::Size(
            batch.current_size_raw,
            batch.depends_on_viewport_metrics,
        ))
    }

    /// Run the drive's remaining phase for the selected longhands over a copy of the node's
    /// current table, against the record's own font metrics, the document's computation inputs
    /// and the parent's record. The required driver inputs recompute on every drive and their
    /// post-compute adjustments read element facts this context does not carry, so the table
    /// stands only when they came out exactly as before.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn engine_driven_table(
        &self,
        node: StyleNodeID,
        old_style_record: computed::FinalStyleRecordID,
        store: &WinnerStore,
        selected: &[u64],
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        installed_ancestors: Option<&InstalledAncestors>,
        counters: &mut Counters,
    ) -> Drive<PartialDrive> {
        let store = store.view(self);
        use crate::css::computed_value_types::{STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX};
        use crate::css::style_compute::{
            FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiLengthResolutionContext, FfiStyleComputationEnvironment,
            LONGHAND_DRIVE_PHASE_REMAINING, drive_property_computation, empty_longhand_driver_results,
            is_required_driver_input, parent_snapshot_for_style_record, property_computation_order_for_phase,
        };

        let Some(view) = self.computed_group_sets.style_record_view(old_style_record.raw()) else {
            counters.bump(Counter::EngineComputedRecordBailRecord);
            return Err(Unanswered::Refused);
        };
        if !view.animated_overlay.is_null() {
            counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return Err(Unanswered::Refused);
        }
        let Some(old_table) = (unsafe { view.longhand_table.as_ref() }) else {
            counters.bump(Counter::EngineComputedRecordBailRecordTable);
            return Err(Unanswered::Refused);
        };
        // A record under display:none may no longer be the style C++ holds, and a property change
        // on an element with active transitions starts one in the C++ computation.
        if view.dependency_flags & (1 << 2) != 0
            || crate::css::style_compute::has_active_transition_properties(old_table)
        {
            counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return Err(Unanswered::Refused);
        }
        let parent = self
            .record_inheritance_parent(node, installed_ancestors)
            .inspect_err(|_| counters.bump(Counter::EngineComputedRecordBailRecordParent))?;
        let snapshot = match parent.and_then(|parent| self.computed_group_sets.assigned_style_record(parent)) {
            None => None,
            Some(record) => {
                let Some(view) = self.computed_group_sets.style_record_view(record.raw()) else {
                    debug_assert!(false, "an assigned parent record has a view");
                    counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return Err(Unanswered::Refused);
                };
                if !view.animated_overlay.is_null() {
                    counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return Err(Unanswered::Refused);
                }
                Some(parent_snapshot_for_style_record(self, record.raw(), None))
            }
        };
        let font = unsafe {
            view.payloads[STYLE_GROUP_INDEX_FONT]
                .cast::<crate::css::computed_value_types::FontValues>()
                .deref()
        };
        let inherited_box = unsafe {
            view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX]
                .cast::<crate::css::computed_values::InheritedBoxValues>()
                .deref()
        };
        let mut resolved_viewport_relative_length = false;
        let length = FfiLengthResolutionContext {
            viewport_width: inputs.viewport_width,
            viewport_height: inputs.viewport_height,
            font_metrics: FfiFontMetrics {
                font_size: font.font_size.to_double(),
                x_height: drive_font_metric(font.font_x_height),
                // The C++ metrics approximate the cap height with the ascent.
                cap_height: drive_font_metric(font.font_ascent),
                zero_advance: drive_font_metric(font.font_zero_advance),
                line_height: font.line_height_used.to_double(),
            },
            root_font_metrics: FfiFontMetrics {
                font_size: inputs.root_font_size,
                x_height: inputs.root_font_x_height,
                cap_height: inputs.root_font_cap_height,
                zero_advance: inputs.root_font_zero_advance,
                line_height: inputs.root_line_height,
            },
            font_metrics_depend_on_viewport_metrics: view.dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS
                != 0,
            root_font_metrics_depend_on_viewport_metrics: inputs.root_font_metrics_depend_on_viewport_metrics,
            has_container_width_basis: false,
            has_container_height_basis: false,
            container_width_basis: 0.0,
            container_height_basis: 0.0,
            container_width_basis_depends_on_viewport_metrics: false,
            container_height_basis_depends_on_viewport_metrics: false,
            subject_inline_axis_is_horizontal: inherited_box.writing_mode
                == crate::css::css_enums::writing_mode::HORIZONTAL_TB,
            resolved_viewport_relative_length: &raw mut resolved_viewport_relative_length,
        };
        // No element fact reaches the remaining phase through this environment: the moved
        // properties were checked not to need one, and the required driver inputs are compared
        // against the record below.
        let environment = FfiStyleComputationEnvironment {
            box_type_input: crate::css::style_compute::rust_box_type_transformation_input(
                0,
                crate::css::style_compute::FfiStyleAdjustmentTarget::Element,
                false,
                crate::css::display::FfiDisplay::block(),
            ),
            color_scheme_input: FfiEffectiveColorSchemeInput {
                preferred_color_scheme: 0,
                has_document_supported_schemes: false,
                document_supported_scheme_codes: std::ptr::null(),
                document_supported_scheme_count: 0,
            },
            is_th_element: false,
            has_new_font_size: false,
            has_tree_counting_context: false,
            sibling_count: 0,
            sibling_index: 0,
            random_base_values: std::ptr::null(),
            random_base_value_count: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            style_sheet_resource_contexts: std::ptr::null(),
            style_sheet_resource_context_count: 0,
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            initial_font_size_raw: inputs.initial_font_size_raw,
            default_font_size_raw: inputs.default_font_size_raw,
        };
        let mut table = view.longhand_table_for_partial_drive();
        let mut results = empty_longhand_driver_results();
        let mut effective_color_scheme = old_table.effective_color_scheme();
        unsafe {
            drive_property_computation(
                &raw mut table,
                std::ptr::null_mut(),
                &store,
                snapshot.as_ref(),
                None,
                &raw const environment,
                u32::MAX,
                selected.as_ptr(),
                LONGHAND_DRIVE_PHASE_REMAINING,
                &raw const length,
                std::ptr::null(),
                std::ptr::null(),
                &raw mut results,
                &mut effective_color_scheme,
                true,
            );
        }
        counters.bump(Counter::EnginePartialDrivesStarted);

        counters.add(
            Counter::EnginePhysicalLonghandEvaluations,
            u64::from(results.longhand_evaluations),
        );
        counters.add(
            Counter::EnginePartialLonghandEvaluations,
            u64::from(results.longhand_evaluations),
        );
        if results.explicitly_inherited_non_inherited_style_groups != 0 || results.uses_tree_counting_function {
            counters.bump(Counter::EngineComputedRecordBailDrive);
            return Err(Unanswered::Refused);
        }
        // An input the drive reads for properties it did not select moved with the selection: the
        // caller drives the record in full instead.
        if table.display_before_box_type_transformation() != old_table.display_before_box_type_transformation() {
            return Ok(PartialDrive::DriverInputMoved);
        }
        let old_values = old_table.value_pointers();
        for &property in property_computation_order_for_phase(LONGHAND_DRIVE_PHASE_REMAINING) {
            if !is_required_driver_input(property) {
                continue;
            }
            let slot = usize::from(property - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID);
            let old_value = old_values[slot];
            let new_value = table
                .get(property)
                .map_or(std::ptr::null(), |value| value.pointer().cast());
            if old_value == new_value {
                continue;
            }
            let equal = unsafe {
                match (
                    old_value.cast::<StyleValueData>().as_ref(),
                    new_value.cast::<StyleValueData>().as_ref(),
                ) {
                    (Some(old_value), Some(new_value)) => old_value == new_value,
                    _ => false,
                }
            };
            if !equal {
                return Ok(PartialDrive::DriverInputMoved);
            }
            table.copy_slot_from(old_table, property);
        }
        // The group builders resolve against the same context; they report no viewport dependence
        // of their own.
        let length = FfiLengthResolutionContext {
            resolved_viewport_relative_length: std::ptr::null_mut(),
            ..length
        };
        Ok(PartialDrive::Driven((
            table,
            length,
            results.longhand_evaluations,
            None,
        )))
    }

    /// Drive a record through every phase: the font phase against the parent's metrics, the
    /// element's font resolved through the document's resolver, line-height and color-scheme
    /// against that font, and the remaining phase with the element facts the box-type
    /// transformation reads. Root-input preparation finishes only the font and line-height
    /// phases and preserves them for completion. Monospace default-size recascade stays in C++.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub(super) fn engine_full_drive(
        &self,
        subject: DriveSubject,
        old_style_record: Option<computed::FinalStyleRecordID>,
        store: &WinnerStore,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        font_scratch: &mut FontDriveScratch,
        goal: FontDriveGoal,
        counters: &mut Counters,
    ) -> Drive<FullDrive> {
        let store = store.view(self);
        use crate::css::computed_value_types::{STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX};
        use crate::css::css_pixels::CssPixels;
        use crate::css::property_metadata::property_id as prop;
        use crate::css::style_compute::{
            FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiInputLineHeightMetrics, FfiLengthResolutionContext,
            FfiStyleComputationEnvironment, LONGHAND_DRIVE_PHASE_COLOR_SCHEME, LONGHAND_DRIVE_PHASE_FONT,
            LONGHAND_DRIVE_PHASE_LINE_HEIGHT, LONGHAND_DRIVE_PHASE_REMAINING, drive_property_computation,
            effective_display, empty_longhand_driver_results, font_family_is_monospace, keyword,
        };
        use crate::css::table_group_builder::FfiFontGroupBuildInputs;
        use bridge::element_adjustment_fact as fact;

        let DriveSubject { target, parent, facts } = subject;
        let has = |bit: u32| facts & bit != 0;
        let is_document_element = has(fact::IS_DOCUMENT_ELEMENT);
        // An element with animations composes its style with their effects in C++.
        if facts & fact::HAS_ANIMATIONS != 0 {
            counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return Err(Unanswered::Refused);
        }
        if !self.computes_records() {
            counters.bump(Counter::EngineComputedRecordBailUnhosted);
            return Err(Unanswered::Refused);
        }
        // HACK: A cascade that ends in `font-family: monospace` re-runs the font-size cascade over
        //       the whole ancestor chain against a 13px default instead of the 16px one, which
        //       changes what a keyword size an ancestor declared means. See
        //       `StyleComputer::recascade_font_size_if_needed`.
        let mut recascaded_font_size_reads_viewport = false;
        let recascaded_font_size = if let Some(progress) = font_scratch
            .pending
            .as_ref()
            .filter(|pending| pending.target == target)
            .and_then(|pending| pending.progress.as_ref())
        {
            progress.recascaded_font_size
        } else if store
            .winning_declaration(prop::FONT_FAMILY)
            .is_some_and(|(value, ..)| font_family_is_monospace(unsafe { &*value.cast::<StyleValueData>() }))
        {
            match self.monospace_recascaded_font_size(target, inputs) {
                Some(MonospaceRecascade::Size(recascaded, reads_viewport)) => {
                    recascaded_font_size_reads_viewport = reads_viewport;
                    Some(recascaded)
                }
                Some(MonospaceRecascade::AwaitsFont(request)) => {
                    return Err(font_scratch.suspend(target, request, None));
                }
                None => {
                    counters.bump(Counter::EngineComputedRecordBailMonospaceQuirk);
                    return Err(Unanswered::Refused);
                }
            }
        } else {
            None
        };
        let old_table = match old_style_record {
            Some(old_style_record) => {
                let Some(view) = self.computed_group_sets.style_record_view(old_style_record.raw()) else {
                    counters.bump(Counter::EngineComputedRecordBailRecord);
                    return Err(Unanswered::Refused);
                };
                if !view.animated_overlay.is_null() {
                    counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return Err(Unanswered::Refused);
                }
                let Some(old_table) = (unsafe { view.longhand_table.as_ref() }) else {
                    counters.bump(Counter::EngineComputedRecordBailRecordTable);
                    return Err(Unanswered::Refused);
                };
                // A record kept under display:none is still what the element's own style is driven
                // from, but the animations it names start only when C++ computes the element out of
                // that subtree.
                if (view.dependency_flags & (1 << 2) != 0 && table_names_animations(old_table))
                    || crate::css::style_compute::has_active_transition_properties(old_table)
                {
                    counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return Err(Unanswered::Refused);
                }
                Some(old_table)
            }
            None => None,
        };
        let parent_view = match parent {
            Some(parent) => {
                let Some(parent_record) = self.computed_group_sets.assigned_style_record(parent) else {
                    counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return Err(Unanswered::AwaitsParent);
                };
                let Some(parent_view) = self.computed_group_sets.style_record_view(parent_record.raw()) else {
                    debug_assert!(false, "an assigned parent record has a view");
                    counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return Err(Unanswered::Refused);
                };
                if !parent_view.animated_overlay.is_null() {
                    counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return Err(Unanswered::Refused);
                }
                Some(parent_view)
            }
            None => None,
        };
        // The document element inherits from the initial values and resolves its font against
        // the document's initial font, the way C++'s document resolution context does.
        let initial_metrics = FfiFontMetrics {
            font_size: inputs.initial_font_size,
            x_height: inputs.initial_font_x_height,
            cap_height: inputs.initial_font_cap_height,
            zero_advance: inputs.initial_font_zero_advance,
            line_height: 0.0,
        };
        let (parent_metrics, parent_font_metrics_depend_on_viewport_metrics, parent_line_height_used) =
            match &parent_view {
                Some(parent_view) => {
                    let parent_font = unsafe {
                        parent_view.payloads[STYLE_GROUP_INDEX_FONT]
                            .cast::<crate::css::computed_value_types::FontValues>()
                            .deref()
                    };
                    (
                        FfiFontMetrics {
                            font_size: parent_font.font_size.to_double(),
                            x_height: drive_font_metric(parent_font.font_x_height),
                            cap_height: drive_font_metric(parent_font.font_ascent),
                            zero_advance: drive_font_metric(parent_font.font_zero_advance),
                            line_height: parent_font.line_height_used.to_double(),
                        },
                        parent_view.dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS != 0,
                        parent_font.line_height_used.to_double(),
                    )
                }
                None => (initial_metrics, false, 0.0),
            };
        // C++ computes no style under a display:none ancestor.
        if old_table.is_none()
            && parent_view
                .as_ref()
                .is_some_and(|parent_view| parent_view.dependency_flags & (1 << 2) != 0)
        {
            counters.bump(Counter::EngineComputedRecordBailRecordParent);
            return Err(Unanswered::Refused);
        }
        // The parent's display, past any display:contents ancestor, is what the box-type
        // transformation reads.
        let mut parent_display = None;
        let mut ancestor = parent;
        while let Some(current) = ancestor {
            let Some(record) = self.computed_group_sets.assigned_style_record(current) else {
                break;
            };
            let Some(ancestor_view) = self.computed_group_sets.style_record_view(record.raw()) else {
                break;
            };
            let Some(ancestor_table) = (unsafe { ancestor_view.longhand_table.as_ref() }) else {
                break;
            };
            let display = effective_display(ancestor_table, None);
            if !display.is_contents() {
                parent_display = Some(display);
                break;
            }
            ancestor = self.tree.inheritance_parent(current);
        }
        let snapshot = match &parent_view {
            Some(parent_view) => {
                let Some(parent_table) = (unsafe { parent_view.longhand_table.as_ref() }) else {
                    debug_assert!(false, "an assigned parent record has a longhand table");
                    counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return Err(Unanswered::Refused);
                };
                Some(crate::css::style_compute::ParentSnapshot::new(
                    parent_table,
                    unsafe { parent_view.animated_overlay.as_ref() },
                    parent_font_metrics_depend_on_viewport_metrics,
                    parent_view.dependency_flags & (1 << 2) != 0,
                ))
            }
            None => None,
        };
        // The subject axis is the element's own writing mode when it has one, else its parent's;
        // the initial writing mode is horizontal.
        let inherited_box_payload = match old_style_record {
            Some(old_style_record) => {
                let Some(view) = self.computed_group_sets.style_record_view(old_style_record.raw()) else {
                    counters.bump(Counter::EngineComputedRecordBailRecord);
                    return Err(Unanswered::Refused);
                };
                Some(view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX])
            }
            None => parent_view
                .as_ref()
                .map(|parent_view| parent_view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX]),
        };
        let subject_inline_axis_is_horizontal = inherited_box_payload.is_none_or(|payload| {
            let inherited_box = unsafe {
                payload
                    .cast::<crate::css::computed_values::InheritedBoxValues>()
                    .deref()
            };
            inherited_box.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB
        });
        let document_root_font_metrics = FfiFontMetrics {
            font_size: inputs.root_font_size,
            x_height: inputs.root_font_x_height,
            cap_height: inputs.root_font_cap_height,
            zero_advance: inputs.root_font_zero_advance,
            line_height: inputs.root_line_height,
        };
        let environment = FfiStyleComputationEnvironment {
            box_type_input: crate::css::style_compute::rust_box_type_transformation_input(
                facts,
                crate::css::style_compute::FfiStyleAdjustmentTarget::Element,
                parent_display.is_some(),
                parent_display.unwrap_or_else(crate::css::display::FfiDisplay::block),
            ),
            color_scheme_input: FfiEffectiveColorSchemeInput {
                preferred_color_scheme: inputs.preferred_color_scheme,
                has_document_supported_schemes: inputs.has_document_supported_schemes,
                document_supported_scheme_codes: inputs.document_supported_scheme_codes.as_ptr(),
                document_supported_scheme_count: usize::from(inputs.document_supported_scheme_count),
            },
            is_th_element: has(fact::IS_TH),
            has_new_font_size: recascaded_font_size.is_some(),
            has_tree_counting_context: false,
            sibling_count: 0,
            sibling_index: 0,
            random_base_values: std::ptr::null(),
            random_base_value_count: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            style_sheet_resource_contexts: std::ptr::null(),
            style_sheet_resource_context_count: 0,
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            initial_font_size_raw: inputs.initial_font_size_raw,
            default_font_size_raw: inputs.default_font_size_raw,
        };
        let mut resolved_viewport_relative_length = false;
        let resolved_viewport_relative_length_pointer = &raw mut resolved_viewport_relative_length;
        // The root metrics a phase resolves against: the document's, except for the document
        // element itself, whose font phase reads the initial font and whose line-height phase
        // reads its own font; its remaining phase reads the document's metrics as they stand,
        // which C++ refreshes only after computing it.
        let length_context =
            |font_metrics: FfiFontMetrics,
             font_metrics_depend_on_viewport_metrics: bool,
             root_font_metrics: FfiFontMetrics,
             root_font_metrics_depend_on_viewport_metrics: bool| FfiLengthResolutionContext {
                viewport_width: inputs.viewport_width,
                viewport_height: inputs.viewport_height,
                font_metrics,
                root_font_metrics,
                font_metrics_depend_on_viewport_metrics,
                root_font_metrics_depend_on_viewport_metrics,
                has_container_width_basis: false,
                has_container_height_basis: false,
                container_width_basis: 0.0,
                container_height_basis: 0.0,
                container_width_basis_depends_on_viewport_metrics: false,
                container_height_basis_depends_on_viewport_metrics: false,
                subject_inline_axis_is_horizontal,
                resolved_viewport_relative_length: resolved_viewport_relative_length_pointer,
            };
        // A drive another row left behind is dropped rather than taken up.
        let resumed = font_scratch
            .pending
            .take()
            .filter(|pending| pending.target == target)
            .and_then(|pending| pending.progress);
        let resuming = resumed.is_some();
        let root_font_complete = resumed.as_ref().is_some_and(|pending| pending.root_font_complete);
        if !resuming {
            counters.bump(Counter::EngineFullDrivesStarted);
        }
        let (mut table, mut results, mut effective_color_scheme) = match resumed {
            Some(pending) => {
                resolved_viewport_relative_length = pending.resolved_viewport_relative_length;
                if !pending.root_font_complete {
                    counters.bump(Counter::FontRefillResumedDrives);
                    counters.add(
                        Counter::FontRefillPreservedLonghands,
                        u64::from(pending.results.longhand_evaluations),
                    );
                }
                (pending.table, pending.results, pending.effective_color_scheme)
            }
            None => (
                old_table.map_or_else(ComputedLonghandTable::new, ComputedLonghandTable::copied_for_drive),
                empty_longhand_driver_results(),
                -1,
            ),
        };
        let drive = |counters: &mut Counters,
                     table: &mut ComputedLonghandTable,
                     results: &mut crate::css::style_compute::FfiLonghandDriverResults,
                     effective_color_scheme: &mut i16,
                     phase: u8,
                     length: *const FfiLengthResolutionContext,
                     input_line_height_metrics: *const FfiInputLineHeightMetrics,
                     line_height_before: *const std::ffi::c_void| unsafe {
            let evaluations_before = results.longhand_evaluations;
            drive_property_computation(
                std::ptr::from_mut(table),
                std::ptr::null_mut(),
                &store,
                snapshot.as_ref(),
                None,
                &raw const environment,
                u32::MAX,
                std::ptr::null(),
                phase,
                length,
                input_line_height_metrics,
                line_height_before,
                std::ptr::from_mut(results),
                effective_color_scheme,
                true,
            );
            counters.add(
                Counter::EnginePhysicalLonghandEvaluations,
                u64::from(results.longhand_evaluations - evaluations_before),
            );
        };
        let font_length = if is_document_element {
            length_context(initial_metrics, false, initial_metrics, false)
        } else {
            length_context(
                parent_metrics,
                parent_font_metrics_depend_on_viewport_metrics,
                document_root_font_metrics,
                inputs.root_font_metrics_depend_on_viewport_metrics,
            )
        };
        if !resuming {
            // The recascaded size stands in for the element's cascaded `font-size`, which the drive
            // then leaves alone, exactly as C++ writes it into the working set before driving. An
            // element that declares its own `font-size` still has that declaration win, because the
            // drive reads a winning declaration before it consults this.
            if let Some(recascaded) = recascaded_font_size {
                table.set_computed(
                    prop::FONT_SIZE,
                    StyleValueData::Length {
                        value: CssPixels::from_raw(recascaded).to_double(),
                        unit: crate::css::style_compute::px_length_unit(),
                    },
                    -1,
                );
            }
            drive(
                counters,
                &mut table,
                &mut results,
                &mut effective_color_scheme,
                LONGHAND_DRIVE_PHASE_FONT,
                &raw const font_length,
                std::ptr::null(),
                std::ptr::null(),
            );
            // A recascaded size that read the viewport makes the element's style and font metrics
            // read it, as C++ marks them beside the size it writes.
            if recascaded_font_size_reads_viewport {
                results.depends_on_viewport_metrics = true;
                results.font_metrics_depend_on_viewport_metrics = true;
            }
        }

        // The element's own font, resolved as the C++ font computer would for these values.
        let value_of = |table: &ComputedLonghandTable, property: u16| -> Option<&StyleValueData> {
            unsafe {
                table
                    .effective_value(None, property, true)
                    .value
                    .cast::<StyleValueData>()
                    .as_ref()
            }
        };
        // `font-variant-alternates` names features through its tree scope's `@font-feature-values`,
        // and the engine's resolver only has the document's. An element in a shadow tree that sets it
        // keeps its record in C++.
        if self.tree.tree_scope(target.node()) != TreeScopeID::DOCUMENT
            && !matches!(
                value_of(&table, prop::FONT_VARIANT_ALTERNATES),
                Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::NORMAL
            )
        {
            counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return Err(Unanswered::Refused);
        }
        // The resolver reads these beside the family, so the request names each one whose computed
        // value is not the initial one and nothing for the rest. A non-initial value can also come
        // from inheritance.
        let mut font_feature_values = [bridge::FfiHostHandle::default(); bridge::FONT_RESOLUTION_FEATURE_INPUT_COUNT];
        for (input, property, initial_keyword) in FONT_RESOLUTION_FEATURE_PROPERTIES {
            if !matches!(value_of(&table, property),
                Some(StyleValueData::Keyword { keyword }) if *keyword == initial_keyword)
            {
                font_feature_values[input as usize] =
                    bridge::FfiHostHandle::from_pointer(table.effective_value(None, property, true).value.cast());
            }
        }
        // The font size the element's own lengths resolve against is the C++ working set's, a
        // CSSPixels value, not the computed value's double.
        let font_size = match value_of(&table, prop::FONT_SIZE) {
            Some(StyleValueData::Length { value, unit }) if *unit == crate::css::style_compute::px_length_unit() => {
                CssPixels::nearest_value_for(*value).to_double()
            }
            _ => {
                counters.bump(Counter::EngineComputedRecordBailFontPhase);
                return Err(Unanswered::Refused);
            }
        };
        let font_size_raw = CssPixels::nearest_value_for(font_size).raw_value();
        let font_family = table.effective_value(None, prop::FONT_FAMILY, true).value;
        let font_slope = match value_of(&table, prop::FONT_STYLE) {
            Some(StyleValueData::FontStyle { font_style, .. }) => match *font_style {
                crate::css::css_enums::font_style_keyword::ITALIC => 1,
                crate::css::css_enums::font_style_keyword::OBLIQUE => 2,
                _ => 0,
            },
            _ => 0,
        };
        let (font_weight, font_width) = match (value_of(&table, prop::FONT_WEIGHT), value_of(&table, prop::FONT_WIDTH))
        {
            (Some(StyleValueData::Number { value: weight }), Some(StyleValueData::Percentage { value: width })) => {
                (*weight, *width)
            }
            _ => {
                counters.bump(Counter::EngineComputedRecordBailFontPhase);
                return Err(Unanswered::Refused);
            }
        };
        let font_optical_sizing = match value_of(&table, prop::FONT_OPTICAL_SIZING) {
            Some(StyleValueData::Keyword { keyword }) => {
                crate::css::css_enums::keyword_to_font_optical_sizing(*keyword).unwrap_or(0)
            }
            _ => 0,
        };
        let request = bridge::FfiFontResolutionRequest {
            font_family: bridge::FfiHostHandle::from_pointer(font_family.cast()),
            font_feature_values,
            font_size_raw,
            font_slope,
            font_weight,
            font_width,
            font_optical_sizing,
            font_environment_generation: inputs.font_environment_generation,
        };
        let Some(resolved) = self
            .font_resolution
            .as_ref()
            .and_then(|resolutions| resolutions.lookup(request))
        else {
            let progress = DriveProgress {
                root_font_complete: false,
                recascaded_font_size,
                table,
                results,
                effective_color_scheme,
                resolved_viewport_relative_length,
            };
            return Err(font_scratch.suspend(target, request, Some(progress)));
        };
        let own_metrics = |line_height: f64| FfiFontMetrics {
            font_size,
            x_height: drive_font_metric(resolved.x_height),
            cap_height: drive_font_metric(resolved.ascent),
            zero_advance: drive_font_metric(resolved.zero_advance),
            line_height,
        };

        let line_height_length = if is_document_element {
            length_context(
                own_metrics(parent_line_height_used),
                results.font_metrics_depend_on_viewport_metrics,
                own_metrics(parent_line_height_used),
                results.font_metrics_depend_on_viewport_metrics,
            )
        } else {
            length_context(
                own_metrics(parent_line_height_used),
                results.font_metrics_depend_on_viewport_metrics,
                document_root_font_metrics,
                inputs.root_font_metrics_depend_on_viewport_metrics,
            )
        };
        if !root_font_complete {
            drive(
                counters,
                &mut table,
                &mut results,
                &mut effective_color_scheme,
                LONGHAND_DRIVE_PHASE_LINE_HEIGHT,
                &raw const line_height_length,
                std::ptr::null(),
                std::ptr::null(),
            );
        }

        // The used line height, as the C++ working set reads it from the computed value.
        let normal_line_height = f64::from(resolved.ascent.round() as i32 + resolved.descent.round() as i32);
        let line_height_used = |table: &ComputedLonghandTable| -> Option<f64> {
            match value_of(table, prop::LINE_HEIGHT)? {
                StyleValueData::Keyword { keyword } if *keyword == keyword::NORMAL => Some(normal_line_height),
                StyleValueData::Length { value, unit } if *unit == crate::css::style_compute::px_length_unit() => {
                    Some(CssPixels::nearest_value_for(*value).to_double())
                }
                StyleValueData::Number { value } => Some(CssPixels::nearest_value_for(value * font_size).to_double()),
                _ => None,
            }
        };
        let Some(line_height_before_adjustments) = line_height_used(&table) else {
            counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return Err(Unanswered::Refused);
        };
        if goal == FontDriveGoal::RootInputs {
            let root_inputs = RootFontInputs {
                metrics: [
                    font_size.to_bits(),
                    drive_font_metric(resolved.x_height).to_bits(),
                    drive_font_metric(resolved.ascent).to_bits(),
                    drive_font_metric(resolved.zero_advance).to_bits(),
                    line_height_before_adjustments.to_bits(),
                ],
                depends_on_viewport: results.font_metrics_depend_on_viewport_metrics,
            };
            font_scratch.pending = Some(PendingFontDrive {
                target,
                progress: Some(DriveProgress {
                    root_font_complete: true,
                    recascaded_font_size,
                    table,
                    results,
                    effective_color_scheme,
                    resolved_viewport_relative_length,
                }),
            });
            return Ok(FullDrive::RootInputs(root_inputs));
        }
        drive(
            counters,
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_COLOR_SCHEME,
            std::ptr::null(),
            std::ptr::null(),
            std::ptr::null(),
        );
        effective_color_scheme = table.effective_color_scheme();

        let remaining_length = length_context(
            own_metrics(line_height_before_adjustments),
            results.font_metrics_depend_on_viewport_metrics,
            if is_document_element {
                own_metrics(line_height_before_adjustments)
            } else {
                document_root_font_metrics
            },
            if is_document_element {
                results.font_metrics_depend_on_viewport_metrics
            } else {
                inputs.root_font_metrics_depend_on_viewport_metrics
            },
        );
        let input_line_height_metrics = if has(fact::CHECK_INPUT_LINE_HEIGHT) {
            FfiInputLineHeightMetrics {
                current_line_height: line_height_before_adjustments,
                minimum_line_height: normal_line_height,
            }
        } else {
            FfiInputLineHeightMetrics {
                current_line_height: 0.0,
                minimum_line_height: 0.0,
            }
        };
        let line_height_value = table.effective_value(None, prop::LINE_HEIGHT, true).value;
        drive(
            counters,
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_REMAINING,
            &raw const remaining_length,
            &raw const input_line_height_metrics,
            line_height_value,
        );
        if results.explicitly_inherited_non_inherited_style_groups != 0 || results.uses_tree_counting_function {
            counters.bump(Counter::EngineComputedRecordBailDrive);
            return Err(Unanswered::Refused);
        }
        let Some(line_height_used_after) = line_height_used(&table) else {
            counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return Err(Unanswered::Refused);
        };
        let keyword_code = |property: u16, map: fn(u16) -> Option<u8>| match value_of(&table, property) {
            Some(StyleValueData::Keyword { keyword }) => map(*keyword).unwrap_or(0),
            _ => 0,
        };
        let math_depth = match value_of(&table, prop::MATH_DEPTH) {
            Some(StyleValueData::Integer { value }) => *value,
            _ => 0,
        };
        let font = FfiFontGroupBuildInputs {
            font_size_raw,
            line_height_used_raw: CssPixels::nearest_value_for(line_height_used_after).raw_value(),
            font_variant_emoji: keyword_code(
                prop::FONT_VARIANT_EMOJI,
                crate::css::css_enums::keyword_to_font_variant_emoji,
            ),
            font_ascent: resolved.ascent,
            font_descent: resolved.descent,
            font_x_height: resolved.x_height,
            font_zero_advance: resolved.zero_advance,
            first_available_font: resolved.first_available_font.as_pointer(),
            font_cascade_list: resolved.font_cascade_list.as_pointer(),
            font_weight,
            font_width,
            math_shift: keyword_code(prop::MATH_SHIFT, crate::css::css_enums::keyword_to_math_shift),
            math_style: keyword_code(prop::MATH_STYLE, crate::css::css_enums::keyword_to_math_style),
            math_depth,
        };
        let length = FfiLengthResolutionContext {
            resolved_viewport_relative_length: std::ptr::null_mut(),
            ..remaining_length
        };
        Ok(FullDrive::Driven((
            table,
            length,
            results.longhand_evaluations,
            Some(font),
        )))
    }
}

/// The property behind each value a font resolution request names, with the initial keyword for
/// which the request names nothing.
const FONT_RESOLUTION_FEATURE_PROPERTIES: [(bridge::FontResolutionFeatureInput, u16, u16);
    bridge::FONT_RESOLUTION_FEATURE_INPUT_COUNT] = {
    use crate::css::property_metadata::property_id as prop;
    use crate::css::style_compute::keyword::{AUTO, NORMAL};
    use bridge::FontResolutionFeatureInput as Input;
    [
        (Input::FontFeatureSettings, prop::FONT_FEATURE_SETTINGS, NORMAL),
        (Input::FontVariationSettings, prop::FONT_VARIATION_SETTINGS, NORMAL),
        (Input::FontVariantCaps, prop::FONT_VARIANT_CAPS, NORMAL),
        (Input::FontVariantEastAsian, prop::FONT_VARIANT_EAST_ASIAN, NORMAL),
        (Input::FontVariantEmoji, prop::FONT_VARIANT_EMOJI, NORMAL),
        (Input::FontVariantLigatures, prop::FONT_VARIANT_LIGATURES, NORMAL),
        (Input::FontVariantNumeric, prop::FONT_VARIANT_NUMERIC, NORMAL),
        (Input::FontVariantPosition, prop::FONT_VARIANT_POSITION, NORMAL),
        (Input::FontVariantAlternates, prop::FONT_VARIANT_ALTERNATES, NORMAL),
        (Input::FontKerning, prop::FONT_KERNING, AUTO),
        (Input::TextRendering, prop::TEXT_RENDERING, AUTO),
    ]
};

// Every input appears once, at its own index, so no slot of a request goes unwritten.
const _: () = {
    let mut index = 0;
    while index < FONT_RESOLUTION_FEATURE_PROPERTIES.len() {
        assert!(FONT_RESOLUTION_FEATURE_PROPERTIES[index].0 as usize == index);
        index += 1;
    }
};

/// Whether a computed table's `animation-name` names any animation.
fn table_names_animations(table: &ComputedLonghandTable) -> bool {
    use crate::css::style_compute::keyword;
    let is_none =
        |value: &StyleValueData| matches!(value, StyleValueData::Keyword { keyword: name } if *name == keyword::NONE);
    table
        .get(crate::css::property_metadata::property_id::ANIMATION_NAME)
        .is_some_and(|value| match value.data() {
            StyleValueData::ValueList { values, .. } => values.as_slice().iter().any(|value| !is_none(value.data())),
            value => !is_none(value),
        })
}

/// A font's pixel metric as the drive resolves font-relative units against it: the C++ length
/// resolution context carries the metrics as `CSSPixels`, so an `ex` resolves against the
/// fixed-point x-height rather than the font's raw floating-point one.
pub(super) fn drive_font_metric(value: f32) -> f64 {
    crate::css::css_pixels::CssPixels::nearest_value_for_f32(value).to_double()
}
