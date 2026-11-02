/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::super::publication::pseudo_kind;
use super::super::*;
use super::compute::DriveSubject;
use super::winner_store::WinnerStore;
use crate::css::cascaded_properties::CascadedValues;
use crate::css::computed_longhand_table::FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS;
use crate::css::computed_values::GroupValuesWriter;

/// Where a drive reads the records of the elements it computes against: its inheritance parent's,
/// and its ancestors' for the box-type transformation and the monospace font-size recascade.
pub(in crate::css::style) trait DriveRecords:
    super::super::container_queries::ContainerRecords
{
    fn assigned_record_view(&self, node: StyleNodeID) -> Option<computed::StyleRecordView<'_>>;

    /// The record the highlight pseudo-element of `kind` of `node` inherits from: that of the same
    /// pseudo-element of its nearest flat-tree ancestor holding one.
    fn highlight_inheritance_parent_view(&self, node: StyleNodeID, kind: u8) -> Option<computed::StyleRecordView<'_>>;
}

impl DriveRecords for RetainedState {
    fn assigned_record_view(&self, node: StyleNodeID) -> Option<computed::StyleRecordView<'_>> {
        self.computed_group_sets
            .assigned_style_record(node)
            .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
    }

    fn highlight_inheritance_parent_view(&self, node: StyleNodeID, kind: u8) -> Option<computed::StyleRecordView<'_>> {
        self.retained_highlight_inheritance_parent_style_record(node, kind)
            .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
    }
}

/// Why a row has no record yet. A suspended row waits on a request the host
/// services between passes, and the same row resumes once that request is answered.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(in crate::css::style) enum Unanswered {
    Suspended(Suspension),
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
    /// A random function's base value is not drawn yet; the drive named its random caching key.
    RandomBaseValues,
}

/// A row's answer from the engine, or why there is none.
pub(in crate::css::style) type Drive<T> = Result<T, Unanswered>;

/// What the monospace font-size recascade answers for a drive target.
pub(in crate::css::style) enum MonospaceRecascade {
    /// The font size the recascade reaches, and whether reaching it read the viewport.
    Size(i32, bool),
    /// A length in the ancestor chain resolves against the monospace font at the size reached so
    /// far, which the font resolver has not resolved yet.
    AwaitsFont(bridge::FfiFontResolutionRequest),
}

/// Driven values, with the length context they were driven against and the font a full drive
/// resolved.
pub(in crate::css::style) struct DrivenValues<'a> {
    pub(in crate::css::style) writer: GroupValuesWriter<'a>,
    pub(in crate::css::style) length: crate::css::style_compute::FfiLengthResolutionContext,
    pub(in crate::css::style) font: Option<crate::css::table_group_builder::FfiFontGroupBuildInputs>,
}

#[derive(Clone, Default)]
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

    /// The font request a drive suspended on.
    pub(in crate::css::style) fn take_suspended_request(&mut self) -> font_resolution::FontRequest {
        self.request
            .take()
            .expect("a drive suspended on a font names its request")
    }
}

/// A drive left for its target's retry. No parent/context borrow survives refill; the caller
/// resumes the same subject before evaluating any later canonical element.
#[derive(Clone)]
struct PendingFontDrive {
    /// What the suspended drive belongs to. The record loop can settle that element another way
    /// before the retry comes, which leaves the drive behind; whoever is driven next must not
    /// take up someone else's table.
    target: computed::ComputedStyleTarget,
    /// What the drive completed, or nothing when the monospace recascade waited on a font before
    /// the drive began: the retry then starts the drive afresh.
    progress: Option<DriveProgress>,
}

/// The completed font phase owns its values, holding a reference to each group they were seeded
/// from.
#[derive(Clone)]
struct DriveProgress {
    root_font_complete: bool,
    /// Whether the color-scheme phase ran too, as it has for a drive awaiting its registered
    /// context.
    color_scheme_complete: bool,
    /// The monospace recascade's size the values already hold, which the remaining phases read.
    recascaded_font_size: Option<i32>,
    writer: GroupValuesWriter<'static>,
    results: crate::css::style_compute::LonghandDriverResults,
    effective_color_scheme: i16,
    resolved_viewport_relative_length: bool,
}

