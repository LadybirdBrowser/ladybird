/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Container queries, evaluated over the retained container facts: the containers' published
//! container query inputs, the previous layout's boxes and the containers' style records.

use super::bridge::FfiContainerEffectKind;
use super::tree::StyleNodeID;
use super::*;
use crate::css::computed_longhand_table::FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS;
use crate::css::parser::query_parser::{
    CONTAINER_QUERY_HAS_UNKNOWN_FEATURE, CONTAINER_QUERY_REQUIRES_BLOCK_SIZE, CONTAINER_QUERY_REQUIRES_HEIGHT,
    CONTAINER_QUERY_REQUIRES_INLINE_SIZE, CONTAINER_QUERY_REQUIRES_SCROLL_STATE, CONTAINER_QUERY_REQUIRES_STYLE,
    CONTAINER_QUERY_REQUIRES_WIDTH, ContainerFacts, FfiQueryHandle, MatchResult, SCROLL_STATE_SIDE_BOTTOM,
    SCROLL_STATE_SIDE_LEFT, SCROLL_STATE_SIDE_RIGHT, SCROLL_STATE_SIDE_TOP,
};

/// What a rule's container conditions say for one subject: whether they hold, whether they ask
/// about a container's size, scroll state or style, and what the evaluation read of the
/// containers, for the host to record.
#[derive(Clone, Default)]
pub(crate) struct ContainerVerdict {
    pub(crate) matches: bool,
    pub(crate) depends_on_size: bool,
    pub(crate) depends_on_style: bool,
    /// Whether a query was evaluated, whose lengths may be relative to the root element's font.
    pub(crate) reads_root_font: bool,
    /// The containers a query was evaluated against, whose first available font's metrics the
    /// query's `ex`, `cap`, `ch` and `lh` lengths resolve against.
    pub(crate) font_metric_containers: Vec<StyleNodeID>,
    pub(crate) effects: Vec<(StyleNodeID, FfiContainerEffectKind)>,
}

impl ContainerVerdict {
    /// Add what another evaluation read of the containers to what this one read.
    pub(crate) fn add_reads(&mut self, other: ContainerVerdict) {
        self.depends_on_size |= other.depends_on_size;
        self.depends_on_style |= other.depends_on_style;
        self.reads_root_font |= other.reads_root_font;
        for container in other.font_metric_containers {
            if !self.font_metric_containers.contains(&container) {
                self.font_metric_containers.push(container);
            }
        }
        self.effects.extend(other.effects);
    }
}

/// One physical axis' basis for a subject's container-relative lengths, as
/// `Length::container_relative_length_to_px_without_rounding` finds it: the content size of the
/// nearest flat-tree ancestor that accepts size queries on that axis as of the last layout commit,
/// zero while that container has no box, and the viewport's size when there is no such container.
#[derive(Clone, Copy)]
pub(crate) struct ContainerUnitBasis {
    basis: f64,
    /// The container the basis is read from, or none for the viewport.
    container: Option<StyleNodeID>,
    container_has_no_box: bool,
}

impl ContainerUnitBasis {
    /// The container the basis is read from, or none for the viewport.
    pub(crate) fn container(&self) -> Option<StyleNodeID> {
        self.container
    }

    /// Whether the container has no box as of the last layout commit, so its queries are evaluated again after the
    /// next layout gives it one.
    pub(crate) fn container_has_no_box(&self) -> bool {
        self.container_has_no_box
    }
}

/// The bases of both physical axes, width and height.
#[derive(Clone, Copy)]
pub(crate) struct ContainerUnitBases([ContainerUnitBasis; 2]);

impl ContainerUnitBases {
    pub(crate) fn width_and_height(&self) -> [ContainerUnitBasis; 2] {
        self.0
    }

    /// Resolve a context's container-relative lengths against these bases; one read from the
    /// viewport is a viewport dependency.
    pub(crate) fn apply_to(&self, length: &mut crate::css::style_compute::FfiLengthResolutionContext) {
        let [width, height] = self.0;
        length.has_container_width_basis = true;
        length.has_container_height_basis = true;
        length.container_width_basis = width.basis;
        length.container_height_basis = height.basis;
        length.container_width_basis_depends_on_viewport_metrics = width.container.is_none();
        length.container_height_basis_depends_on_viewport_metrics = height.container.is_none();
    }
}

