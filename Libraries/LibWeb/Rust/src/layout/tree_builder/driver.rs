/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a layout tree build builds: the child lists it plans again, found from the nodes the DOM marked since the last
//! build, without walking down to them. A node whose child list or pseudo-elements changed has its own list planned
//! again. A node whose box is built again is an entry of the list of the box it is laid out in now, and of the list its
//! box was in before, which may differ: a top layer member's box is in the viewport's list, which is the document's. A
//! restyled element is both: the list it is an entry of decides whether its box stays, and its own list is planned
//! again where it does.
//! Each list is planned again once, outermost first, and an inner list whose box an outer one built again is done.

use super::list_unit::{UnitPlace, update_child_list};
use super::*;

/// The box whose child list a build plans again.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
enum UnitRoot {
    /// The viewport, whose list is the box of the document element and those of the top layer members.
    Viewport,
    /// The box of an element.
    Element(StyleNodeID),
}

/// A child list the build plans again: its box, as it was when the build found the list, and the box's pre-order label,
/// which puts the lists in the order the build plans them.
struct Unit {
    label: u64,
    root: UnitRoot,
    row: NodeSlotId,
}

/// Plans again the child lists the marked nodes ask for, and retires every mark the build answers.
pub(super) fn update_marked_child_lists(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    document: StyleNodeID,
) {
    let marks = host.arena_mut().layout_tree_update_marks_mut();
    let dirty = marks.take_dirty();
    let removed_entries = removed_entries_by_list_box(host.arena_mut());

    let mut roots = Vec::new();
    for &node in &dirty {
        roots_of_marked_node(host.arena(), document, node, &mut roots);
    }
    if top_layer_member_box_is_stale(host) {
        roots.push(UnitRoot::Viewport);
    }
    let mut units: Vec<Unit> = roots
        .into_iter()
        .filter_map(|root| {
            let row = row_of(host.arena(), root);
            (!row.is_invalid()).then(|| Unit {
                label: host.arena().node_pre_order_label(row),
                root,
                row,
            })
        })
        .collect();
    units.sort_by_key(|unit| unit.label);
    units.dedup_by_key(|unit| unit.row);
    let mut marked_children = marked_children_of_window_candidates(host.arena(), &units, &dirty);

    for unit in &units {
        let marked_children = match unit.root {
            UnitRoot::Viewport => Vec::new(),
            UnitRoot::Element(element) => marked_children.remove(&element).unwrap_or_default(),
        };
        let first = removed_entries.partition_point(|&(list_box, _)| slot_order(list_box) < slot_order(unit.row));
        let removed_entries = removed_entries[first..]
            .iter()
            .take_while(|&&(list_box, _)| list_box == unit.row)
            .map(|&(_, entry)| entry);
        run_unit(host, state, document, unit, marked_children, removed_entries);
    }

    retire_marks(host.arena_mut(), &dirty);
}

/// Builds the document's whole tree, below a viewport the build makes for it: the first build of a document, or one
/// asked for every box again, whose viewport the host puts in place of the one the document had.
pub(super) fn build_document(host: &mut TreeBuilderHost<'_>, state: &mut TreeBuilderState, document: StyleNodeID) {
    let marks = host.arena_mut().layout_tree_update_marks_mut();
    let dirty = marks.take_dirty();
    // NB: The new viewport's lists hold none of the rows the removals left.
    marks.take_removed_entries();
    let document_style = state
        .document_style
        .take()
        .expect("a build that builds the viewport is handed the document's style");
    let viewport = host.create_viewport_box(document_style);
    host.arena_mut().owe_image_resources(
        viewport,
        OwedImageResources::StyleResources {
            owns_content_replacement_image: false,
        },
    );
    host.arena_mut().set_layout_root(viewport);
    planned_build::build_document_descendants(host, state, viewport, document);
    retire_marks(host.arena_mut(), &dirty);
}

/// The entries the removals since the last build left in lists whose boxes stay (see
/// [`super::note_removed_list_entries`]), each with its list's box, ordered by that box. An entry an earlier change of
/// the tree freed since is none of them. The entries of a list go in its order.
fn removed_entries_by_list_box(arena: &mut LayoutNodeArena) -> Vec<(NodeSlotId, NodeSlotId)> {
    let entries = arena.layout_tree_update_marks_mut().take_removed_entries();
    let mut entries: Vec<(NodeSlotId, NodeSlotId)> = entries
        .into_iter()
        .filter(|&entry| arena.slot_is_live(entry))
        .map(|entry| (super::list_box_of_entry(arena, entry), entry))
        .filter(|&(list_box, _)| !list_box.is_invalid())
        .collect();
    entries.sort_unstable_by_key(|&(list_box, entry)| (slot_order(list_box), arena.node_pre_order_label(entry)));
    entries
}

/// An order of rows, by which [`removed_entries_by_list_box`] groups entries.
fn slot_order(slot: NodeSlotId) -> (u32, u8) {
    (slot.slot_index(), slot.generation())
}

