/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Whether a text node that holds nothing but white space gets a box.
//!
//! Collapsible white space renders nothing where a line starts, after other collapsible white space, and among the
//! items of a container that lays out each of its children as its own box. Such a text node gets no box, as text in a
//! `display: none` subtree gets none, which also spares the anonymous block that would wrap it among block-level
//! siblings.
//! https://drafts.csswg.org/css-display-3/#anonymous
//! https://drafts.csswg.org/css-flexbox-1/#flex-items
//! https://drafts.csswg.org/css-grid-2/#grid-items
//!
//! Whether the text renders depends on the nearest box before it among the boxes of its layout parent, which a walk
//! back over the flat tree finds, looking through `display: contents` elements and slots.

use super::{
    LayoutNode, TreeBuilderHost, display_for_table_fixup, is_table_non_root_box_with_display, is_tabular_container,
    lays_out_each_child_as_its_own_box, node_is_inline_outside, node_is_out_of_flow_child_of,
    node_kind_is_node_with_style, text_produces_line_box_fragment_when_empty,
};
use crate::css::css_enums::{float, positioning};
use crate::css::style::RetainedState;
use crate::css::style::bridge::{ElementBoxKind, element_adjustment_fact};
use crate::css::style::tree::StyleNodeID;
use crate::layout::node_data::{GENERATED_FOR_AFTER, GENERATED_FOR_BEFORE, NodeKind};
use crate::layout::node_facts;

/// The nearest box before a white space text among the boxes its layout parent lays out, as far as the text cares.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PrecedingBox {
    /// No box comes before the text.
    Nothing,
    /// The box of a text, into whose trailing collapsible white space the text's own white space collapses.
    Text { ends_in_collapsible_white_space: bool },
    /// An inline-level box other than a line break.
    InlineLevel,
    /// A table-internal box outside a table, which table fixup later wraps in an anonymous table. Inside inline content
    /// that table is inline-level, and white space after it renders.
    TableInternal,
    /// A forced line break or a block-level box, after which a line starts.
    LineStart,
}

/// What a node before the text is to the walk back.
enum Step {
    /// The node has no box among the ones before the text, or one out of flow.
    PassOver,
    /// A `display: contents` element, whose children's boxes are its layout parent's.
    Enter,
    Box(PrecedingBox),
    /// The box the node will have is not known yet.
    Unknown,
}

/// Answers what each node before the text contributes to the boxes of its layout parent.
trait PrecedingBoxes {
    fn engine(&self) -> &RetainedState;
    fn step(&self, node: StyleNodeID) -> Step;
    fn pseudo_element_step(&self, element: StyleNodeID, generated_for: u8) -> Step;
}

/// The boxes the walk built already: every box before the text is final once the text's turn comes, as walks go in
/// tree order.
struct BuiltBoxes<'a, 'host> {
    host: &'a TreeBuilderHost<'host>,
    engine: &'a RetainedState,
    layout_parent: LayoutNode,
}

impl BuiltBoxes<'_, '_> {
    fn box_step(&self, row: LayoutNode) -> Step {
        let host = self.host;
        let kind = host.data(row).kind.get();
        if node_kind_is_node_with_style(kind) && node_is_out_of_flow_child_of(host, self.layout_parent, row) {
            return Step::PassOver;
        }
        if kind == NodeKind::BreakNode {
            return Step::Box(PrecedingBox::LineStart);
        }
        if is_table_non_root_box_with_display(display_for_table_fixup(host, row)) {
            return Step::Box(PrecedingBox::TableInternal);
        }
        if node_is_inline_outside(host, row) {
            return Step::Box(PrecedingBox::InlineLevel);
        }
        Step::Box(PrecedingBox::LineStart)
    }
}

impl PrecedingBoxes for BuiltBoxes<'_, '_> {
    fn engine(&self) -> &RetainedState {
        self.engine
    }

    fn step(&self, node: StyleNodeID) -> Step {
        let row = self.host.arena().bound_row(node);
        if node.text_index().is_some() {
            if let Some(step) = collapsing_whitespace_text_step(self.engine, node) {
                return step;
            }
            if row.is_invalid() {
                return Step::PassOver;
            }
            return Step::Box(text_box(self.engine, node));
        }
        if self.engine.element_adjustment_facts(node) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0 {
            return Step::PassOver;
        }
        if row.is_invalid() {
            return if is_display_contents(self.engine, node) {
                Step::Enter
            } else {
                Step::PassOver
            };
        }
        self.box_step(row)
    }

