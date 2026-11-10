/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A clock lane's drag selection: while the host runs a task, the highlight of a drag that selects text follows the
//! pointer in the frames the lane presents, so a busy page keeps up with the mouse.
//!
//! The press stays the host's, which alone knows whether the page lets the drag start, and a rendering update after it
//! seals where the selection is anchored with the plan. A mouse move has no default action, so nothing the page does
//! keeps the drag from going on, and a tick can show where it goes before the host gets to the move.
//!
//! The lane paints and decides nothing else. What a tick shows goes with the lane's fork, the selection stays the
//! host's, and the host makes its own selection as it handles the same moves, in the frame that takes over from the
//! lane's. What a tick shows includes the `::selection` styles of the elements it reaches, which it styles in the fork.
//! A move the host would treat in a way only it can, such as one that `user-select: all` widens, is left to it,
//! and the highlight waits where it was. A word or paragraph selection, and a selection in a text control or an editing
//! host, are the host's altogether.

use super::{Lane, RenderState, hover};
use crate::css::css_pixels::{CssPixelPoint, CssPixels};
use crate::css::style::StyleEngine;
use crate::css::style::drag_selection::{SelectionStyles, UsedUserSelect};
use crate::css::style::tree::StyleNodeID;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::svg_formatting_context::FfiFloatRect;
use crate::painting::hit_test::HitTestList;
use crate::painting::hit_test::resolve::row_dom_style_node;
use crate::painting::host::{FfiCaretBoundaryKind, FfiHitTestQueryCallbacks, FfiResolvedCaret};
use crate::painting::paint_read::GeometryRead;
use crate::painting::paintable_data::{
    FfiSelectionEntry, SELECTION_STATE_END, SELECTION_STATE_FULL, SELECTION_STATE_NONE, SELECTION_STATE_START,
    SELECTION_STATE_START_AND_END,
};
use crate::painting::selection::sync_selection_style;
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

/// Where a drag selection the host started is anchored, sealed with the plan.
#[derive(Clone, Copy, Debug)]
pub(crate) struct SelectionDrag {
    anchor: Boundary,
    /// The scrollport the host's autoscroll keeps the pointer in as it selects, in CSS pixels of the viewport.
    scrollport: Option<FfiFloatRect>,
}

impl SelectionDrag {
    /// The drag anchored in the node `anchor` names, if it names one: `anchor_offset` code units into it where it is a
    /// text node, or right before its child `anchor_child` otherwise, or after its last child for 0. A `scrollport`
    /// without an area stands for none.
    pub(crate) fn new(anchor: u32, anchor_offset: usize, anchor_child: u32, scrollport: FfiFloatRect) -> Option<Self> {
        let node = StyleNodeID::from_raw(anchor)?;
        let anchor = match node.text_index() {
            Some(_) => Boundary::InText {
                node,
                offset: anchor_offset,
            },
            None => Boundary::InElement {
                container: node,
                child: StyleNodeID::from_raw(anchor_child),
            },
        };
        Some(Self {
            anchor,
            scrollport: (scrollport.width > 0.0 && scrollport.height > 0.0).then_some(scrollport),
        })
    }

    /// `point`, in CSS pixels of the viewport, as the host's `AutoScrollHandler::process` keeps it in the scrollport.
    fn constrained(self, x: f32, y: f32) -> (f32, f32) {
        let Some(scrollport) = self.scrollport else {
            return (x, y);
        };
        (
            x.clamp(scrollport.x, scrollport.x + scrollport.width - 1.0),
            y.clamp(scrollport.y, scrollport.y + scrollport.height - 1.0),
        )
    }
}

/// A boundary point in the DOM, among the nodes the style engine mirrors. Comments and processing instructions have no
/// place there, and no box to highlight.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Boundary {
    InText {
        node: StyleNodeID,
        offset: usize,
    },
    /// Right before `child`, or after the last of the children of `container` for none.
    InElement {
        container: StyleNodeID,
        child: Option<StyleNodeID>,
    },
}

impl Boundary {
    /// The boundary point right before `node`.
    fn before(engine: &StyleEngine, node: StyleNodeID) -> Option<Self> {
        Some(Self::InElement {
            container: engine.drag_parent(node)?,
            child: Some(node),
        })
    }

    /// The boundary point right after `node`.
    fn after(engine: &StyleEngine, node: StyleNodeID) -> Option<Self> {
        Some(Self::InElement {
            container: engine.drag_parent(node)?,
            child: engine.drag_next_sibling(node),
        })
    }

    /// The node of the boundary point.
    fn container(self) -> StyleNodeID {
        match self {
            Self::InText { node, .. } => node,
            Self::InElement { container, .. } => container,
        }
    }

