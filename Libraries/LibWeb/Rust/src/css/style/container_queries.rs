/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Container queries, evaluated over the retained container facts: the containers' published
//! container query inputs, the previous layout's boxes and the containers' style records.

use std::ffi::c_void;

use super::bridge::FfiContainerEffectKind;
use super::tree::StyleNodeID;
use super::*;
use crate::css::computed_longhand_table::FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS;
use crate::css::custom_properties::StyleQueryDependencies;
use crate::css::parser::query_parser::{
    CONTAINER_QUERY_HAS_UNKNOWN_FEATURE, CONTAINER_QUERY_REQUIRES_BLOCK_SIZE, CONTAINER_QUERY_REQUIRES_HEIGHT,
    CONTAINER_QUERY_REQUIRES_INLINE_SIZE, CONTAINER_QUERY_REQUIRES_SCROLL_STATE, CONTAINER_QUERY_REQUIRES_STYLE,
    CONTAINER_QUERY_REQUIRES_WIDTH, FfiContainerFacts, FfiContainerStyleFeature, FfiQueryHandle, MatchResult,
    SCROLL_STATE_SIDE_BOTTOM, SCROLL_STATE_SIDE_LEFT, SCROLL_STATE_SIDE_RIGHT, SCROLL_STATE_SIDE_TOP,
};

/// What a rule's container conditions say for one subject: whether they hold, whether they ask
/// about a container's size, scroll state or style, and what the evaluation read of the
/// containers, for the host to record.
#[derive(Clone, Default)]
pub(crate) struct ContainerVerdict {
    pub(crate) matches: bool,
    pub(crate) depends_on_size: bool,
    pub(crate) depends_on_style: bool,
    pub(crate) effects: Vec<(StyleNodeID, FfiContainerEffectKind)>,
    /// The custom properties the conditions' `style()` features read of the containers, which
    /// the host records as the subject's style query references.
    pub(crate) style_query_references: Option<Box<StyleQueryDependencies>>,
}

impl ContainerVerdict {
    /// Add what another evaluation read of the containers to what this one read.
    pub(crate) fn add_reads(&mut self, other: ContainerVerdict) {
        self.depends_on_size |= other.depends_on_size;
        self.depends_on_style |= other.depends_on_style;
        self.effects.extend(other.effects);
        if let Some(references) = other.style_query_references {
            match &mut self.style_query_references {
                Some(noted) => noted.extend(&references),
                noted => *noted = Some(references),
            }
        }
    }
}

/// What a `style()` feature reads of the container it asks about: the custom-property
/// environment of its record, and lengths and colors as the record computes them.
struct RetainedContainerStyleContext<'a> {
    store: Option<&'a crate::css::custom_properties::CustomPropertyStore>,
    registry: &'a crate::css::custom_properties::CustomPropertyRegistry,
    color: crate::css::color_resolution::ColorResolutionInput<'a>,
}

/// The engine answers `style()` features in Rust, never through the facts' callback.
unsafe extern "C" fn no_style_feature_callback(_: *mut c_void, _: FfiContainerStyleFeature) -> u8 {
    MatchResult::Unknown as u8
}