/// Where container queries and container-relative lengths read what the elements above their
/// subject are as query containers: the records the host installed, or a style pass's records,
/// falling back to the installed ones for the elements the pass did not compute.
pub(in crate::css::style) trait ContainerRecords {
    /// Whether, and as what, an element is a query container.
    fn query_row(&self, node: StyleNodeID) -> Option<tree::ContainerQueryInputs<'_>>;

    /// What an element that is no query container is as the style container every element is,
    /// which a query asking only about style asks about. `None` where the element holds no style
    /// yet.
    fn any_element_row(&self, node: StyleNodeID) -> Option<tree::ContainerQueryInputs<'_>>;

    /// The style of a container `row` describes, which a query's lengths and colors compute as.
    fn record_view(
        &self,
        node: StyleNodeID,
        row: tree::ContainerQueryInputs<'_>,
    ) -> Option<computed::StyleRecordView<'_>>;

    /// The custom properties of a container `row` describes, which a `style()` feature reads, or
    /// `None` where they are not known.
    fn custom_store(
        &self,
        node: StyleNodeID,
        row: tree::ContainerQueryInputs<'_>,
    ) -> Option<Option<&crate::css::custom_properties::CustomPropertyStore>>;

    /// The font a `rem` in a query resolves against where it is not the one the document's inputs
    /// say: the font of the document element as the records hold it.
    fn root_font(&self) -> Option<super::pipeline::compute::RootFontInputs> {
        None
    }
}

impl ContainerRecords for RetainedState {
    fn query_row(&self, node: StyleNodeID) -> Option<tree::ContainerQueryInputs<'_>> {
        self.container_query_inputs(node)
    }

    /// One holding no record yet is one the host styles in this update, which decides it. One
    /// this batch settled is read as the host will leave it, as a query container is.
    fn any_element_row(&self, node: StyleNodeID) -> Option<tree::ContainerQueryInputs<'_>> {
        self.container_query_input_row(node, self.container_style_record(node)?, true)
    }

    fn record_view(
        &self,
        _node: StyleNodeID,
        row: tree::ContainerQueryInputs<'_>,
    ) -> Option<computed::StyleRecordView<'_>> {
        self.computed_group_sets.style_record_view(row.style_record)
    }

    /// The environment the element's animations sample where they do, else its record's.
    fn custom_store(
        &self,
        node: StyleNodeID,
        row: tree::ContainerQueryInputs<'_>,
    ) -> Option<Option<&crate::css::custom_properties::CustomPropertyStore>> {
        let environment = match self.element_custom_property_data.get(&node) {
            Some(held) if held.sampled_over.is_some() => held.identity,
            _ => self
                .computed_group_sets
                .style_record_custom_property_environment(row.style_record)
                .unwrap_or_default(),
        };
        match environment {
            0 => Some(None),
            identity => Some(Some(&**self.custom_property_environments.store(identity)?)),
        }
    }
}

/// What a `style()` feature reads of the container it asks about: the custom-property
/// environment of its record, and lengths and colors as the record computes them.
struct RetainedContainerStyleContext<'a> {
    store: Option<&'a crate::css::custom_properties::CustomPropertyStore>,
    registry: &'a crate::css::custom_properties::CustomPropertyRegistry,
    color: crate::css::color_resolution::ColorResolutionInput<'a>,
    /// The container's sibling count and index, which a tree-counting function in a query value
    /// resolves against.
    tree_counting: Option<(u64, u64)>,
}

/// Whether one of the container conditions asks a `style()` question.
pub(super) fn conditions_ask_container_style(
    containers: &[std::sync::Arc<crate::css::container_conditions::ContainerConditionsData>],
) -> bool {
    containers.iter().any(|conditions| {
        conditions.conditions.iter().any(|condition| {
            condition
                .query
                .as_ref()
                .is_some_and(|query| query.container_requirements() & CONTAINER_QUERY_REQUIRES_STYLE != 0)
        })
    })
}