/// The row of the box of `root`.
fn row_of(arena: &LayoutNodeArena, root: UnitRoot) -> NodeSlotId {
    match root {
        UnitRoot::Viewport => arena.layout_root(),
        UnitRoot::Element(element) => arena.bound_row(element),
    }
}

/// Adds the boxes whose child lists the mark of `node` asks the build to plan again to `roots`.
fn roots_of_marked_node(arena: &LayoutNodeArena, document: StyleNodeID, node: StyleNodeID, roots: &mut Vec<UnitRoot>) {
    let marks = arena.layout_tree_update_marks();
    if !marks.needs(node) {
        return;
    }
    if node == document {
        roots.push(UnitRoot::Viewport);
        return;
    }
    // NB: A shadow root's mark marked its host.
    if node.element_index().is_none() && node.text_index().is_none() {
        return;
    }
    let row = arena.bound_row(node);
    let is_attached = !row.is_invalid() && !arena.data(row).parent.get().is_invalid();
    let reasons = marks.reuse_reasons(node);
    let is_restyle = reasons & layout_tree_update_reuse_reason::RESTYLE != 0;
    // NB: A node without a box that its list or pseudo-elements changed of is a `display: contents` element, whose
    //     parent the DOM marked with it, a node in a subtree without boxes, or a node just inserted, whose parent's mark
    //     plans its list. Such a node asks for nothing of its own.
    if reasons != 0 && !is_restyle && (!is_attached || node.element_index().is_none()) {
        return;
    }
    // NB: The content of an SVG resource is built below the boxes of the elements that reference it, which a change of
    //     it marked.
    if lies_in_svg_resource(arena, node) {
        return;
    }
    if reasons != 0 {
        if is_attached && node.element_index().is_some() {
            roots.push(UnitRoot::Element(node));
        }
        // NB: Whether a restyled element keeps its box is decided in the list it is an entry of.
        if !is_restyle {
            return;
        }
    }
    // The list the node's box goes in now.
    let is_top_layer_member = node.element_index().is_some()
        && arena.element_adjustment_facts(Some(node)) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0;
    let new_root = if is_top_layer_member || arena.first_dom_child(document) == Some(node) {
        Some(UnitRoot::Viewport)
    } else {
        arena.layout_parent_with_box(node).map(UnitRoot::Element)
    };
    roots.extend(new_root);
    // The list its box was in, which differs where it entered or left the top layer.
    if is_attached && let Some(root) = list_root_of_row(arena, row) {
        roots.push(root);
    }
}

/// The marked nodes in the lists of the elements whose lists only gained or lost children, by element: the nodes whose
/// boxes go in the list, which is what a list planned from a window checks the children outside the window against.
/// Other lists are planned whole, and need none.
fn marked_children_of_window_candidates(
    arena: &LayoutNodeArena,
    units: &[Unit],
    dirty: &[StyleNodeID],
) -> HashMap<StyleNodeID, Vec<StyleNodeID>> {
    let marks = arena.layout_tree_update_marks();
    let candidates: HashSet<StyleNodeID> = units
        .iter()
        .filter_map(|unit| match unit.root {
            UnitRoot::Element(element)
                if marks.reuse_reasons(element) != 0
                    && marks.reuse_reasons(element)
                        & !(layout_tree_update_reuse_reason::CHILD_LIST_INSERTION
                            | layout_tree_update_reuse_reason::CHILD_LIST_REMOVAL)
                        == 0 =>
            {
                Some(element)
            }
            _ => None,
        })
        .collect();
    let mut marked_children: HashMap<StyleNodeID, Vec<StyleNodeID>> = HashMap::default();
    if candidates.is_empty() {
        return marked_children;
    }
    arena.with_style_store(|engine| {
        for &node in dirty {
            if !marks.needs(node) {
                continue;
            }
            // NB: A node below a marked node without a box, such as one inserted with it, changes nothing the marked
            //     node does not: that node's own entry, or the one it is flattened below, is checked.
            let dom_parent = match node.text_index() {
                Some(_) => engine.text_parent(node),
                None => engine.dom_parent(node),
            };
            if dom_parent.is_some_and(|parent| marks.needs(parent) && arena.bound_row(parent).is_invalid()) {
                continue;
            }
            let layout_parent = arena.layout_parent_with_box(node);
            if let Some(parent) = layout_parent.filter(|parent| candidates.contains(parent)) {
                marked_children.entry(parent).or_default().push(node);
            }
            // NB: The boxes of a node, or of the children of a `display: contents` one, that moved into or out of the
            //     top layer leave the list of the nearest box above it in the DOM, or enter it, which the node's mark
            //     tells that list too.
            let mut ancestor = dom_parent;
            while let Some(current) = ancestor.filter(|&current| arena.bound_row(current).is_invalid()) {
                ancestor = engine.dom_parent(current);
            }
            if let Some(ancestor) =
                ancestor.filter(|ancestor| Some(*ancestor) != layout_parent && candidates.contains(ancestor))
            {
                marked_children.entry(ancestor).or_default().push(node);
            }
        }
    });
    marked_children
}