    /// Whether this boundary point comes before `other`, in the tree they share, as
    /// `position_of_boundary_point_relative_to_other_boundary_point` decides it.
    fn precedes(self, engine: &StyleEngine, other: Self) -> bool {
        let (container, other_container) = (self.container(), other.container());
        // The child of `ancestor` that holds `node`.
        let holder_of = |node, ancestor| {
            std::iter::successors(Some(node), |&node| engine.drag_parent(node))
                .find(|&node| engine.drag_parent(node) == Some(ancestor))
        };
        let child_of = |boundary| match boundary {
            Self::InText { .. } => None,
            Self::InElement { child, .. } => child,
        };
        match (self, other) {
            _ if container == other_container => match (self, other) {
                (
                    Self::InText { offset, .. },
                    Self::InText {
                        offset: other_offset, ..
                    },
                ) => offset < other_offset,
                (Self::InElement { child: Some(child), .. }, Self::InElement { child: other_child, .. }) => {
                    other_child.is_none_or(|other_child| engine.drag_precedes(child, other_child))
                }
                _ => false,
            },
            _ if engine.drag_is_inclusive_ancestor(container, other_container) => {
                let (Some(child), Some(holder)) = (child_of(self), holder_of(other_container, container)) else {
                    return false;
                };
                child == holder || engine.drag_precedes(child, holder)
            }
            _ if engine.drag_is_inclusive_ancestor(other_container, container) => {
                let (Some(other_child), Some(holder)) = (child_of(other), holder_of(container, other_container)) else {
                    return child_of(other).is_none();
                };
                engine.drag_precedes(holder, other_child)
            }
            _ => engine.drag_precedes(container, other_container),
        }
    }
}

/// A node a selection highlights: its style node, its selection state, where the selection starts and ends, and the
/// background and text colors its `::selection` styles give it, as ARGB, zero for none. For a test.
pub(in crate::render_state) type SelectionRowForTesting = (u32, u8, usize, usize, u32, u32);

/// The background and text colors the `::selection` styles of `row` give it, as ARGB, zero for none.
fn selection_colors_for_testing(arena: &LayoutNodeArena, row: NodeSlotId) -> (u32, u32) {
    let styles = &arena.paint_state().selection_pseudo_styles;
    let element_row = std::iter::successors(arena.node_parent_if_live(row), |&row| arena.node_parent_if_live(row))
        .find(|&row| arena.node_flags_if_live(row) & NodeFlag::Anonymous as u32 == 0);
    styles
        .get(&row)
        .or_else(|| element_row.and_then(|row| styles.get(&row)))
        .map_or((0, 0), |answer| {
            let text = answer.facts.text_color;
            (
                answer.facts.background_color.0,
                if text.has_value { text.value.0 } else { 0 },
            )
        })
}

/// The nodes the selection of `state` highlights, in the order of their style nodes. For a test.
pub(super) fn rows_for_testing(state: &RenderState) -> Vec<SelectionRowForTesting> {
    let arena = state.arena.arena();
    let Some(selection) = arena.paint_state().selection.clone() else {
        return Vec::new();
    };
    let texts = selection
        .text_states
        .iter()
        .map(|(&row, &state)| (row, row_dom_style_node(arena, row), state));
    let boxes = selection
        .boxes_with_selection_state
        .iter()
        .filter(|&&row| arena.paintable_row_is_populated(row))
        .map(|&row| {
            (
                row,
                row_dom_style_node(arena, row),
                arena.paintable_data(row).selection_state,
            )
        });
    let mut rows: Vec<_> = texts
        .chain(boxes)
        .filter(|&(_, node, state)| node != 0 && state != SELECTION_STATE_NONE)
        .map(|(row, node, state)| {
            let (background, text) = selection_colors_for_testing(arena, row);
            (
                node,
                state,
                selection.start_offset,
                selection.end_offset,
                background,
                text,
            )
        })
        .collect();
    rows.sort_unstable();
    rows.dedup();
    rows
}

