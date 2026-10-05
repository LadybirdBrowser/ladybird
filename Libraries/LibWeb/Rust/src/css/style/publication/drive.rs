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
    /// update. The style pass settles the row in its next wave, once the host installed the rows
    /// before it.
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
/// evaluations it took, the font a full drive resolved, and the non-inherited style groups it read
/// straight from the parent through an explicit `inherit`, which C++ marks the parent with.
pub(super) struct DrivenTable {
    pub(super) table: ComputedLonghandTable,
    pub(super) length: crate::css::style_compute::FfiLengthResolutionContext,
    pub(super) longhand_evaluations: u32,
    pub(super) font: Option<crate::css::table_group_builder::FfiFontGroupBuildInputs>,
    pub(super) explicitly_inherited_groups: u32,
}

/// What a partial drive answers besides a refusal.
#[expect(
    clippy::large_enum_variant,
    reason = "the driven table moves by value, as it did in an `Option`"
)]
pub(super) enum PartialDrive {
    Driven(DrivenTable),
    /// An input the drive reads for properties it did not select moved with the selection, or the
    /// old record holds no table to copy them from: the caller drives the record in full instead.
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
    /// The drive settled the element's font and color scheme, which the registered custom
    /// properties it declares compute against; it is left pending until the caller resolves them
    /// against this context, substitutes its winners again, and drives once more without waiting.
    AwaitsRegisteredContext(custom_property_cascade::RegisteredValueContext),
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
    /// Whether the color-scheme phase ran too, as it has for a drive awaiting its registered
    /// context.
    color_scheme_complete: bool,
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
    /// inputs alone resolves against the monospace font at the size reached so far, the line
    /// height its ancestor inherits and the ancestor's query containers; a `calc()` is skipped as
    /// C++ skips it. `None` where a length still does not resolve, which it should not.
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
            views.push((
                node,
                self.computed_group_sets
                    .assigned_style_record(node)
                    .and_then(|record| self.computed_group_sets.style_record_view(record.raw())),
            ));
            ancestor = self.tree.inheritance_parent(node);
        }
        views.reverse();
        let value_at = |index: usize| {
            views[index]
                .1
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
                font_feature_values_scope: TreeScopeID::DOCUMENT.0,
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
                .and_then(|parent| views[parent].1.as_ref())
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
            let (ancestor_node, ancestor_view) = &views[index];
            let subject_inline_axis_is_horizontal = ancestor_view.as_ref().is_none_or(|view| {
                let inherited_box = unsafe {
                    view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX]
                        .cast::<crate::css::computed_values::InheritedBoxValues>()
                        .deref()
                };
                inherited_box.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB
            });
            let mut context = FfiLengthResolutionContext {
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
            // The ancestor's container-relative font size resolves against its own containers.
            self.container_unit_bases(*ancestor_node).apply_to(&mut context);
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
            // The context resolves every length a cascaded font-size holds.
            if batch.status == FontSizeRecascadeStatus::NeedsCppLengthResolution {
                debug_assert!(false, "the recascade left a font-size length unresolved");
                return None;
            }
        }
        Some(MonospaceRecascade::Size(
            batch.current_size_raw,
            batch.depends_on_viewport_metrics,
        ))
    }

    /// The tree scope whose `@font-feature-values` an element's `font-variant-alternates` names
    /// features through: the nearest around it, through the trees its hosts are in, that declares
    /// some, or the document's. Those of the trees between add nothing.
    pub(in crate::css::style) fn font_feature_values_scope(&self, node: StyleNodeID) -> TreeScopeID {
        let declaring = self
            .font_resolution
            .as_ref()
            .map_or(&[][..], |resolutions| resolutions.feature_values_shadow_scopes());
        let mut scope = self.tree.tree_scope(node);
        while scope != TreeScopeID::DOCUMENT && !declaring.contains(&scope) {
            let Some(host) = self.scope_root(scope).and_then(|root| self.tree.host_of(root)) else {
                return TreeScopeID::DOCUMENT;
            };
            scope = self.tree.tree_scope(host);
        }
        scope
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
        counters: &mut Counters,
    ) -> Drive<PartialDrive> {
        let random_base_values = store
            .drive_random_base_values(self, node)
            .or_refused()
            .inspect_err(|_| counters.bump(Counter::EngineComputedRecordBailValue))?;
        let resource_contexts = store.drive_resource_contexts(self);
        let reads_container_units = store.reads_container_units(self);
        let document_base_url = &self.document_resource_contexts.document_base_url;
        let store = store.view(self);
        use crate::css::computed_value_types::{STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX};
        use crate::css::style_compute::{
            FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiLengthResolutionContext, FfiStyleComputationEnvironment,
            LONGHAND_DRIVE_PHASE_REMAINING, drive_property_computation, empty_longhand_driver_results,
            is_required_driver_input, parent_snapshot_for_style_record, property_computation_order_for_phase,
        };

        // The record being driven again has a view. Without one, what the selection leaves standing
        // is unknown: the caller drives in full.
        let Some(view) = self.computed_group_sets.style_record_view(old_style_record.raw()) else {
            debug_assert!(false, "the record being driven again has a view");
            return Ok(PartialDrive::DriverInputMoved);
        };
        if !view.animated_overlay.is_null() {
            counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return Err(Unanswered::Refused);
        }
        // A record holding no table has no slots to copy the unselected properties from, and one
        // under display:none may have kept values its moved ancestors no longer pass on: the caller
        // drives either in full.
        let Some(old_table) =
            (unsafe { view.longhand_table.as_ref() }).filter(|_| view.dependency_flags & IN_DISPLAY_NONE_SUBTREE == 0)
        else {
            return Ok(PartialDrive::DriverInputMoved);
        };
        let parent = self
            .record_inheritance_parent(node)
            .inspect_err(|_| counters.bump(Counter::EngineComputedRecordBailRecordParent))?;
        let snapshot = match parent.and_then(|parent| self.computed_group_sets.assigned_style_record(parent)) {
            None => None,
            Some(record) => {
                let Some(view) = self.computed_group_sets.style_record_view(record.raw()) else {
                    debug_assert!(false, "an assigned parent record has a view");
                    counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return Err(Unanswered::Refused);
                };
                // A child inherits what the parent's animations sampled over its record.
                Some(parent_snapshot_for_style_record(self, record.raw(), unsafe {
                    view.animated_overlay.as_ref()
                }))
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
        let mut length = FfiLengthResolutionContext {
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
        if reads_container_units {
            self.container_unit_bases(node).apply_to(&mut length);
        }
        // No element fact reaches the remaining phase through this environment but the element's
        // place among its siblings: the moved properties were checked not to need one, and the
        // required driver inputs are compared against the record below.
        let sibling_position = self.sibling_position(node);
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
            has_tree_counting_context: sibling_position.is_some(),
            sibling_count: sibling_position.map_or(0, |position| u64::from(position.count)),
            sibling_index: sibling_position.map_or(0, |position| u64::from(position.index)),
            random_base_values: random_base_values.as_ptr(),
            random_base_value_count: random_base_values.len(),
            document_base_url: document_base_url.as_ptr(),
            document_base_url_length: document_base_url.len(),
            style_sheet_resource_contexts: resource_contexts.as_ptr(),
            style_sheet_resource_context_count: resource_contexts.len(),
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
        // A tree-counting value is admitted only where the retained tree places the element.
        if results.uses_tree_counting_function && sibling_position.is_none() {
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
        Ok(PartialDrive::Driven(DrivenTable {
            table,
            length,
            longhand_evaluations: results.longhand_evaluations,
            font: None,
            explicitly_inherited_groups: results.explicitly_inherited_non_inherited_style_groups,
        }))
    }

    /// What the box-type transformation of a style reads besides its own display, position and
    /// float: the facts the host published for the element and the display of `parent`, the
    /// element it inherits from, past any display:contents ancestor, as its record composes it.
    /// A record the engine drives and a composition the host samples over it are transformed
    /// against the same input.
    pub(in crate::css::style) fn box_type_transformation_input(
        &self,
        facts: u32,
        target: crate::css::style_compute::FfiStyleAdjustmentTarget,
        parent: Option<StyleNodeID>,
    ) -> crate::css::style_compute::FfiBoxTypeTransformationInput {
        use crate::css::style_compute::effective_display;

        let mut parent_display = None;
        let mut ancestor = parent;
        while let Some(current) = ancestor
            && let Some(record) = self.computed_group_sets.assigned_style_record(current)
            && let Some(view) = self.computed_group_sets.style_record_view(record.raw())
            && let Some(table) = unsafe { view.longhand_table.as_ref() }
        {
            let display = effective_display(table, unsafe { view.animated_overlay.as_ref() });
            if !display.is_contents() {
                parent_display = Some(display);
                break;
            }
            ancestor = self.tree.inheritance_parent(current);
        }
        crate::css::style_compute::rust_box_type_transformation_input(
            facts,
            target,
            parent_display.is_some(),
            parent_display.unwrap_or_else(crate::css::display::FfiDisplay::block),
        )
    }

    /// The box-type transformation input of the composition the host samples over the record of
    /// `node`, or of its pseudo-element of `pseudo_kind`, which inherits from the element.
    pub(crate) fn composition_box_type_transformation_input(
        &self,
        node: StyleNodeID,
        pseudo_kind: u8,
    ) -> crate::css::style_compute::FfiBoxTypeTransformationInput {
        use crate::css::style_compute::FfiStyleAdjustmentTarget;

        let facts = self.computed_group_sets.adjustment_facts(node);
        if pseudo_kind == crate::css::cascaded_properties::NO_PSEUDO_ELEMENT {
            self.box_type_transformation_input(
                facts,
                FfiStyleAdjustmentTarget::Element,
                self.tree.inheritance_parent(node),
            )
        } else {
            self.box_type_transformation_input(facts, FfiStyleAdjustmentTarget::PseudoElement, Some(node))
        }
    }

    /// Drive a record through every phase: the font phase against the parent's metrics, the
    /// element's font resolved through the document's resolver, line-height and color-scheme
    /// against that font, and the remaining phase with the element facts the box-type
    /// transformation reads. Root-input preparation finishes only the font and line-height
    /// phases and preserves them for completion, and a drive that `awaits_registered_context`
    /// stops after the color-scheme phase with the context the registered custom properties it
    /// declares compute against, which it preserves likewise. Monospace default-size recascade
    /// stays in C++.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub(super) fn engine_full_drive(
        &self,
        subject: DriveSubject,
        old_style_record: Option<computed::FinalStyleRecordID>,
        store: &WinnerStore,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        font_scratch: &mut FontDriveScratch,
        goal: FontDriveGoal,
        awaits_registered_context: bool,
        counters: &mut Counters,
    ) -> Drive<FullDrive> {
        let random_base_values = store
            .drive_random_base_values(self, subject.target.node())
            .or_refused()
            .inspect_err(|_| counters.bump(Counter::EngineComputedRecordBailValue))?;
        let resource_contexts = store.drive_resource_contexts(self);
        // A pseudo-element's container-relative lengths resolve against its originating
        // element's query containers.
        let container_unit_bases = store
            .reads_container_units(self)
            .then(|| self.container_unit_bases(subject.target.node()));
        let document_base_url = &self.document_resource_contexts.document_base_url;
        let store = store.view(self);
        use crate::css::computed_value_types::{STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX};
        use crate::css::css_pixels::CssPixels;
        use crate::css::property_metadata::property_id as prop;
        use crate::css::style_compute::{
            FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiInputLineHeightMetrics, FfiLengthResolutionContext,
            FfiStyleComputationEnvironment, LONGHAND_DRIVE_PHASE_COLOR_SCHEME, LONGHAND_DRIVE_PHASE_FONT,
            LONGHAND_DRIVE_PHASE_LINE_HEIGHT, LONGHAND_DRIVE_PHASE_REMAINING, drive_property_computation,
            empty_longhand_driver_results, font_family_is_monospace,
        };
        use bridge::element_adjustment_fact as fact;

        let DriveSubject { target, parent, facts } = subject;
        let has = |bit: u32| facts & bit != 0;
        let is_document_element = has(fact::IS_DOCUMENT_ELEMENT);
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
        // The record being driven again has a view. One without is driven as an element with no
        // record is, from a fresh table.
        let old_view = old_style_record.and_then(|old_style_record| {
            let view = self.computed_group_sets.style_record_view(old_style_record.raw());
            debug_assert!(view.is_some(), "the record being driven again has a view");
            view
        });
        let old_table = match &old_view {
            Some(view) => {
                if !view.animated_overlay.is_null() {
                    counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return Err(Unanswered::Refused);
                }
                // A record holding no table is driven from a fresh one, like a first record.
                unsafe { view.longhand_table.as_ref() }
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
                ))
            }
            None => None,
        };
        // A highlight pseudo-element inherits its applicable properties from the same
        // pseudo-element of the nearest ancestor.
        let highlight = pseudo_kind::is_highlight(target.pseudo_kind()).then(|| {
            let snapshot = self
                .retained_highlight_inheritance_parent_style_record(target.node(), target.pseudo_kind())
                .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
                .and_then(|view| {
                    let table = unsafe { view.longhand_table.as_ref() }?;
                    Some(crate::css::style_compute::ParentSnapshot::new(
                        table,
                        unsafe { view.animated_overlay.as_ref() },
                        view.dependency_flags & (1 << 1) != 0,
                    ))
                });
            crate::css::style_compute::HighlightInheritance {
                pseudo_kind: target.pseudo_kind(),
                snapshot,
            }
        });
        // The subject axis is the element's own writing mode when it has one, else its parent's;
        // the initial writing mode is horizontal.
        let inherited_box_payload = old_view
            .as_ref()
            .or(parent_view.as_ref())
            .map(|view| view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX]);
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
        // A tree-counting function on a pseudo-element counts its originating element's siblings.
        let sibling_position = self.sibling_position(subject.target.node());
        let environment = FfiStyleComputationEnvironment {
            box_type_input: self.box_type_transformation_input(
                facts,
                crate::css::style_compute::FfiStyleAdjustmentTarget::Element,
                parent,
            ),
            color_scheme_input: FfiEffectiveColorSchemeInput {
                preferred_color_scheme: inputs.preferred_color_scheme,
                has_document_supported_schemes: inputs.has_document_supported_schemes,
                document_supported_scheme_codes: inputs.document_supported_scheme_codes.as_ptr(),
                document_supported_scheme_count: usize::from(inputs.document_supported_scheme_count),
            },
            is_th_element: has(fact::IS_TH),
            has_new_font_size: recascaded_font_size.is_some(),
            has_tree_counting_context: sibling_position.is_some(),
            sibling_count: sibling_position.map_or(0, |position| u64::from(position.count)),
            sibling_index: sibling_position.map_or(0, |position| u64::from(position.index)),
            random_base_values: random_base_values.as_ptr(),
            random_base_value_count: random_base_values.len(),
            document_base_url: document_base_url.as_ptr(),
            document_base_url_length: document_base_url.len(),
            style_sheet_resource_contexts: resource_contexts.as_ptr(),
            style_sheet_resource_context_count: resource_contexts.len(),
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
        let length_context = |font_metrics: FfiFontMetrics,
                              font_metrics_depend_on_viewport_metrics: bool,
                              root_font_metrics: FfiFontMetrics,
                              root_font_metrics_depend_on_viewport_metrics: bool| {
            let mut length = FfiLengthResolutionContext {
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
            if let Some(bases) = &container_unit_bases {
                bases.apply_to(&mut length);
            }
            length
        };
        // A drive another row left behind is dropped rather than taken up.
        let resumed = font_scratch
            .pending
            .take()
            .filter(|pending| pending.target == target)
            .and_then(|pending| pending.progress);
        let resuming = resumed.is_some();
        let root_font_complete = resumed.as_ref().is_some_and(|pending| pending.root_font_complete);
        let color_scheme_complete = resumed.as_ref().is_some_and(|pending| pending.color_scheme_complete);
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
                     input_line_height_metrics: *const FfiInputLineHeightMetrics| unsafe {
            let evaluations_before = results.longhand_evaluations;
            drive_property_computation(
                std::ptr::from_mut(table),
                std::ptr::null_mut(),
                &store,
                snapshot.as_ref(),
                highlight.as_ref(),
                &raw const environment,
                u32::MAX,
                std::ptr::null(),
                phase,
                length,
                input_line_height_metrics,
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
            );
            // A recascaded size that read the viewport makes the element's style and font metrics
            // read it, as C++ marks them beside the size it writes.
            if recascaded_font_size_reads_viewport {
                results.depends_on_viewport_metrics = true;
                results.font_metrics_depend_on_viewport_metrics = true;
            }
        }

        // The element's own font, resolved as the C++ font computer would for these values.
        let request = engine_sample::font_resolution_request(&table, None, inputs, || {
            self.font_feature_values_scope(target.node())
        });
        let font_size = request.font_size();
        let Some(resolved) = self
            .font_resolution
            .as_ref()
            .and_then(|resolutions| resolutions.lookup(request))
        else {
            let progress = DriveProgress {
                root_font_complete: false,
                color_scheme_complete: false,
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
            );
        }

        let line_height_before_adjustments = engine_sample::used_line_height(&table, None, &request, &resolved);
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
                    color_scheme_complete: false,
                    recascaded_font_size,
                    table,
                    results,
                    effective_color_scheme,
                    resolved_viewport_relative_length,
                }),
            });
            return Ok(FullDrive::RootInputs(root_inputs));
        }
        if !color_scheme_complete {
            drive(
                counters,
                &mut table,
                &mut results,
                &mut effective_color_scheme,
                LONGHAND_DRIVE_PHASE_COLOR_SCHEME,
                std::ptr::null(),
                std::ptr::null(),
            );
        }
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
        // A registered custom property computes against the font and color scheme settled now, as
        // the host finalizes one after the line-height phase; the winners the remaining phase reads
        // substitute what it computes to.
        if awaits_registered_context {
            let context = custom_property_cascade::RegisteredValueContext {
                length: FfiLengthResolutionContext {
                    resolved_viewport_relative_length: std::ptr::null_mut(),
                    ..remaining_length
                },
                color_scheme: u8::try_from(effective_color_scheme).unwrap_or(inputs.preferred_color_scheme),
            };
            font_scratch.pending = Some(PendingFontDrive {
                target,
                progress: Some(DriveProgress {
                    root_font_complete: true,
                    color_scheme_complete: true,
                    recascaded_font_size,
                    table,
                    results,
                    effective_color_scheme,
                    resolved_viewport_relative_length,
                }),
            });
            return Ok(FullDrive::AwaitsRegisteredContext(context));
        }
        let input_line_height_metrics = if has(fact::CHECK_INPUT_LINE_HEIGHT) {
            FfiInputLineHeightMetrics {
                current_line_height: line_height_before_adjustments,
                minimum_line_height: engine_sample::normal_line_height(&resolved),
            }
        } else {
            FfiInputLineHeightMetrics {
                current_line_height: 0.0,
                minimum_line_height: 0.0,
            }
        };
        drive(
            counters,
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_REMAINING,
            &raw const remaining_length,
            &raw const input_line_height_metrics,
        );
        // A tree-counting value is admitted only where the retained tree places the element.
        if results.uses_tree_counting_function && sibling_position.is_none() {
            counters.bump(Counter::EngineComputedRecordBailDrive);
            return Err(Unanswered::Refused);
        }
        let line_height_used_after = engine_sample::used_line_height(&table, None, &request, &resolved);
        let font = engine_sample::font_group_build_inputs(&table, None, &request, line_height_used_after, &resolved);
        let length = FfiLengthResolutionContext {
            resolved_viewport_relative_length: std::ptr::null_mut(),
            ..remaining_length
        };
        Ok(FullDrive::Driven(DrivenTable {
            table,
            length,
            longhand_evaluations: results.longhand_evaluations,
            font: Some(font),
            explicitly_inherited_groups: results.explicitly_inherited_non_inherited_style_groups,
        }))
    }
}

/// A font's pixel metric as the drive resolves font-relative units against it: the C++ length
/// resolution context carries the metrics as `CSSPixels`, so an `ex` resolves against the
/// fixed-point x-height rather than the font's raw floating-point one.
pub(in crate::css::style) fn drive_font_metric(value: f32) -> f64 {
    crate::css::css_pixels::CssPixels::nearest_value_for_f32(value).to_double()
}