impl RetainedState {
    /// What lengths resolve against as a record computes them, as the host takes them from an
    /// element's computed style: the record's font metrics and writing mode, the root's font
    /// metrics and the viewport, with no container to resolve container-relative lengths
    /// against. `None` for a record without a font.
    pub(super) fn record_length_resolution_context(
        &self,
        payloads: &crate::css::computed_values::StyleGroups,
        dependency_flags: u8,
    ) -> Option<crate::css::style_compute::FfiLengthResolutionContext> {
        let document = &self.document_style_computation_inputs;
        let font = unsafe {
            payloads[crate::css::table_group_builder::group_index::FONT]
                .cast::<crate::css::computed_value_types::FontValues>()
                .as_ref()
        }?;
        let values = crate::css::computed_value_views::ComputedValuesView::of(payloads);
        let first_available = font.first_available_font_metrics();
        Some(crate::css::style_compute::FfiLengthResolutionContext {
            viewport_width: document.viewport_width,
            viewport_height: document.viewport_height,
            font_metrics: crate::css::style_compute::FfiFontMetrics {
                font_size: font.font_size.to_double(),
                x_height: super::pipeline::drive::drive_font_metric(first_available.x_height),
                // The C++ metrics approximate the cap height with the ascent.
                cap_height: super::pipeline::drive::drive_font_metric(first_available.ascent),
                zero_advance: super::pipeline::drive::drive_font_metric(first_available.zero_advance),
                line_height: font.line_height_used().to_double(),
            },
            root_font_metrics: crate::css::style_compute::FfiFontMetrics {
                font_size: document.root_font_size,
                x_height: document.root_font_x_height,
                cap_height: document.root_font_cap_height,
                zero_advance: document.root_font_zero_advance,
                line_height: document.root_line_height,
            },
            font_metrics_depend_on_viewport_metrics: dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS != 0,
            root_font_metrics_depend_on_viewport_metrics: document.root_font_metrics_depend_on_viewport_metrics,
            has_container_width_basis: false,
            has_container_height_basis: false,
            container_width_basis: 0.0,
            container_height_basis: 0.0,
            container_width_basis_depends_on_viewport_metrics: false,
            container_height_basis_depends_on_viewport_metrics: false,
            subject_inline_axis_is_horizontal: values.writing_mode()
                == crate::css::css_enums::writing_mode::HORIZONTAL_TB,
            resolved_viewport_relative_length: std::ptr::null_mut(),
            zoom: 1.0,
        })
    }

    /// The color a parsed `<color>` names against a style record: the record's used color-scheme and `color`, with
    /// lengths resolving as the record computes them. With no such record, it resolves against no style at all.
    pub(crate) fn color_against_style_record(
        &self,
        style_record: u64,
        value: &crate::css::style_value::StyleValueData,
    ) -> Option<crate::css::color_resolution::Rgba> {
        use crate::css::color_resolution::{ColorResolutionInput, EMPTY_INPUT, to_color};
        let Some(view) = self.computed_group_sets.style_record_view(style_record) else {
            return to_color(value, &EMPTY_INPUT);
        };
        let length = self.record_length_resolution_context(view.payloads, view.dependency_flags);
        let values = crate::css::computed_value_views::ComputedValuesView::of(view.payloads);
        let input = ColorResolutionInput {
            length: length.as_ref(),
            ..ColorResolutionInput::for_style(values)
        };
        to_color(value, &input)
    }

    /// The basis of one physical axis for a subject's container-relative lengths.
    fn container_unit_basis(
        &self,
        records: &impl ContainerRecords,
        subject: StyleNodeID,
        axis_is_horizontal: bool,
        subject_is_pseudo_element: bool,
    ) -> ContainerUnitBasis {
        // https://drafts.csswg.org/css-conditional-5/#container-queries
        // For other pseudo-elements, query containers can be established by inclusive flat tree ancestors
        // of their originating element.
        let mut ancestor = if subject_is_pseudo_element {
            Some(subject)
        } else {
            self.projection.flat_tree_parent(subject)
        };
        while let Some(node) = ancestor {
            ancestor = self.projection.flat_tree_parent(node);
            let Some(inputs) = records.query_row(node) else {
                continue;
            };
            // The container's own writing mode decides which of its axes is the inline one, the
            // only one an `inline-size` container answers for.
            let answers_for_inline_axis = inputs.is_size_container || inputs.is_inline_size_container;
            let eligible =
                if axis_is_horizontal == (inputs.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB) {
                    answers_for_inline_axis
                } else {
                    inputs.is_size_container
                };
            if !eligible || !inputs.size_queries_available {
                continue;
            }
            let snapshot = self.layout_style_snapshot(node).unwrap_or_default();
            let raw = if axis_is_horizontal {
                snapshot.content_width_raw
            } else {
                snapshot.content_height_raw
            };
            return ContainerUnitBasis {
                basis: if snapshot.has_committed_box {
                    crate::css::css_pixels::CssPixels::from_raw(raw).to_double()
                } else {
                    0.0
                },
                container: Some(node),
                container_has_no_box: !snapshot.has_committed_box,
            };
        }
        let document = &self.document_style_computation_inputs;
        ContainerUnitBasis {
            basis: if axis_is_horizontal {
                document.viewport_width
            } else {
                document.viewport_height
            },
            container: None,
            container_has_no_box: false,
        }
    }