/// Whether one of the container conditions asks a `style()` question.
fn conditions_ask_container_style(
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
        view: &computed::StyleRecordView<'_>,
    ) -> Option<crate::css::style_compute::FfiLengthResolutionContext> {
        let document = &self.document_style_computation_inputs;
        let font = unsafe {
            view.payloads[crate::css::computed_value_types::STYLE_GROUP_INDEX_FONT]
                .cast::<crate::css::computed_value_types::FontValues>()
                .as_ref()
        }?;
        let values = crate::css::computed_value_views::ComputedValuesView::new(
            crate::css::host_shared::SharedPayload::as_pointer_slice(view.payloads),
        );
        Some(crate::css::style_compute::FfiLengthResolutionContext {
            viewport_width: document.viewport_width,
            viewport_height: document.viewport_height,
            font_metrics: crate::css::style_compute::FfiFontMetrics {
                font_size: font.font_size.to_double(),
                x_height: super::publication::drive_font_metric(font.font_x_height),
                // The C++ metrics approximate the cap height with the ascent.
                cap_height: super::publication::drive_font_metric(font.font_ascent),
                zero_advance: super::publication::drive_font_metric(font.font_zero_advance),
                line_height: font.line_height_used.to_double(),
            },
            root_font_metrics: crate::css::style_compute::FfiFontMetrics {
                font_size: document.root_font_size,
                x_height: document.root_font_x_height,
                cap_height: document.root_font_cap_height,
                zero_advance: document.root_font_zero_advance,
                line_height: document.root_line_height,
            },
            font_metrics_depend_on_viewport_metrics: view.dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS
                != 0,
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
        })
    }

    /// Evaluate a rule's container conditions for a subject, as the host evaluates them. `None`
    /// when the engine cannot decide them: the rule has no target, or a container it asks about
    /// holds no record or custom-property environment to read.
    pub(crate) fn rule_container_verdict(
        &self,
        rule: RuleID,
        subject: StyleNodeID,
        subject_is_pseudo_element: bool,
    ) -> Option<ContainerVerdict> {
        let containers = self.native_rules.targets.get(&rule)?.containers();
        self.container_conditions_verdict(containers, subject, subject_is_pseudo_element)
    }

    /// Evaluate container conditions for a subject, as the host evaluates them: a rule's, or those
    /// around a block of a custom function's declarations. `None` when the engine cannot decide
    /// them, as for `rule_container_verdict`.
    pub(super) fn container_conditions_verdict(
        &self,
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
            self.tree.flat_tree_parent(subject)
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
        let mut any_element_row;
        while let Some(candidate) = container {
            container = self.tree.flat_tree_parent(candidate);
            let inputs = match self.container_query_inputs(candidate) {
                Some(inputs) => inputs,
                // Every element is a style container, so the nearest one is the container; one
                // holding no record yet is one the host styles in this update, which decides it.
                None if asks_only_style => {
                    let style_record = self.held_style_records.get(&candidate).copied().or_else(|| {
                        self.computed_group_sets
                            .assigned_style_record(candidate)
                            .map(|record| record.raw())
                    })?;
                    any_element_row = self.container_query_input_row(style_record, true);
                    any_element_row.as_ref()?
                }
                None => continue,
            };
            if !name.is_empty() && !inputs.names.iter().any(|candidate| candidate == name) {
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
            let snapshot = self.layout_style_snapshot(candidate).unwrap_or_default();
            let document = &self.document_style_computation_inputs;
            let view = self.computed_group_sets.style_record_view(inputs.style_record)?;
            let mut length = self.record_length_resolution_context(&view)?;
            // A container-relative length in the query resolves against the container's own query
            // container, the nearest above it eligible for the axis, or the viewport.
            let container_unit_basis = |horizontal: bool| {
                let mut ancestor = self.tree.flat_tree_parent(candidate);
                while let Some(node) = ancestor {
                    ancestor = self.tree.flat_tree_parent(node);
                    let Some(ancestor_inputs) = self.container_query_inputs(node) else {
                        continue;
                    };
                    let ancestor_inline_axis_horizontal =
                        ancestor_inputs.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB;
                    let eligible = if horizontal == ancestor_inline_axis_horizontal {
                        ancestor_inputs.is_size_container || ancestor_inputs.is_inline_size_container
                    } else {
                        ancestor_inputs.is_size_container
                    };
                    if !eligible {
                        continue;
                    }
                    let snapshot = self.layout_style_snapshot(node).unwrap_or_default();
                    if !snapshot.has_committed_box {
                        return (0.0, false, Some(node));
                    }
                    let raw = if horizontal {
                        snapshot.content_width_raw
                    } else {
                        snapshot.content_height_raw
                    };
                    return (
                        crate::css::css_pixels::CssPixels::from_raw(raw).to_double(),
                        false,
                        Some(node),
                    );
                }
                let viewport = if horizontal {
                    document.viewport_width
                } else {
                    document.viewport_height
                };
                (viewport, true, None)
            };
            let (container_width_basis, width_basis_depends_on_viewport, width_basis_node) = container_unit_basis(true);
            let (container_height_basis, height_basis_depends_on_viewport, height_basis_node) =
                container_unit_basis(false);
            let mut resolved_viewport_relative_length = false;
            length.has_container_width_basis = true;
            length.has_container_height_basis = true;
            length.container_width_basis = container_width_basis;
            length.container_height_basis = container_height_basis;
            length.container_width_basis_depends_on_viewport_metrics = width_basis_depends_on_viewport;
            length.container_height_basis_depends_on_viewport_metrics = height_basis_depends_on_viewport;
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
                let store = match self
                    .computed_group_sets
                    .style_record_custom_property_environment(inputs.style_record)
                    .unwrap_or_default()
                {
                    0 => None,
                    // SAFETY: The store is live while the container's record names its environment.
                    identity => Some(unsafe { &*self.custom_property_environments.store(identity)?.cast() }),
                };
                let values = crate::css::computed_value_views::ComputedValuesView::new(
                    crate::css::host_shared::SharedPayload::as_pointer_slice(view.payloads),
                );
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
                });
            }
            let facts = FfiContainerFacts {
                container_available: true,
                size_available: snapshot.has_committed_box,
                width: crate::css::css_pixels::CssPixels::from_raw(snapshot.content_width_raw).to_double(),
                height: crate::css::css_pixels::CssPixels::from_raw(snapshot.content_height_raw).to_double(),
                // The host applies the container's new style to its box before its descendants are
                // styled, so the writing mode is the record's, not the last commit's.
                inline_axis_horizontal,
                length_resolution_context: std::ptr::from_ref(&length).cast(),
                style_context: std::ptr::null_mut(),
                evaluate_style_feature: no_style_feature_callback,
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
            for basis in [width_basis_node, height_basis_node].into_iter().flatten() {
                verdict
                    .effects
                    .push((basis, FfiContainerEffectKind::SizeContainerUsage));
                // A container-relative length against a container without a box resolved to
                // zero, as it does for the host, which evaluates it again after layout.
                if !self.layout_style_snapshot(basis).unwrap_or_default().has_committed_box {
                    verdict
                        .effects
                        .push((basis, FfiContainerEffectKind::NeedsEvaluationAfterLayout));
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
            let references = &mut verdict.style_query_references;
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
                        references,
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
    pub(super) fn note_container_effects_for_host(&mut self, node: StyleNodeID, verdict: ContainerVerdict) {
        self.container_effects_for_host
            .entry(node)
            .or_default()
            .add_reads(verdict);
    }

    /// What the rows the host installs read of their containers, taken as it installs each.
    pub(crate) fn take_container_effects_for_host(&mut self, node: StyleNodeID) -> Option<ContainerVerdict> {
        self.container_effects_for_host.remove(&node)
    }

    /// Whether the winners published for a node hold a rule's container conditions: an element's
    /// winners hold a gated rule where its conditions held when they were published, and the record
    /// loop checks that they still do. A pseudo-element's are not checked there, so its gated rules
    /// stay the host's.
    pub(crate) fn container_gate_is_held(&self, node: Option<StyleNodeID>, rule: RuleID, pseudo: bool) -> bool {
        !self.program.rule_is_gated_by_container_query(rule)
            || (!pseudo && node.is_some_and(|node| !self.container_gates_unheld.contains(&node)))
    }

    /// Whether one of a rule's container conditions asks a `style()` question, which every
    /// element above the subject may answer.
    fn rule_asks_container_style(&self, rule: RuleID) -> bool {
        self.native_rules
            .targets
            .get(&rule)
            .is_some_and(|target| conditions_ask_container_style(target.containers()))
    }

    /// Decide, as a node's winners are published, whether its gated rules can be: their
    /// conditions read the containers above it, which are final only when no ancestor's answer
    /// moves in the same transaction.
    pub(super) fn note_container_gates_for_publication(&mut self, node: StyleNodeID, effects: &AnswerEffects) {
        if self.container_ancestor_answer_moves(node, effects) {
            self.container_gates_unheld.insert(node);
        } else {
            self.container_gates_unheld.remove(&node);
        }
    }

    /// Whether an ancestor's answer moves in the transaction `effects` belong to: the containers
    /// above `node` are not final then, and neither are its gated rules' conditions.
    pub(super) fn container_ancestor_answer_moves(&self, node: StyleNodeID, effects: &AnswerEffects) -> bool {
        let mut ancestor = self.tree.flat_tree_parent(node);
        while let Some(current) = ancestor {
            if effects.lookup(current).is_some() {
                return true;
            }
            ancestor = self.tree.flat_tree_parent(current);
        }
        false
    }

    /// A node whose gated rules decide differently over the containers as they stand now than
    /// when its winners were published holds winners no record may be derived from: they are
    /// dropped, and the node is the host's until they are published again. One the engine could
    /// not decide when they were published, and still cannot, has not moved: the record loop
    /// leaves that node to the host without its winners being published again every flush.
    pub(super) fn drop_winners_whose_container_verdicts_moved(&mut self) {
        if self.published_container_verdicts.is_empty() {
            return;
        }
        let moved: Vec<StyleNodeID> = self
            .published_container_verdicts
            .iter()
            .filter(|(node, verdicts)| {
                verdicts.iter().any(|&(rule, held)| {
                    let verdict = self.rule_container_verdict(rule, **node, false);
                    verdict.map(|verdict| verdict.matches) != held
                })
            })
            .map(|(&node, _)| node)
            .collect();
        for node in moved {
            self.published_container_verdicts.remove(&node);
            self.winner_groups.remove(node);
        }
    }

    /// Whether a rule decides for the node as far as its container conditions go: an ungated rule
    /// always does, a gated one where they held when the node's winners were published.
    pub(crate) fn published_container_verdict_holds(&self, node: StyleNodeID, rule: RuleID) -> bool {
        !self.program.rule_is_gated_by_container_query(rule)
            || self
                .published_container_verdicts
                .get(&node)
                .is_some_and(|verdicts| verdicts.contains(&(rule, Some(true))))
    }

    pub(super) fn publish_container_verdicts(&mut self, node: StyleNodeID, verdicts: Vec<(RuleID, Option<bool>)>) {
        if verdicts.is_empty() {
            self.published_container_verdicts.remove(&node);
        } else {
            self.published_container_verdicts.insert(node, verdicts);
        }
    }

    /// Whether every gated rule of a node's winners decides now as it did when they were published,
    /// over its containers as its settled ancestors left them. What the evaluations read of the
    /// containers is kept for the host, which records it with the node's record. An undecided one
    /// never stands: the winners hold the rule nowhere, which may not be where it holds.
    pub(super) fn container_verdicts_stand(&mut self, node: StyleNodeID) -> bool {
        let Some(published) = self.published_container_verdicts.get(&node) else {
            return true;
        };
        let mut verdicts = Vec::with_capacity(published.len());
        for &(rule, held) in published {
            match self.rule_container_verdict(rule, node, false) {
                Some(verdict) if held == Some(verdict.matches) => verdicts.push(verdict),
                _ => return false,
            }
        }
        for verdict in verdicts {
            self.note_container_effects_for_host(node, verdict);
        }
        true
    }

    /// Whether a flat-tree ancestor of the node the conditions can ask about was declined in this
    /// batch: the record the host computes for it is installed after the batch, and so are the
    /// container inputs it publishes. A size or scroll-state query asks only about an ancestor that
    /// is such a container, or whose winners may make it one; a style query about any.
    pub(super) fn container_ancestor_is_unsettled(
        &self,
        node: StyleNodeID,
        scratch: &super::publication::EngineComputedRecordScratch,
    ) -> bool {
        let asks_about_style = self
            .published_container_verdicts
            .get(&node)
            .is_some_and(|verdicts| verdicts.iter().any(|&(rule, _)| self.rule_asks_container_style(rule)));
        let mut ancestor = self.tree.flat_tree_parent(node);
        while let Some(current) = ancestor {
            if let Some(index) = current.element_index()
                && scratch
                    .derived_child_inputs
                    .get(index as usize)
                    .is_some_and(|row| row.declined)
                && (asks_about_style || self.may_be_a_query_container(current))
            {
                return true;
            }
            ancestor = self.tree.flat_tree_parent(current);
        }
        false
    }

    /// Whether an element is a size, scroll-state or named container, or its winners may make it
    /// one: an element that is none now and declares neither `container-type` nor `container-name`
    /// is none a query asks about. A declared name can move with what it substitutes.
    fn may_be_a_query_container(&self, node: StyleNodeID) -> bool {
        use crate::css::property_metadata::property_id::{CONTAINER_NAME, CONTAINER_TYPE};
        if self.container_query_inputs(node).is_some_and(|inputs| {
            inputs.is_size_container
                || inputs.is_inline_size_container
                || inputs.is_scroll_state_container
                || !inputs.names.is_empty()
        }) {
            return true;
        }
        match self
            .current_winner_groups()
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        {
            Lookup::Known((_, state)) => {
                self.winner_groups.winner_in_state(state, CONTAINER_TYPE).is_some()
                    || self.winner_groups.winner_in_state(state, CONTAINER_NAME).is_some()
            }
            _ => true,
        }
    }
}