/// Whether `node` is in the content of an SVG mask, clip path or pattern, or is one.
fn lies_in_svg_resource(arena: &LayoutNodeArena, node: StyleNodeID) -> bool {
    use element_adjustment_fact::{
        IS_SVG_CLIP_PATH_ELEMENT, IS_SVG_ELEMENT, IS_SVG_MASK_ELEMENT, IS_SVG_PATTERN_ELEMENT,
    };
    arena.with_style_store(|engine| {
        let mut current = if node.text_index().is_some() {
            engine.text_parent(node)
        } else {
            Some(node)
        };
        while let Some(element) = current.filter(|element| element.element_index().is_some()) {
            let facts = engine.element_adjustment_facts(element);
            if facts & IS_SVG_ELEMENT == 0 {
                return false;
            }
            if facts & (IS_SVG_MASK_ELEMENT | IS_SVG_CLIP_PATH_ELEMENT | IS_SVG_PATTERN_ELEMENT) != 0 {
                return true;
            }
            current = engine.parent_element_or_host(element);
        }
        false
    })
}

/// The box whose child list holds the box `row`: its parent, past the anonymous boxes the list made.
fn list_root_of_row(arena: &LayoutNodeArena, row: NodeSlotId) -> Option<UnitRoot> {
    let parent = super::list_box_of_entry(arena, arena.list_entry_top_row(row));
    if parent.is_invalid() {
        return None;
    }
    if arena.data(parent).kind.get() == NodeKind::Viewport {
        return Some(UnitRoot::Viewport);
    }
    let element = arena.dom_node_style_node(parent)?;
    (element.element_index().is_some() && arena.bound_row(element) == parent).then_some(UnitRoot::Element(element))
}

/// Whether a top layer member has a box in the tree where it should have none, or none where it should have one: an
/// ancestor that hid it was shown, or one that shows it was hidden, which the build of the ancestor's list passes by,
/// as the member's box is in the viewport's list.
fn top_layer_member_box_is_stale(host: &TreeBuilderHost<'_>) -> bool {
    let arena = host.arena();
    (0..).map_while(|index| arena.top_layer_element(index)).any(|member| {
        if !host.rendered_in_top_layer(Some(member)) {
            return false;
        }
        let Some(display) = arena.published_box_facts(Some(member)).map(|facts| facts.display) else {
            return false;
        };
        // NB: A `display: contents` member has no box of its own.
        if display.is_contents() {
            return false;
        }
        let row = arena.bound_row(member);
        let has_box = !row.is_invalid() && !arena.data(row).parent.get().is_invalid();
        has_box != (!display.is_none() && !has_unrendered_flat_tree_ancestor(host, Some(member)))
    })
}

/// Plans the child list of `unit` again, where an earlier list did not build its box again or take it away.
fn run_unit(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    document: StyleNodeID,
    unit: &Unit,
    marked_children: Vec<StyleNodeID>,
    removed_entries: impl Iterator<Item = NodeSlotId>,
) {
    let arena = host.arena();
    if row_of(arena, unit.root) != unit.row
        || arena.data(unit.row).parent.get().is_invalid() && unit.root != UnitRoot::Viewport
    {
        return;
    }
    let element = match unit.root {
        UnitRoot::Viewport => document,
        UnitRoot::Element(element) => {
            // NB: A box built again is an entry of its parent's list, which built it, and its list with it.
            let marks = arena.layout_tree_update_marks();
            if marks.needs(element) && marks.reuse_reasons(element) == 0 {
                return;
            }
            element
        }
    };
    let mut ancestor_rows = Vec::new();
    let mut ancestor = arena.data(unit.row).parent.get();
    while !ancestor.is_invalid() {
        ancestor_rows.push(ancestor);
        ancestor = arena.data(ancestor).parent.get();
    }
    ancestor_rows.reverse();
    // NB: The walk enters SVG content at the box of an `<svg>`, and leaves it at that of a `<foreignObject>`.
    let kind = arena.data(unit.row).kind.get();
    let place = UnitPlace {
        is_viewport: unit.root == UnitRoot::Viewport,
        has_svg_root: node_facts::kind_is_svg_content(kind),
        ancestor_rows,
        marked_children,
        removed_entries: removed_entries.collect(),
    };
    update_child_list(host, state, unit.row, element, &place);
}

/// Retires the marks of `dirty`, the nodes the build was asked to build. A node marked during the build keeps its mark
/// for the next build.
fn retire_marks(arena: &mut LayoutNodeArena, dirty: &[StyleNodeID]) {
    let marked_during_build = arena.layout_tree_update_marks_mut().take_dirty();
    let marked_again: HashSet<StyleNodeID> = marked_during_build.iter().copied().collect();
    let marks = arena.layout_tree_update_marks_mut();
    for &node in dirty {
        if !marked_again.contains(&node) {
            marks.clear(node);
        }
    }
    marks.keep_dirty(marked_during_build);
}