    /// The bases of both physical axes for a subject's container-relative lengths.
    pub(crate) fn container_unit_bases(
        &self,
        subject: StyleNodeID,
        subject_is_pseudo_element: bool,
    ) -> ContainerUnitBases {
        self.container_unit_bases_with(self, subject, subject_is_pseudo_element)
    }

    /// The bases of both physical axes for a subject's container-relative lengths, against the
    /// query containers `records` holds.
    pub(super) fn container_unit_bases_with(
        &self,
        records: &impl ContainerRecords,
        subject: StyleNodeID,
        subject_is_pseudo_element: bool,
    ) -> ContainerUnitBases {
        ContainerUnitBases([
            self.container_unit_basis(records, subject, true, subject_is_pseudo_element),
            self.container_unit_basis(records, subject, false, subject_is_pseudo_element),
        ])
    }

    /// Keep for the host what container-relative units of `unit_mask` read of the subject's
    /// containers, for a subject whose inline axis is or is not the horizontal one.
    pub(super) fn note_container_unit_reads_for_host(
        &mut self,
        node: StyleNodeID,
        unit_mask: u8,
        inline_axis_is_horizontal: bool,
        subject_is_pseudo_element: bool,
    ) {
        let (reads_width, reads_height) =
            crate::css::style_compute::container_relative_axes_read(unit_mask, inline_axis_is_horizontal);
        let mut verdict = ContainerVerdict {
            depends_on_size: true,
            ..Default::default()
        };
        for (reads, axis_is_horizontal) in [(reads_width, true), (reads_height, false)] {
            if !reads {
                continue;
            }
            let basis = self.container_unit_basis(self, node, axis_is_horizontal, subject_is_pseudo_element);
            match basis.container {
                Some(container) => {
                    verdict
                        .effects
                        .push((container, FfiContainerEffectKind::SizeContainerUsage));
                    if basis.container_has_no_box {
                        verdict
                            .effects
                            .push((container, FfiContainerEffectKind::UnitsNeedEvaluationAfterLayout));
                    }
                }
                None => verdict
                    .effects
                    .push((node, FfiContainerEffectKind::SubjectViewportDependency)),
            }
        }
        self.note_container_effects_for_host(node, &std::sync::Arc::new(verdict));
    }

    /// Evaluate container conditions for a subject, as the host evaluates them: a rule's, or those
    /// around a block of a custom function's declarations. `None` when the engine cannot decide
    /// them.
    pub(super) fn container_conditions_verdict(
        &self,
        containers: &[std::sync::Arc<crate::css::container_conditions::ContainerConditionsData>],
        subject: StyleNodeID,
        subject_is_pseudo_element: bool,
    ) -> Option<ContainerVerdict> {
        self.container_conditions_verdict_with(self, containers, subject, subject_is_pseudo_element)
    }

    /// Evaluate container conditions for a subject as `container_conditions_verdict` does, over
    /// the containers `records` holds.
    pub(super) fn container_conditions_verdict_with(
        &self,
        records: &impl ContainerRecords,
        containers: &[std::sync::Arc<crate::css::container_conditions::ContainerConditionsData>],
        subject: StyleNodeID,
        subject_is_pseudo_element: bool,
    ) -> Option<ContainerVerdict> {
        let mut verdict = ContainerVerdict {
            matches: true,
            // A dependency is marked even where an inner condition then fails to match.
            depends_on_size: containers.iter().any(|conditions| conditions.contains_size_feature()),
            depends_on_style: conditions_ask_container_style(containers),
            ..Default::default()
        };
        // Every group holds when one of its conditions does, and the evaluation stops where the
        // host's does, so it reads no more of the containers than the host records.
        for conditions in containers {
            let mut group_matches = false;
            for condition in &conditions.conditions {
                let name = condition.name.as_ref().map_or(&[][..], |name| name.units());
                if self.container_condition_matches(
                    records,
                    subject,
                    subject_is_pseudo_element,
                    condition.query.as_deref(),
                    name,
                    &mut verdict,
                )? {
                    group_matches = true;
                    break;
                }
            }
            if !group_matches {
                verdict.matches = false;
                break;
            }
        }
        Some(verdict)
    }