    fn pseudo_element_step(&self, element: StyleNodeID, generated_for: u8) -> Step {
        let row = self.host.arena().bound_pseudo_element_row(element, generated_for);
        if row.is_invalid() {
            return Step::PassOver;
        }
        self.box_step(row)
    }
}

/// The boxes a child list insertion will leave, before the walk builds them: an inserted or restyled node answers from
/// what it publishes, and only where the box it gets is certain.
struct PredictedBoxes<'a, 'host> {
    built: BuiltBoxes<'a, 'host>,
}

impl PrecedingBoxes for PredictedBoxes<'_, '_> {
    fn engine(&self) -> &RetainedState {
        self.built.engine
    }

    fn step(&self, node: StyleNodeID) -> Step {
        let engine = self.built.engine;
        if !self.built.host.arena().needs_layout_tree_update(node) {
            return self.built.step(node);
        }
        if node.text_index().is_some() {
            if let Some(step) = collapsing_whitespace_text_step(engine, node) {
                return step;
            }
            // Of the text that is white space alone, only empty text may or may not get a box once it is inserted.
            if engine.text_is_ascii_whitespace(node) && engine.text_collapses_white_space(node) == Some(true) {
                return Step::Unknown;
            }
            return Step::Box(text_box(engine, node));
        }
        let adjustment_facts = engine.element_adjustment_facts(node);
        if adjustment_facts & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0 {
            return Step::PassOver;
        }
        let Some(facts) = engine.element_published_box_facts(node) else {
            return Step::Unknown;
        };
        if facts.display.is_none() {
            return Step::PassOver;
        }
        if facts.display.is_contents()
            || adjustment_facts
                & (element_adjustment_fact::IS_SVG_ELEMENT | element_adjustment_fact::REQUIRES_SVG_CONTAINER)
                != 0
        {
            return Step::Unknown;
        }
        if facts.position == positioning::ABSOLUTE
            || facts.position == positioning::FIXED
            || facts.float_ != float::NONE
        {
            return Step::PassOver;
        }
        match engine.element_box_kind(node) {
            ElementBoxKind::Break => Step::Box(PrecedingBox::LineStart),
            ElementBoxKind::FromDisplay if is_table_non_root_box_with_display(facts.display) => {
                Step::Box(PrecedingBox::TableInternal)
            }
            ElementBoxKind::FromDisplay if facts.display.is_inline_outside() => Step::Box(PrecedingBox::InlineLevel),
            ElementBoxKind::FromDisplay => Step::Box(PrecedingBox::LineStart),
            _ => Step::Unknown,
        }
    }

    fn pseudo_element_step(&self, element: StyleNodeID, generated_for: u8) -> Step {
        self.built.pseudo_element_step(element, generated_for)
    }
}

fn is_display_contents(engine: &RetainedState, element: StyleNodeID) -> bool {
    engine
        .element_published_box_facts(element)
        .is_some_and(|facts| facts.display.is_contents())
}

/// White space that collapses, before a white space text, means the later text renders nothing whether the earlier one
/// has a box or not: with a box, the later white space collapses into it, and without one, the earlier text renders
/// nothing after the same box that comes before them both. So the walk back stops at it without asking about its box,
/// which also keeps a walk over a long run of white space and hidden elements short.
fn collapsing_whitespace_text_step(engine: &RetainedState, text: StyleNodeID) -> Option<Step> {
    let is_collapsing_whitespace = engine.text_is_ascii_whitespace(text)
        && engine.text_data(text).is_some_and(|data| !data.is_empty())
        && engine.text_collapses_white_space(text) == Some(true);
    is_collapsing_whitespace.then_some(Step::Box(PrecedingBox::Text {
        ends_in_collapsible_white_space: true,
    }))
}

fn text_box(engine: &RetainedState, text: StyleNodeID) -> PrecedingBox {
    let last_code_unit = engine.text_data(text).and_then(|data| match data.as_units() {
        ak::Utf16StringUnits::Ascii(units) | ak::Utf16StringUnits::Latin1(units) => {
            units.last().map(|&unit| unit.into())
        }
        ak::Utf16StringUnits::Utf16(units) => units.last().copied(),
    });
    let ends_in_white_space = last_code_unit.is_some_and(|unit: u16| matches!(unit, 0x09..=0x0d | 0x20));
    PrecedingBox::Text {
        ends_in_collapsible_white_space: ends_in_white_space && engine.text_collapses_white_space(text) == Some(true),
    }
}