impl RetainedState {
    fn monospace_recascaded_font_size_with(
        &self,
        records: &impl DriveRecords,
        target: computed::ComputedStyleTarget,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
    ) -> Option<MonospaceRecascade> {
        use crate::css::css_pixels::CssPixels;
        use crate::css::style_compute::{
            FfiFontMetrics, FfiLengthResolutionContext, FontSizeRecascadeDocumentInputs, FontSizeRecascadeStatus,
            recascade_font_size_batch,
        };
        use crate::css::table_group_builder::group_index;

        let mut views = Vec::new();
        let mut ancestor = match target.is_pseudo() {
            true => Some(target.node()),
            false => self.projection.inheritance_parent(target.node()),
        };
        while let Some(node) = ancestor {
            views.push((node, records.assigned_record_view(node)));
            ancestor = self.projection.inheritance_parent(node);
        }
        views.reverse();
        let mut inherited_zoom = 1.0;
        let zooms: Vec<_> = views
            .iter()
            .map(|(_, view)| {
                if let Some(view) = view {
                    inherited_zoom = f64::from(view.longhands().effective_zoom());
                }
                inherited_zoom
            })
            .collect();
        let applies_zoom_at = |index: usize| {
            self.projection.adjustment_facts(views[index].0) & bridge::element_adjustment_fact::IS_SVG_ELEMENT == 0
        };
        let value_at = |index: usize| {
            views[index]
                .1
                .as_ref()
                .and_then(|view| view.longhands().raw_cascaded_font_size())
        };
        let default_size = CssPixels::from_integer(13).raw_value();
        let document_inputs = FontSizeRecascadeDocumentInputs::from_document(inputs);
        let mut batch = recascade_font_size_batch(
            views.len(),
            value_at,
            |index| zooms[index],
            applies_zoom_at,
            0,
            default_size,
            false,
            default_size,
            document_inputs,
            None,
        );
        while batch.status != FontSizeRecascadeStatus::Complete {
            let index = batch.next_index;
            let zoom = zooms[index];
            let parent_zoom = index.checked_sub(1).map_or(1.0, |parent| zooms[parent]);
            let applies_zoom = applies_zoom_at(index);
            let own_zoom = if applies_zoom { zoom / parent_zoom } else { 1.0 };
            let inherited_size = CssPixels::from_raw(batch.current_size_raw).scaled(own_zoom);
            let root_zoom = if !applies_zoom {
                1.0
            } else if inputs.root_effective_zoom > 0.0 {
                zoom / inputs.root_effective_zoom
            } else {
                zoom
            };
            // AD-HOC: C++ measures the platform's monospace font directly, where this resolves the
            //         generic `monospace` family like any other request. The two differ only where
            //         an @font-face takes the platform monospace font's name.
            let request = bridge::FfiFontResolutionRequest {
                font_family: bridge::FfiHostHandle::from_pointer(self.monospace_font_family.pointer().cast()),
                font_feature_values: [bridge::FfiHostHandle::default(); bridge::FONT_RESOLUTION_FEATURE_INPUT_COUNT],
                font_size_raw: inherited_size.raw_value(),
                font_slope: 0,
                font_weight: 400.0,
                font_width: 100.0,
                font_optical_sizing: 0,
                font_size_adjust: bridge::FontSizeAdjustKind::None,
                font_size_adjust_metric: 0,
                font_size_adjust_value: 0.0,
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
            // NB: The ancestor's own font is the one being recascaded; its line height is the one it
            //     inherits, or the initial font's normal line height when there is no parent.
            let (line_height, inherited_font_metrics_depend_on_viewport_metrics) = index
                .checked_sub(1)
                .and_then(|parent| views[parent].1.as_ref())
                .map_or((inputs.initial_line_height, false), |view| {
                    let font = unsafe {
                        view.payloads[group_index::FONT]
                            .cast::<crate::css::computed_value_types::FontValues>()
                            .deref()
                    };
                    (
                        font.line_height_used().to_double() * own_zoom,
                        view.dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS != 0,
                    )
                });
            let (ancestor_node, ancestor_view) = &views[index];
            let subject_inline_axis_is_horizontal = ancestor_view.as_ref().is_none_or(|view| {
                view.payloads.inherited_bits().writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB
            });
            let mut context = FfiLengthResolutionContext {
                viewport_width: inputs.viewport_width,
                viewport_height: inputs.viewport_height,
                font_metrics: FfiFontMetrics {
                    font_size: inherited_size.to_double(),
                    x_height: drive_font_metric(resolved.x_height),
                    cap_height: drive_font_metric(resolved.ascent),
                    zero_advance: drive_font_metric(resolved.zero_advance),
                    line_height,
                },
                root_font_metrics: FfiFontMetrics {
                    font_size: inputs.root_font_size * root_zoom,
                    x_height: inputs.root_font_x_height * root_zoom,
                    cap_height: inputs.root_font_cap_height * root_zoom,
                    zero_advance: inputs.root_font_zero_advance * root_zoom,
                    line_height: inputs.root_line_height * root_zoom,
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
                zoom: if applies_zoom { zoom } else { 1.0 },
            };
            // The ancestor's container-relative font size resolves against its own containers.
            self.container_unit_bases_with(records, *ancestor_node, false)
                .apply_to(&mut context);
            batch = recascade_font_size_batch(
                views.len(),
                value_at,
                |index| zooms[index],
                applies_zoom_at,
                index,
                batch.current_size_raw,
                batch.depends_on_viewport_metrics,
                default_size,
                document_inputs,
                Some(&context),
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
        let mut scope = self.projection.tree_scope(node);
        while scope != TreeScopeID::DOCUMENT && !declaring.contains(&scope) {
            let Some(host) = self.scope_root(scope).and_then(|root| self.projection.host_of(root)) else {
                return TreeScopeID::DOCUMENT;
            };
            scope = self.projection.tree_scope(host);
        }
        scope
    }

    /// What the box-type transformation of a style reads besides its own display, position and
    /// float: the facts the host published for the element and the display of `parent`, the
    /// element it inherits from, past any display:contents ancestor, as its record composes it.
    /// A record the engine drives and a composition the host samples over it are transformed
    /// against the same input.
    pub(in crate::css::style) fn box_type_transformation_input(
        &self,
        facts: u32,
        target: crate::css::style_compute::StyleAdjustmentTarget,
        parent: Option<StyleNodeID>,
    ) -> crate::css::style_compute::FfiBoxTypeTransformationInput {
        self.box_type_transformation_input_with(self, facts, target, parent)
    }

    pub(in crate::css::style) fn box_type_transformation_input_with(
        &self,
        records: &impl DriveRecords,
        facts: u32,
        target: crate::css::style_compute::StyleAdjustmentTarget,
        parent: Option<StyleNodeID>,
    ) -> crate::css::style_compute::FfiBoxTypeTransformationInput {
        let mut parent_display = None;
        let mut parent_writing_mode = crate::css::css_enums::writing_mode::HORIZONTAL_TB;
        let mut ancestor = parent;
        while let Some(current) = ancestor {
            let Some(view) = records.assigned_record_view(current) else {
                break;
            };
            let longhands = view.longhands();
            let display = longhands.effective_display(view.animated_overlay);
            let writing_mode = longhands.effective_writing_mode(view.animated_overlay);
            if !display.is_contents() {
                parent_display = Some(display);
                parent_writing_mode = writing_mode;
                break;
            }
            ancestor = self.projection.inheritance_parent(current);
        }
        crate::css::style_compute::rust_box_type_transformation_input(
            facts,
            target,
            parent_display.is_some(),
            parent_display.unwrap_or_else(crate::css::display::FfiDisplay::block),
            parent_writing_mode,
        )
    }

    /// The box-type transformation input of the composition the host samples over the record of
    /// `node`, or of its pseudo-element of `pseudo_kind`, which inherits from the element.
    pub(crate) fn composition_box_type_transformation_input(
        &self,
        node: StyleNodeID,
        pseudo_kind: u8,
    ) -> crate::css::style_compute::FfiBoxTypeTransformationInput {
        use crate::css::style_compute::StyleAdjustmentTarget;

        let facts = self.projection.adjustment_facts(node);
        if pseudo_kind == crate::css::cascaded_properties::NO_PSEUDO_ELEMENT {
            self.box_type_transformation_input(
                facts,
                StyleAdjustmentTarget::Element,
                self.projection.inheritance_parent(node),
            )
        } else {
            self.box_type_transformation_input(facts, StyleAdjustmentTarget::PseudoElement, Some(node))
        }
    }

    /// Drive a record through every phase: the font phase against the parent's metrics, the
    /// element's font resolved through the document's resolver, line-height and color-scheme
    /// against that font, and the remaining phase with the element facts the box-type
    /// transformation reads. Root-input preparation finishes only the font and line-height
    /// phases and preserves them for completion.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub(in crate::css::style) fn engine_full_drive<'a>(
        &self,
        subject: DriveSubject,
        writer: GroupValuesWriter<'a>,
        store: &WinnerStore,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        font_scratch: &mut FontDriveScratch,
    ) -> Drive<DrivenValues<'a>> {
        self.engine_drive_with(self, subject, writer, None, store, inputs, font_scratch)
    }

    /// Drive a record as [`Self::engine_full_drive`] does, into `writer`, reading the records it
    /// computes against from `records`. With a `selected` set of longhands, the drive evaluates
    /// only those, beside the inputs every drive evaluates, and keeps the writer's values for the
    /// rest.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub(in crate::css::style) fn engine_drive_with<'a>(
        &self,
        records: &impl DriveRecords,
        subject: DriveSubject,
        writer: GroupValuesWriter<'a>,
        selected: Option<&[u64]>,
        store: &WinnerStore,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        font_scratch: &mut FontDriveScratch,
    ) -> Drive<DrivenValues<'a>> {
        let random_base_values = store
            .drive_random_base_values(self, subject.target.node())
            .expect("a random function's base values are drawn before the row is driven");
        let resource_contexts = store.drive_resource_contexts(self);
        // NB: Include the originating element when selecting a pseudo-element's query containers.
        let container_unit_bases = (store.container_unit_mask(self) != 0)
            .then(|| self.container_unit_bases_with(records, subject.target.node(), subject.target.is_pseudo()));
        let document_base_url = &self.document_resource_contexts.document_base_url;
        let store = store.view(self);
        use crate::css::css_pixels::CssPixels;
        use crate::css::property_metadata::property_id as prop;
        use crate::css::style_compute::{
            FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiInputLineHeightMetrics, FfiLengthResolutionContext,
            FfiStyleComputationEnvironment, LONGHAND_DRIVE_PHASE_COLOR_SCHEME, LONGHAND_DRIVE_PHASE_FONT,
            LONGHAND_DRIVE_PHASE_LINE_HEIGHT, LONGHAND_DRIVE_PHASE_REMAINING, drive_property_computation,
            empty_longhand_driver_results, font_family_is_monospace,
        };
        use crate::css::table_group_builder::group_index;
        use bridge::element_adjustment_fact as fact;

        let DriveSubject { target, parent, facts } = subject;
        let has = |bit: u32| facts & bit != 0;
        let is_document_element = has(fact::IS_DOCUMENT_ELEMENT);
        // HACK: A cascade that ends in `font-family: monospace` re-runs the font-size cascade over
        //       the whole ancestor chain against a 13px default instead of the 16px one, which
        //       changes what a keyword size an ancestor declared means.
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
            .is_some_and(|(value, ..)| font_family_is_monospace(value.data()))
        {
            match self.monospace_recascaded_font_size_with(records, target, inputs) {
                Some(MonospaceRecascade::Size(recascaded, reads_viewport)) => {
                    recascaded_font_size_reads_viewport = reads_viewport;
                    Some(recascaded)
                }
                Some(MonospaceRecascade::AwaitsFont(request)) => {
                    return Err(font_scratch.suspend(target, request, None));
                }
                None => {
                    unreachable!("the style engine answers every row");
                }
            }
        } else {
            None
        };
        let parent_view = match parent {
            Some(parent) => {
                let Some(parent_view) = records.assigned_record_view(parent) else {
                    return Err(Unanswered::AwaitsParent);
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
            line_height: inputs.initial_line_height,
        };
        let (parent_metrics, parent_font_metrics_depend_on_viewport_metrics, parent_line_height_used) =
            match &parent_view {
                Some(parent_view) => {
                    let parent_font = unsafe {
                        parent_view.payloads[group_index::FONT]
                            .cast::<crate::css::computed_value_types::FontValues>()
                            .deref()
                    };
                    let first_available = parent_font.first_available_font_metrics();
                    (
                        FfiFontMetrics {
                            font_size: parent_font.font_size.to_double(),
                            x_height: drive_font_metric(first_available.x_height),
                            cap_height: drive_font_metric(first_available.ascent),
                            zero_advance: drive_font_metric(first_available.zero_advance),
                            line_height: parent_font.line_height_used().to_double(),
                        },
                        parent_view.dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS != 0,
                        parent_font.line_height_used().to_double(),
                    )
                }
                None => (initial_metrics, false, inputs.initial_line_height),
            };
        let snapshot = match &parent_view {
            Some(parent_view) => {
                let parent_longhands = parent_view.longhands();
                Some(crate::css::style_compute::ParentSnapshot::from_view(
                    parent_longhands,
                    parent_view.animated_overlay,
                    parent_font_metrics_depend_on_viewport_metrics,
                ))
            }
            None => None,
        };
        // A highlight pseudo-element inherits its applicable properties from the same
        // pseudo-element of the nearest ancestor.
        let highlight = pseudo_kind::is_highlight(target.pseudo_kind()).then(|| {
            let snapshot = records
                .highlight_inheritance_parent_view(target.node(), target.pseudo_kind())
                .map(|view| {
                    crate::css::style_compute::ParentSnapshot::from_view(
                        view.longhands(),
                        view.animated_overlay,
                        view.dependency_flags & (1 << 1) != 0,
                    )
                });
            crate::css::style_compute::HighlightInheritance {
                pseudo_kind: target.pseudo_kind(),
                snapshot,
            }
        });
        // The subject axis is the element's own writing mode: the keyword its winner declares,
        // which is its computed value, else the one it held, else its parent's; the initial
        // writing mode is horizontal.
        let declared_writing_mode =
            store
                .winning_declaration(prop::WRITING_MODE)
                .and_then(|(value, ..)| match value.data() {
                    StyleValueData::Keyword {
                        keyword: crate::css::style_compute::keyword::INITIAL,
                    } => Some(crate::css::css_enums::writing_mode::HORIZONTAL_TB),
                    StyleValueData::Keyword { keyword } => crate::css::style_compute::keyword_to_writing_mode(*keyword),
                    _ => None,
                });
        let subject_inline_axis_is_horizontal = match declared_writing_mode {
            Some(writing_mode) => writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB,
            None => parent_view.as_ref().is_none_or(|view| {
                view.payloads.inherited_bits().writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB
            }),
        };
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
            box_type_input: self.box_type_transformation_input_with(
                records,
                facts,
                crate::css::style_compute::StyleAdjustmentTarget::Element,
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
            root_effective_zoom: inputs.root_effective_zoom,
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
                zoom: 1.0,
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
            self.counters.bump(Counter::EngineFullDrivesStarted);
        }
        let (mut writer, mut results, mut effective_color_scheme) = match resumed {
            Some(pending) => {
                resolved_viewport_relative_length = pending.resolved_viewport_relative_length;
                if !pending.root_font_complete {
                    self.counters.bump(Counter::FontRefillResumedDrives);
                    self.counters.add(
                        Counter::FontRefillPreservedLonghands,
                        u64::from(pending.results.longhand_evaluations),
                    );
                }
                (pending.writer, pending.results, pending.effective_color_scheme)
            }
            None => (writer, empty_longhand_driver_results(), -1),
        };
        let drive = |counters: &Counters,
                     writer: &mut GroupValuesWriter<'_>,
                     results: &mut crate::css::style_compute::LonghandDriverResults,
                     effective_color_scheme: &mut i16,
                     phase: u8,
                     length: Option<&FfiLengthResolutionContext>,
                     input_line_height_metrics: Option<&FfiInputLineHeightMetrics>| unsafe {
            let evaluations_before = results.longhand_evaluations;
            drive_property_computation(
                writer,
                &store,
                snapshot.as_ref(),
                highlight.as_ref(),
                &environment,
                selected,
                phase,
                length,
                input_line_height_metrics,
                results,
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
                writer.set_computed(
                    prop::FONT_SIZE,
                    StyleValueData::Length {
                        value: CssPixels::from_raw(recascaded).to_double(),
                        unit: crate::css::style_compute::px_length_unit(),
                    },
                );
            }
            drive(
                &self.counters,
                &mut writer,
                &mut results,
                &mut effective_color_scheme,
                LONGHAND_DRIVE_PHASE_FONT,
                Some(&font_length),
                None,
            );
            // A recascaded size that read the viewport makes the element's style and font metrics
            // read it, as C++ marks them beside the size it writes.
            if recascaded_font_size_reads_viewport {
                results.depends_on_viewport_metrics = true;
                results.font_metrics_depend_on_viewport_metrics = true;
            }
        }

        // The element's own font, resolved as the C++ font computer would for these values.
        let request = engine_sample::font_resolution_request(
            |property| Some(writer.effective_value(property)),
            inputs,
            || self.font_feature_values_scope(target.node()),
        );
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
                writer: writer.detached(),
                results,
                effective_color_scheme,
                resolved_viewport_relative_length,
            };
            return Err(font_scratch.suspend(target, request, Some(progress)));
        };
        if request.font_size_adjust == bridge::FontSizeAdjustKind::FromFont {
            writer.set_computed(
                prop::FONT_SIZE_ADJUST,
                engine_sample::computed_font_size_adjust_from_font(&request, &resolved),
            );
        }
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
                &self.counters,
                &mut writer,
                &mut results,
                &mut effective_color_scheme,
                LONGHAND_DRIVE_PHASE_LINE_HEIGHT,
                Some(&line_height_length),
                None,
            );
        }

        let line_height_before_adjustments =
            engine_sample::used_line_height(|property| Some(writer.effective_value(property)), &request, &resolved);
        if !color_scheme_complete {
            drive(
                &self.counters,
                &mut writer,
                &mut results,
                &mut effective_color_scheme,
                LONGHAND_DRIVE_PHASE_COLOR_SCHEME,
                None,
                None,
            );
        }
        effective_color_scheme = writer.effective_color_scheme();

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
                minimum_line_height: engine_sample::normal_line_height(&resolved),
            }
        } else {
            FfiInputLineHeightMetrics {
                current_line_height: 0.0,
                minimum_line_height: 0.0,
            }
        };
        drive(
            &self.counters,
            &mut writer,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_REMAINING,
            Some(&remaining_length),
            Some(&input_line_height_metrics),
        );
        // A tree-counting value is admitted only where the retained tree places the element.
        if results.uses_tree_counting_function && sibling_position.is_none() {
            unreachable!("the style engine answers every row");
        }
        let values = |property| Some(writer.effective_value(property));
        let line_height_used_after = engine_sample::used_line_height(values, &request, &resolved);
        let font = engine_sample::font_group_build_inputs(values, &request, line_height_used_after, &resolved);
        let length = FfiLengthResolutionContext {
            resolved_viewport_relative_length: std::ptr::null_mut(),
            ..remaining_length
        };
        Ok(DrivenValues {
            writer,
            length,
            font: Some(font),
        })
    }
}

/// A font's pixel metric as the drive resolves font-relative units against it: the C++ length
/// resolution context carries the metrics as `CSSPixels`, so an `ex` resolves against the
/// fixed-point x-height rather than the font's raw floating-point one.
pub(in crate::css::style) fn drive_font_metric(value: f32) -> f64 {
    crate::css::css_pixels::CssPixels::nearest_value_for_f32(value).to_double()
}