    /// Whether one container condition holds for a subject: the nearest flat-tree ancestor that is
    /// a query container for every feature the query asks about, by the given name if any, as of
    /// the last layout commit.
    fn container_condition_matches(
        &self,
        records: &impl ContainerRecords,
        subject: StyleNodeID,
        subject_is_pseudo_element: bool,
        query: Option<&FfiQueryHandle>,
        name: &[u16],
        verdict: &mut ContainerVerdict,
    ) -> Option<bool> {
        let requirements = query.map_or(0, FfiQueryHandle::container_requirements);
        if requirements & CONTAINER_QUERY_HAS_UNKNOWN_FEATURE != 0 {
            return Some(false);
        }
        let mut container = if subject_is_pseudo_element {
            Some(subject)
        } else {
            self.projection.flat_tree_parent(subject)
        };
        // A query asking only about style, by no name, may ask any element.
        let asks_only_style = name.is_empty()
            && requirements
                & (CONTAINER_QUERY_REQUIRES_WIDTH
                    | CONTAINER_QUERY_REQUIRES_HEIGHT
                    | CONTAINER_QUERY_REQUIRES_INLINE_SIZE
                    | CONTAINER_QUERY_REQUIRES_BLOCK_SIZE
                    | CONTAINER_QUERY_REQUIRES_SCROLL_STATE)
                == 0;
        while let Some(candidate) = container {
            container = self.projection.flat_tree_parent(candidate);
            let inputs = match records.query_row(candidate) {
                Some(inputs) => inputs,
                // Every element is a style container, so the nearest one is the container, which
                // the records may not decide yet.
                None if asks_only_style => records.any_element_row(candidate)?,
                None => continue,
            };
            if !name.is_empty() && !inputs.is_named(name) {
                continue;
            }
            let inline_axis_horizontal = inputs.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB;
            let satisfies_width = if inline_axis_horizontal {
                inputs.is_size_container || inputs.is_inline_size_container
            } else {
                inputs.is_size_container
            };
            let satisfies_height = if inline_axis_horizontal {
                inputs.is_size_container
            } else {
                inputs.is_size_container || inputs.is_inline_size_container
            };
            if requirements & CONTAINER_QUERY_REQUIRES_WIDTH != 0 && !satisfies_width
                || requirements & CONTAINER_QUERY_REQUIRES_HEIGHT != 0 && !satisfies_height
                || requirements & CONTAINER_QUERY_REQUIRES_INLINE_SIZE != 0
                    && !(inputs.is_size_container || inputs.is_inline_size_container)
                || requirements & CONTAINER_QUERY_REQUIRES_BLOCK_SIZE != 0 && !inputs.is_size_container
                || requirements & CONTAINER_QUERY_REQUIRES_SCROLL_STATE != 0 && !inputs.is_scroll_state_container
            {
                continue;
            }
            let Some(query) = query else {
                return Some(true);
            };
            // A `rem` in the query resolves against the root element's font, and an `ex`, `cap`,
            // `ch` or `lh` against the container's.
            verdict.reads_root_font = true;
            if !verdict.font_metric_containers.contains(&candidate) {
                verdict.font_metric_containers.push(candidate);
            }
            let snapshot = self.layout_style_snapshot(candidate).unwrap_or_default();
            let document = &self.document_style_computation_inputs;
            let view = records.record_view(candidate, inputs)?;
            let mut length = self.record_length_resolution_context(view.payloads, view.dependency_flags)?;
            if let Some(root_font) = records.root_font() {
                (
                    length.root_font_metrics,
                    length.root_font_metrics_depend_on_viewport_metrics,
                ) = root_font.font_metrics();
            }
            // A container-relative length in the query resolves against the container's own query
            // container, the nearest above it eligible for the axis, or the viewport.
            let bases = self.container_unit_bases_with(records, candidate, false);
            bases.apply_to(&mut length);
            let mut resolved_viewport_relative_length = false;
            length.subject_inline_axis_is_horizontal = inline_axis_horizontal;
            length.resolved_viewport_relative_length = &raw mut resolved_viewport_relative_length;
            let opposite = |side: u8| (side + 2) % 4;
            let (block_start_side, mut inline_start_side) = match inputs.writing_mode {
                crate::css::css_enums::writing_mode::HORIZONTAL_TB => (SCROLL_STATE_SIDE_TOP, SCROLL_STATE_SIDE_LEFT),
                crate::css::css_enums::writing_mode::VERTICAL_RL | crate::css::css_enums::writing_mode::SIDEWAYS_RL => {
                    (SCROLL_STATE_SIDE_RIGHT, SCROLL_STATE_SIDE_TOP)
                }
                crate::css::css_enums::writing_mode::VERTICAL_LR => (SCROLL_STATE_SIDE_LEFT, SCROLL_STATE_SIDE_TOP),
                crate::css::css_enums::writing_mode::SIDEWAYS_LR => (SCROLL_STATE_SIDE_LEFT, SCROLL_STATE_SIDE_BOTTOM),
                _ => return None,
            };
            if inputs.direction == crate::css::css_enums::direction::RTL {
                inline_start_side = opposite(inline_start_side);
            }
            // A `style()` feature reads the container's custom properties, and its values compute
            // as the container's record computes them.
            let mut style_context = None;
            if requirements & CONTAINER_QUERY_REQUIRES_STYLE != 0 {
                let store = records.custom_store(candidate, inputs)?;
                let values = crate::css::computed_value_views::ComputedValuesView::of(view.payloads);
                let inherited_text = values.inherited_text();
                style_context = Some(RetainedContainerStyleContext {
                    store,
                    registry: document.custom_property_registry(),
                    color: crate::css::color_resolution::ColorResolutionInput {
                        scheme: Some(values.inherited_ui().color_scheme),
                        current_color: Some(crate::css::color_resolution::Rgba::from_packed(inherited_text.color)),
                        current_color_value: inherited_text.color_style_value.data(),
                        length: Some(&length),
                        channels: None,
                    },
                    tree_counting: self
                        .sibling_position(candidate)
                        .map(|position| (u64::from(position.count), u64::from(position.index))),
                });
            }
            let facts = ContainerFacts {
                container_available: true,
                size_available: snapshot.has_committed_box && inputs.size_queries_available,
                width: crate::css::css_pixels::CssPixels::from_raw(snapshot.content_width_raw).to_double(),
                height: crate::css::css_pixels::CssPixels::from_raw(snapshot.content_height_raw).to_double(),
                // The host applies the container's new style to its box before its descendants are
                // styled, so the writing mode is the record's, not the last commit's.
                inline_axis_horizontal,
                length_resolution_context: std::ptr::from_ref(&length).cast(),
                scroll_state_available: requirements & CONTAINER_QUERY_REQUIRES_SCROLL_STATE != 0,
                stuck: snapshot.stuck,
                snapped: snapshot.snapped,
                scrollable: snapshot.scrollable,
                scrolled: snapshot.scrolled,
                block_start_side,
                inline_start_side,
            };
            // What the host records when it evaluates the same condition: the container is asked
            // about, and so are those the query's container-relative lengths resolve against; a
            // container without a box is evaluated again after layout.
            if requirements
                & (CONTAINER_QUERY_REQUIRES_WIDTH
                    | CONTAINER_QUERY_REQUIRES_HEIGHT
                    | CONTAINER_QUERY_REQUIRES_INLINE_SIZE
                    | CONTAINER_QUERY_REQUIRES_BLOCK_SIZE
                    | CONTAINER_QUERY_REQUIRES_SCROLL_STATE)
                != 0
            {
                verdict
                    .effects
                    .push((candidate, FfiContainerEffectKind::SizeContainerUsage));
            }
            for basis in bases.0 {
                let Some(container) = basis.container else {
                    continue;
                };
                verdict
                    .effects
                    .push((container, FfiContainerEffectKind::SizeContainerUsage));
                // A container-relative length against a container without a box resolved to
                // zero, as it does for the host, which evaluates it again after layout.
                if basis.container_has_no_box {
                    verdict
                        .effects
                        .push((container, FfiContainerEffectKind::UnitsNeedEvaluationAfterLayout));
                }
            }
            if requirements & CONTAINER_QUERY_REQUIRES_STYLE != 0 {
                verdict
                    .effects
                    .push((candidate, FfiContainerEffectKind::StyleContainerUsage));
            }
            if requirements & CONTAINER_QUERY_REQUIRES_SCROLL_STATE != 0 {
                verdict
                    .effects
                    .push((candidate, FfiContainerEffectKind::ScrollStateContainerUsage));
            }
            if !snapshot.has_committed_box {
                verdict
                    .effects
                    .push((candidate, FfiContainerEffectKind::NeedsEvaluationAfterLayout));
            }
            // SAFETY: The facts' length context is live for the call.
            let result = unsafe {
                crate::css::parser::query_parser::evaluate_container_query(query, &facts, &mut |feature| {
                    let Some(context) = &style_context else {
                        return MatchResult::Unknown;
                    };
                    crate::css::custom_properties::evaluate_retained_container_style_feature(
                        context.store,
                        context.registry,
                        feature,
                        &length,
                        context.color,
                        context.tree_counting,
                    )
                })
            };
            if resolved_viewport_relative_length {
                verdict
                    .effects
                    .push((subject, FfiContainerEffectKind::SubjectViewportDependency));
            }
            return Some(result == MatchResult::True);
        }
        Some(false)
    }