/// Why a tick left a move of a drag to the host.
struct LeftToHost(&'static str);

/// The boundary point of the caret position `caret` as the focus of a drag anchored at `anchor`, with the node the caret
/// stands at, as `choose_caret_boundary_for_selection_focus` decides it: an atomic box has a caret position on both of
/// its sides, of which the focus takes the one away from the anchor.
fn focus_at(
    engine: &StyleEngine,
    caret: &FfiResolvedCaret,
    anchor: Boundary,
) -> Result<(Boundary, StyleNodeID), LeftToHost> {
    const OUTSIDE_OF_A_TREE: LeftToHost = LeftToHost("a boundary outside of a tree");
    if caret.node.is_document {
        return Err(LeftToHost("a boundary in the document"));
    }
    let node = StyleNodeID::from_raw(caret.node.style_node).ok_or(LeftToHost("a boundary in no node"))?;
    let focus = match caret.boundary {
        FfiCaretBoundaryKind::Offset if node.text_index().is_some() => Boundary::InText {
            node,
            offset: caret.offset,
        },
        // NB: The offset counts the comments the mirror does not hold.
        FfiCaretBoundaryKind::Offset => return Err(LeftToHost("a boundary among the children of an element")),
        FfiCaretBoundaryKind::IndexOfNodeInParent => Boundary::before(engine, node).ok_or(OUTSIDE_OF_A_TREE)?,
        FfiCaretBoundaryKind::BeforeNode | FfiCaretBoundaryKind::AfterNode => {
            let before = Boundary::before(engine, node).ok_or(OUTSIDE_OF_A_TREE)?;
            let after = Boundary::after(engine, node).ok_or(OUTSIDE_OF_A_TREE)?;
            if engine.drag_root(node) != engine.drag_root(anchor.container()) {
                match caret.boundary {
                    FfiCaretBoundaryKind::BeforeNode => before,
                    _ => after,
                }
            } else if anchor.precedes(engine, before) {
                after
            } else if after.precedes(engine, anchor) {
                before
            } else {
                match caret.boundary {
                    FfiCaretBoundaryKind::BeforeNode => before,
                    _ => after,
                }
            }
        }
    };
    Ok((focus, node))
}

/// The boundary point the host's `set_user_selection` leaves the focus `focus` at `node` of a drag anchored at `anchor`
/// on: a focus at a node whose `user-select` is `none` moves out of the outermost element of that value, to the side of it
/// that faces the anchor. What `all` and a `contain` element do to a selection stays with the host.
fn focus_after_user_select(
    engine: &StyleEngine,
    focus: Boundary,
    node: StyleNodeID,
    anchor: Boundary,
) -> Result<Boundary, LeftToHost> {
    let (container, anchor_container) = (focus.container(), anchor.container());
    if engine.drag_root(container) != engine.drag_root(anchor_container) {
        return Err(LeftToHost("a focus in another tree"));
    }
    let common = std::iter::successors(Some(anchor_container), |&node| engine.drag_parent(node))
        .find(|&node| engine.drag_is_inclusive_ancestor(node, container));
    for start in [anchor_container, container] {
        let contained = std::iter::successors(Some(start), |&node| engine.drag_parent(node))
            .take_while(|&node| Some(node) != common)
            .filter(|node| node.element_index().is_some())
            .any(|element| engine.drag_used_user_select(element) == UsedUserSelect::Contain);
        if contained {
            return Err(LeftToHost("a user-select: contain between the anchor and the focus"));
        }
    }
    // NB: The host asks the node of the boundary point, which for an atomic box is its parent, and the box where that
    //     node has no box of its own.
    let decides = match engine.drag_has_no_box_of_its_own(container) {
        true => node,
        false => container,
    };
    let used = engine
        .drag_style_element(decides)
        .map_or(UsedUserSelect::None, |element| engine.drag_used_user_select(element));
    match used {
        UsedUserSelect::All => Err(LeftToHost("a focus user-select: all widens")),
        UsedUserSelect::None if container == anchor_container => {
            Err(LeftToHost("a focus user-select: none keeps at the anchor"))
        }
        UsedUserSelect::None => {
            let is_none = |node: &StyleNodeID| {
                node.element_index().is_some() && engine.drag_used_user_select(*node) == UsedUserSelect::None
            };
            let outermost = std::iter::successors(engine.drag_parent(container), |&node| engine.drag_parent(node))
                .take_while(is_none)
                .last()
                .unwrap_or(container);
            match engine.drag_precedes(outermost, anchor_container) {
                true => Boundary::after(engine, outermost),
                false => Boundary::before(engine, outermost),
            }
            .ok_or(LeftToHost("a focus user-select: none moves out of the tree"))
        }
        UsedUserSelect::Text | UsedUserSelect::Contain => Ok(focus),
    }
}

/// The highlight of a range, as `DocumentPaintState::recompute_selection_states` makes it.
struct Highlight<'a> {
    engine: &'a StyleEngine,
    arena: &'a LayoutNodeArena,
    selection_styles: &'a SelectionStyles,
    used_user_select: HashMap<StyleNodeID, UsedUserSelect>,
    entries: Vec<FfiSelectionEntry>,
    /// The elements of the entries whose rows have yet to get their `::selection` styles.
    unstyled: HashSet<StyleNodeID>,
}