/// The node before `node` among the children of its parent in the flat tree: the slot it is assigned to, or its
/// parent.
fn previous_flat_tree_sibling(engine: &RetainedState, node: StyleNodeID) -> Option<StyleNodeID> {
    let tree = engine;
    if let Some(slot) = tree.assigned_slot_of(node) {
        let assigned = tree.assigned_nodes_of(slot);
        let index = assigned.iter().position(|&assigned_node| assigned_node == node)?;
        return index.checked_sub(1).map(|previous| assigned[previous]);
    }
    tree.previous_sibling_in_dom_order(node)
}

/// The children of `element` in the flat tree, text included.
fn flat_tree_children(engine: &RetainedState, element: StyleNodeID) -> Vec<StyleNodeID> {
    let tree = engine;
    if let Some(shadow_root) = tree.shadow_root_of(element) {
        return tree.dom_children(shadow_root).collect();
    }
    if engine.element_is_slot(element) {
        let assigned = tree.assigned_nodes_of(element);
        if !assigned.is_empty() {
            return assigned.to_vec();
        }
    }
    tree.dom_children(element).collect()
}

/// The last box `node` contributes to the boxes of its layout parent, if any, or `None` when that is not known yet.
fn last_box_of(boxes: &impl PrecedingBoxes, node: StyleNodeID) -> Option<Option<PrecedingBox>> {
    match boxes.step(node) {
        Step::PassOver => Some(None),
        Step::Box(found) => Some(Some(found)),
        Step::Unknown => None,
        Step::Enter => {
            if let Some(found) = pseudo_element_box(boxes, node, GENERATED_FOR_AFTER)? {
                return Some(Some(found));
            }
            for child in flat_tree_children(boxes.engine(), node).into_iter().rev() {
                if let Some(found) = last_box_of(boxes, child)? {
                    return Some(Some(found));
                }
            }
            pseudo_element_box(boxes, node, GENERATED_FOR_BEFORE)
        }
    }
}

fn pseudo_element_box(
    boxes: &impl PrecedingBoxes,
    element: StyleNodeID,
    generated_for: u8,
) -> Option<Option<PrecedingBox>> {
    match boxes.pseudo_element_step(element, generated_for) {
        Step::Box(found) => Some(Some(found)),
        Step::Unknown => None,
        Step::PassOver | Step::Enter => Some(None),
    }
}

/// The nearest box before `text` among the boxes of its layout parent, or `None` when that is not known yet.
fn preceding_box(boxes: &impl PrecedingBoxes, text: StyleNodeID) -> Option<PrecedingBox> {
    let engine = boxes.engine();
    let mut node = text;
    loop {
        let mut sibling = previous_flat_tree_sibling(engine, node);
        while let Some(current) = sibling {
            if let Some(found) = last_box_of(boxes, current)? {
                return Some(found);
            }
            sibling = previous_flat_tree_sibling(engine, current);
        }
        // The slot the node is assigned to, its parent, or the host of the shadow root it is a child of.
        let Some(parent) = engine.inheritance_parent(node) else {
            return Some(PrecedingBox::Nothing);
        };
        if let Some(found) = pseudo_element_box(boxes, parent, GENERATED_FOR_BEFORE)? {
            return Some(found);
        }
        // The children of a `display: contents` element continue the boxes before it.
        if !is_display_contents(engine, parent) {
            return Some(PrecedingBox::Nothing);
        }
        node = parent;
    }
}

/// Whether the text keeps its box whatever comes before it, or `None` when that decides it.
fn decision_of_layout_parent_or_text(
    host: &TreeBuilderHost<'_>,
    engine: &RetainedState,
    layout_parent: LayoutNode,
    text: StyleNodeID,
) -> Option<bool> {
    let kind = host.data(layout_parent).kind.get();
    if kind == NodeKind::SVGSVGBox || node_facts::kind_is_svg_box(kind) {
        // SVG renders text only inside a text content element.
        let mut ancestor = layout_parent;
        while !ancestor.is_invalid() {
            match host.data(ancestor).kind.get() {
                NodeKind::SVGTextBox | NodeKind::SVGTextPathBox => return Some(true),
                NodeKind::SVGSVGBox => return Some(false),
                _ => ancestor = host.parent(ancestor),
            }
        }
        return Some(false);
    }
    if engine.text_collapses_white_space(text) != Some(true)
        || text_produces_line_box_fragment_when_empty(engine, text, engine.text_is_in_user_agent_shadow_tree(text))
    {
        return Some(true);
    }
    None
}