    /// Keep what a row the engine answers read of its containers for the host, which records it
    /// when it installs the element's record, as it does for a row it computes itself.
    pub(super) fn note_container_effects_for_host(
        &mut self,
        node: StyleNodeID,
        verdict: &std::sync::Arc<ContainerVerdict>,
    ) {
        self.container_effects_for_host.note(node, verdict);
    }

    /// What the rows the host installs read of their containers, taken as it installs each.
    pub(crate) fn take_container_effects_for_host(
        &mut self,
        node: StyleNodeID,
    ) -> Option<std::sync::Arc<ContainerVerdict>> {
        self.container_effects_for_host.set(node, None)
    }

    /// See [`Self::detach_host_flags_for_fork`].
    pub(super) fn detach_container_effects_held_for_fork(&mut self) {
        let held = self
            .container_effects_for_host
            .held
            .load(std::sync::atomic::Ordering::Relaxed);
        self.container_effects_for_host.held = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(held));
    }

    /// Raises `flag`, which the document's host reads, while the engine keeps any row's container effects for the
    /// host, rather than a flag of the engine's own. The engine keeps none yet.
    pub(crate) fn share_container_effects_held(&mut self, flag: std::sync::Arc<std::sync::atomic::AtomicBool>) {
        debug_assert!(self.container_effects_for_host.effects.is_empty());
        self.container_effects_for_host.held = flag;
    }
}