impl Highlight<'_> {
    /// Whether `node` is left out of the selection, as `user-select: none` and inertness do.
    fn excludes(&mut self, node: StyleNodeID) -> bool {
        if self.engine.drag_is_inert(node) {
            return true;
        }
        if self.arena.bound_row(node).is_invalid() {
            return false;
        }
        let Some(element) = self.engine.drag_style_element(node) else {
            return true;
        };
        let engine = self.engine;
        *self
            .used_user_select
            .entry(element)
            .or_insert_with(|| engine.drag_used_user_select(element))
            == UsedUserSelect::None
    }

    fn mark(&mut self, node: StyleNodeID, state: u8) {
        if self.excludes(node) {
            return;
        }
        let layout_node = self.arena.bound_row(node);
        if !layout_node.is_invalid() {
            self.entries.push(FfiSelectionEntry {
                is_text_node_entry: node.text_index().is_some(),
                layout_node,
                state,
            });
            if let Some(element) = self.engine.drag_style_element(node)
                && self.selection_styles.owes(element)
            {
                self.unstyled.insert(element);
            }
        }
    }

    /// Marks the nodes of the range from `start` to `end`, as `for_each_node_in_highlight_range` does.
    fn mark_range(&mut self, start: Boundary, end: Boundary) {
        let engine = self.engine;
        if start == end {
            return;
        }
        let (start_container, end_container) = (start.container(), end.container());
        if start_container == end_container && start_container.text_index().is_some() {
            self.mark(start_container, SELECTION_STATE_START_AND_END);
            return;
        }
        let start_at = match start {
            Boundary::InText { node, .. } => {
                self.mark(node, SELECTION_STATE_START);
                engine.drag_next_in_pre_order_within(node, end_container)
            }
            Boundary::InElement { container, child } => {
                self.mark(container, SELECTION_STATE_FULL);
                child.or_else(|| engine.drag_next_after_subtree(container, end_container))
            }
        };
        let stop_at = match end {
            Boundary::InText { .. } => None,
            Boundary::InElement { child, .. } => child,
        };
        let mut next = start_at;
        while let Some(node) = next {
            if Some(node) == stop_at || (node == end_container && !engine.drag_has_children(node)) {
                break;
            }
            self.mark(node, SELECTION_STATE_FULL);
            next = engine.drag_next_in_pre_order_within(node, end_container);
        }
        if let Boundary::InText { node, .. } = end {
            self.mark(node, SELECTION_STATE_END);
        }
    }
}

impl Lane {
    /// The drag selection the lane follows, if any.
    pub(super) fn selection_drag(&self) -> Option<SelectionDrag> {
        self.plan.hover.as_ref()?.selection_drag()
    }

    /// Has the highlight the ticks show follow nothing more: the button went up, and a press after it is the host's.
    pub(super) fn end_selection_drag(&mut self) {
        if let Some(plan) = &mut self.plan.hover {
            plan.end_selection_drag();
        }
        self.selection_hit_test_list = None;
        self.selection_styles = None;
    }

    /// Has the highlight of the drag follow `pointer`, in the frame as the screen shows it at `frame_time_nanoseconds`,
    /// and answers whether that moved anything to present.
    pub(super) fn extend_selection(
        &mut self,
        state: &mut RenderState,
        pointer: hover::PendingPointer,
        frame_time_nanoseconds: i64,
    ) -> bool {
        let Some(drag) = self.selection_drag() else {
            return false;
        };
        let started = std::time::Instant::now();
        let moved = self.show_selection_at(state, drag, pointer, frame_time_nanoseconds);
        if hover::logs_hover() {
            let outcome = match &moved {
                Ok(true) => "extended",
                Ok(false) => "unmoved",
                Err(LeftToHost(reason)) => reason,
            };
            eprintln!(
                "{} hover lane: drag to {:?}: {outcome} in {} us",
                hover::log_time(),
                pointer.position,
                started.elapsed().as_micros()
            );
        }
        moved.unwrap_or(false)
    }

    fn show_selection_at(
        &mut self,
        state: &mut RenderState,
        drag: SelectionDrag,
        pointer: hover::PendingPointer,
        frame_time_nanoseconds: i64,
    ) -> Result<bool, LeftToHost> {
        let plan = self.plan.hover.take().ok_or(LeftToHost("no plan"))?;
        let shown = self.show_selection_with(state, &plan, drag, pointer, frame_time_nanoseconds);
        self.plan.hover = Some(plan);
        shown
    }