/// Whether white space renders after `preceding` among the boxes of `layout_parent`.
fn renders_after(host: &TreeBuilderHost<'_>, layout_parent: LayoutNode, preceding: PrecedingBox) -> bool {
    if lays_out_each_child_as_its_own_box(host, layout_parent) {
        // Only white space that continues a run of text joins an item; a run of nothing but white space is none.
        return preceding
            == PrecedingBox::Text {
                ends_in_collapsible_white_space: false,
            };
    }
    match preceding {
        PrecedingBox::Nothing => {
            // An inline box keeps the white space that starts it, and an inside marker comes before it once it is
            // built, after the children.
            host.data(layout_parent).kind.get() == NodeKind::InlineNode
                || (host.data(layout_parent).kind.get() == NodeKind::ListItemBox
                    && host
                        .style(layout_parent)
                        .is_some_and(|style| style.list_style_position_is_inside()))
        }
        PrecedingBox::Text {
            ends_in_collapsible_white_space,
        } => !ends_in_collapsible_white_space,
        PrecedingBox::InlineLevel => true,
        PrecedingBox::TableInternal => !is_tabular_container(host, layout_parent),
        PrecedingBox::LineStart => false,
    }
}

/// Whether the text node `text`, which holds nothing but white space, gets a box among the children of
/// `layout_parent`. The walk that asks has built the boxes of every node before the text.
pub(super) fn whitespace_text_needs_box(
    host: &TreeBuilderHost<'_>,
    layout_parent: LayoutNode,
    text: StyleNodeID,
) -> bool {
    host.arena().with_style_store(|engine| {
        if let Some(decision) = decision_of_layout_parent_or_text(host, engine, layout_parent, text) {
            return decision;
        }
        let boxes = BuiltBoxes {
            host,
            engine,
            layout_parent,
        };
        let preceding = preceding_box(&boxes, text).expect("every box before the text is built");
        renders_after(host, layout_parent, preceding)
    })
}

/// Whether the text node `text`, which holds nothing but white space, will get a box among the children of
/// `layout_parent` once a child list insertion has built the nodes marked for it, or `None` where a marked node's box
/// is not certain before it is built.
pub(super) fn predicted_whitespace_text_needs_box(
    host: &TreeBuilderHost<'_>,
    engine: &RetainedState,
    layout_parent: LayoutNode,
    text: StyleNodeID,
) -> Option<bool> {
    if let Some(decision) = decision_of_layout_parent_or_text(host, engine, layout_parent, text) {
        return Some(decision);
    }
    let boxes = PredictedBoxes {
        built: BuiltBoxes {
            host,
            engine,
            layout_parent,
        },
    };
    Some(renders_after(host, layout_parent, preceding_box(&boxes, text)?))
}

/// Whether a white space text may come first among the boxes the children of `element` lay out, where a change to the
/// element's `::before` makes it gain or lose its box. A child whose box is not known yet may start with such text.
pub(super) fn starts_with_whitespace_text(engine: &RetainedState, element: StyleNodeID) -> bool {
    first_content_is_whitespace_text(engine, element) == Some(true)
}

/// Whether the first content among the boxes the children of `element` lay out is white space text, or `None` when they
/// lay out none. The children of a `display: contents` child count as its parent's.
fn first_content_is_whitespace_text(engine: &RetainedState, element: StyleNodeID) -> Option<bool> {
    for child in flat_tree_children(engine, element) {
        if child.text_index().is_some() {
            return Some(engine.text_is_ascii_whitespace(child));
        }
        let Some(facts) = engine.element_published_box_facts(child) else {
            return Some(true);
        };
        if facts.display.is_contents() {
            if let Some(found) = first_content_is_whitespace_text(engine, child) {
                return Some(found);
            }
            continue;
        }
        let lays_out_elsewhere = facts.display.is_none()
            || facts.position == positioning::ABSOLUTE
            || facts.position == positioning::FIXED
            || facts.float_ != float::NONE
            || engine.element_adjustment_facts(child) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0;
        if !lays_out_elsewhere {
            return Some(false);
        }
    }
    None
}