/// What the container conditions of the rows the engine answered read of their containers, per element, kept for the
/// host until it takes each as it installs the element's record, and a flag the host reads, without asking, for whether
/// any is kept.
#[derive(Clone, Default)]
pub(super) struct ContainerEffectsForHost {
    effects: HashMap<StyleNodeID, std::sync::Arc<ContainerVerdict>>,
    held: std::sync::Arc<std::sync::atomic::AtomicBool>,
}

impl ContainerEffectsForHost {
    pub(super) fn get(&self, node: &StyleNodeID) -> Option<&ContainerVerdict> {
        self.effects.get(node).map(|verdict| &**verdict)
    }

    fn note(&mut self, node: StyleNodeID, verdict: &std::sync::Arc<ContainerVerdict>) {
        match self.effects.entry(node) {
            std::collections::hash_map::Entry::Occupied(mut held) => {
                std::sync::Arc::make_mut(held.get_mut()).add_reads((**verdict).clone());
            }
            std::collections::hash_map::Entry::Vacant(entry) => {
                entry.insert(verdict.clone());
            }
        }
        self.held.store(true, std::sync::atomic::Ordering::Relaxed);
    }

    /// Keeps `verdict` for `node`, or none, and answers what it kept before.
    pub(super) fn set(
        &mut self,
        node: StyleNodeID,
        verdict: Option<std::sync::Arc<ContainerVerdict>>,
    ) -> Option<std::sync::Arc<ContainerVerdict>> {
        let previous = match verdict {
            Some(verdict) => self.effects.insert(node, verdict),
            None => self.effects.remove(&node),
        };
        self.held
            .store(!self.effects.is_empty(), std::sync::atomic::Ordering::Relaxed);
        previous
    }
}