    fn show_selection_with(
        &mut self,
        state: &mut RenderState,
        plan: &hover::HoverPlan,
        drag: SelectionDrag,
        pointer: hover::PendingPointer,
        frame_time_nanoseconds: i64,
    ) -> Result<bool, LeftToHost> {
        let position = pointer.position.ok_or(LeftToHost("the pointer left the document"))?;
        // A scroll of the compositor's since the frame moved what is under the pointer, which only a tick that said where
        // it scrolled to tells, and the scrollport the plan sealed along with it.
        if pointer.scrolled_since_frame && (self.scroll_offsets.is_empty() || drag.scrollport.is_some()) {
            return Err(LeftToHost("scrolled since the frame"));
        }
        let (tree, published) = self
            .presented_hit_test_items(state, frame_time_nanoseconds)
            .map_err(LeftToHost)?;
        let arena = state.arena.arena();
        let engine = state.engine_ref();
        // NB: The spatial indexes take a walk over every item, which a drag need not repeat over the same frame.
        let list = self
            .selection_hit_test_list
            .take()
            .filter(|list| Arc::ptr_eq(&list.items, &published.items))
            .unwrap_or_else(|| {
                let mut list = HitTestList {
                    items: Arc::clone(&published.items),
                    ..HitTestList::default()
                };
                list.build_spatial_indexes_if_needed();
                list
            });
        let scroll_offsets = self.scroll_offsets_on_screen(arena, &arena.paint_state().visual_context, plan);
        let list = self.selection_hit_test_list.insert(list);
        let callbacks = FfiHitTestQueryCallbacks::sealed(
            plan.device_pixels_per_css_pixel(),
            &scroll_offsets,
            plan.chrome_metrics(),
        );
        let world = callbacks.build_world_transforms(&tree);
        let callbacks = callbacks.with_world_transforms(&world);
        let pixel_ratio = plan.device_pixels_per_css_pixel() as f32;
        let css_point =
            |x, y| CssPixelPoint::new(CssPixels::nearest_value_for_f32(x), CssPixels::nearest_value_for_f32(y));
        let (x, y) = (position.x / pixel_ratio, position.y / pixel_ratio);
        // The host hands a move over the content of another navigable to it, and the selection here stays as it is.
        let over_a_frame = list
            .find_topmost_item(arena, &tree, &callbacks, css_point(x, y))
            .and_then(|topmost| arena.node_kind_if_live(list.items[topmost.index].paintable))
            == Some(NodeKind::NavigableContainerViewport);
        if over_a_frame {
            return Err(LeftToHost("the content of another navigable"));
        }
        let (x, y) = drag.constrained(x, y);
        let caret = list
            .caret_position_from_point(arena, &tree, &callbacks, css_point(x, y), NodeSlotId::INVALID)
            .filter(|caret| caret.caret.has_position)
            .ok_or(LeftToHost("no caret position"))?;
        let (focus, focus_node) = focus_at(engine, &caret.caret, drag.anchor)?;
        let focus = focus_after_user_select(engine, focus, focus_node, drag.anchor)?;
        if self.selection_focus_shown == Some(focus) {
            return Ok(false);
        }
        let (start, end) = match focus.precedes(engine, drag.anchor) {
            true => (focus, drag.anchor),
            false => (drag.anchor, focus),
        };
        let selection_styles = self.selection_styles.get_or_insert_with(|| SelectionStyles::of(engine));
        let mut highlight = Highlight {
            engine,
            arena,
            selection_styles,
            used_user_select: HashMap::new(),
            entries: Vec::new(),
            unstyled: HashSet::new(),
        };
        highlight.mark_range(start, end);
        let Highlight { entries, unstyled, .. } = highlight;
        let engine = state.engine_mut();
        let records = unstyled
            .into_iter()
            .map(|element| Some((element, engine.demand_selection_record(element)?)))
            .collect::<Option<Vec<_>>>()
            .ok_or(LeftToHost("a ::selection style the host resolves"))?;
        let offset_in = |boundary| match boundary {
            Boundary::InText { offset, .. } => offset,
            Boundary::InElement { .. } => 0,
        };
        let arena = state.arena.arena_mut();
        for (element, record) in records {
            sync_selection_style(arena, element, record);
            selection_styles.note_styled(element);
        }
        let selection = crate::painting::selection::apply(
            &mut arena.paintable_rows_mut(),
            &entries,
            offset_in(start),
            offset_in(end),
        );
        arena.paint_state_mut().selection = Some(Arc::new(selection));
        self.selection_focus_shown = Some(focus);
        Ok(true)
    }
}
