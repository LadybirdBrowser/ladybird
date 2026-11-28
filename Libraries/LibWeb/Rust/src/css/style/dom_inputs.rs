/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host's writes of a document's style engine that read what they write from the DOM in place: C++ hands over the
//! nodes, and the entries read the nodes' cores on the main thread, as they write the host's side of the mirror.

use super::bridge::{
    FfiElementArrival, FfiFeatureKind, FfiFeatureValueKind, FfiLocalFeatureDelta, FfiTreeDelta, FfiTreeRelations,
};
use super::dom_tree_relations::{
    PreviousSiblings, arrived_element_at_or_after, arrived_element_at_or_before, dom_order_link_of, relations_of,
    tree_scope_of,
};
use super::engine_calls::{EngineWrite, TextArrival};
use super::host_atoms;
use super::index::{AttributeFact, StyleAtomID};
use super::projection_writes::ElementArrivalFacts;
use super::tree::StyleNodeID;
use crate::dom::element::Element;
use crate::dom::node::{Node, NodeCell};
use crate::dom::node_core::NodeType;
use crate::dom::strings::{FlyStringField, StringField};
use crate::render_state::{ArenaChange, DocumentHost};

crate::stage::main_thread_ffi_entries!();

/// A node a struct holds, which C++ sees as an opaque pointer: the GC plugin rejects a raw cell pointer field, and a
/// `GC::Ref` field would keep the structs C++ gathers out of its vectors.
#[repr(transparent)]
pub struct FfiNode<'a>(&'a NodeCell);

/// A node or null that a struct holds, which C++ sees as an opaque pointer, as it sees an FfiNode.
#[repr(transparent)]
pub struct FfiNullableNode<'a>(Option<&'a NodeCell>);

/// A text node of an arriving subtree, and its identity and what it is beyond the characters it holds.
#[repr(C)]
pub struct FfiArrivingText<'a> {
    pub text: FfiNode<'a>,
    pub node: u32,
    pub is_in_user_agent_shadow_tree: bool,
    pub is_password_input: bool,
}

/// Writes each text node of an arriving subtree, with the characters it holds, in one write.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and each text a live text node.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_text_arrivals(
    host: &DocumentHost,
    texts: ak::Span<'_, FfiArrivingText<'_>>,
) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    let texts: Box<[TextArrival]> = texts
        .as_slice()
        .iter()
        .filter_map(|text| {
            Some(TextArrival {
                node: StyleNodeID::from_raw(text.node)?,
                // SAFETY: Guaranteed by the caller.
                data: unsafe { Node::from_cell(text.text.0, &main_thread) }
                    .data()
                    .to_utf16_string(),
                is_in_user_agent_shadow_tree: text.is_in_user_agent_shadow_tree,
                is_password_input: text.is_password_input,
            })
        })
        .collect();
    if !texts.is_empty() {
        host.queue_change(ArenaChange::Engine(EngineWrite::TextArrivals(texts)));
    }
}

/// Records the characters the text node `text`, whose identity is `node`, holds.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `text` a live text node.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_text_data(host: &DocumentHost, node: u32, text: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: Guaranteed by the caller.
    let data = unsafe { Node::from_cell(text, &main_thread) }.data().to_utf16_string();
    host.queue_change(ArenaChange::Engine(EngineWrite::TextData { node, data }));
}

/// An element of an arriving subtree, with the facts its selectors test that the host gathers, and whether it takes its
/// place in the tree with them, which it does as its core places it there, assigned to the slot whose identity is
/// `assigned_slot`, or zero. Elements that took their place in the tree earlier arrive with `has_relations` false.
#[repr(C)]
pub struct FfiElementArrivalRow<'a> {
    pub element: FfiNode<'a>,
    pub arrival: FfiElementArrival,
    pub unique_node_id: u64,
    pub assigned_slot: u32,
    pub has_relations: bool,
}

/// Writes the arrival of each element of `rows` into its projection row in one go, with every fact its selectors test:
/// those the row carries, and those read from the element itself, which are its name, ID, classes and attributes, and
/// whether a text child keeps it from being `:empty`. `custom_states` holds each row's custom states in turn. Where
/// `fold_ids_and_classes`, as in a quirks-mode document, IDs and classes are published in ASCII lowercase. Keeps what
/// the elements arrive with beside their rows for the host's next barrier.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, each row's element a live element, and the DOM must
/// not change for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_element_arrival_rows(
    host: &DocumentHost,
    rows: ak::Span<'_, FfiElementArrivalRow<'_>>,
    custom_states: ak::Span<'_, u32>,
    fold_ids_and_classes: bool,
) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    let rows = rows.as_slice();
    let mut custom_states = custom_states.as_slice();

    // NB: Interning writes beside the projection rows, so every element's names are interned before the rows are
    //     written.
    let mut arriving_names = ArrivingNames::new(host, fold_ids_and_classes);
    let elements: Vec<ElementNames> = rows
        .iter()
        .map(|row| {
            // SAFETY: Guaranteed by the caller.
            let element = unsafe { Node::from_cell(row.element.0, &main_thread) }
                .as_element()
                .expect("an arriving element is an element");
            arriving_names.of(element)
        })
        .collect();

    let memo = host.engine_memo();
    let mut backing_elements = Vec::new();
    {
        let mut projection = memo.projection.borrow_mut();
        let mut writes = projection.writes();
        for (row, names) in rows.iter().zip(&elements) {
            let arrival = &row.arrival;
            let (row_custom_states, rest) = custom_states.split_at(arrival.custom_state_count as usize);
            custom_states = rest;
            let Some(node) = StyleNodeID::from_raw(arrival.node) else {
                debug_assert!(false, "an element arrival named an invalid style node");
                continue;
            };
            let facts = ElementArrivalFacts {
                tag: names.tag,
                folded_tag: names.folded_tag,
                namespace: names.namespace,
                id: names.id,
                has_text_content: names.has_text_content,
                classes: &arriving_names.classes[names.classes.clone()],
                attributes: &arriving_names.attributes[names.attributes.clone()],
            };
            let relations = row.has_relations.then(|| {
                // SAFETY: Guaranteed by the caller.
                let element = unsafe { Node::from_cell(row.element.0, &main_thread) }
                    .as_element()
                    .expect("an arriving element is an element");
                relations_of(element, PreviousSiblings::Arrived, row.assigned_slot).decode()
            });
            writes.record_element_arrival(node, arrival, &facts, row_custom_states, relations);
            if arrival.associated_pseudo_kind_plus_one != 0 {
                backing_elements.push(node);
            }
        }
    }
    for node in backing_elements {
        memo.note_backing_element(node);
    }

    let ids: Box<[(StyleNodeID, i64)]> = rows
        .iter()
        .filter(|row| row.has_relations)
        .filter_map(|row| {
            Some((
                StyleNodeID::from_raw(row.arrival.node)?,
                row.unique_node_id.cast_signed(),
            ))
        })
        .collect();
    if !ids.is_empty() {
        host.queue_change(ArenaChange::Engine(EngineWrite::ElementUniqueNodeIds(ids)));
    }
    // NB: The engine finds an element by its ID as it is written, in every mode.
    for (row, names) in rows.iter().zip(&elements) {
        if let Some(name) = names.id_name
            && let Some(node) = StyleNodeID::from_raw(row.arrival.node)
        {
            host.queue_change(ArenaChange::Engine(EngineWrite::ElementIdName { node, name }));
        }
    }
}

/// An element of an arriving subtree that takes its place in the tree as its core places it there, assigned to the slot
/// whose identity is `assigned_slot`, or zero, with the unique node id the document names it by. Its features follow.
#[repr(C)]
pub struct FfiArrivingElement<'a> {
    pub element: FfiNode<'a>,
    pub unique_node_id: u64,
    pub assigned_slot: u32,
}

/// The relations an element stands in before it stands in none.
const DETACHED: FfiTreeRelations = FfiTreeRelations {
    parent: 0,
    previous_element_sibling: 0,
    next_element_sibling: 0,
    tree_scope: 0,
    assigned_slot: 0,
};

/// The element a cell an entry is handed is.
///
/// # Safety
///
/// `cell` must be a live element, and the DOM must not change for `'a`.
unsafe fn element_of<'a>(cell: &'a NodeCell, main_thread: &'a crate::stage::MainThread<'_>) -> Element<'a> {
    // SAFETY: Guaranteed by the caller.
    unsafe { Node::from_cell(cell, main_thread) }
        .as_element()
        .expect("the node is an element")
}

/// Writes the arrival of each element of an arriving subtree into the host's projection rows in one call, as a tree
/// delta from standing in no tree, and the unique node ids the document names them by.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, each element a live element that has taken its
/// identity, and the DOM must not change for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_element_arrivals(
    host: &DocumentHost,
    elements: ak::Span<'_, FfiArrivingElement<'_>>,
) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    let elements = elements.as_slice();
    let mut ids = Vec::with_capacity(elements.len());
    {
        let mut projection = host.engine_memo().projection.borrow_mut();
        let mut writes = projection.writes();
        for arriving in elements {
            // SAFETY: Guaranteed by the caller.
            let element = unsafe { element_of(arriving.element.0, &main_thread) };
            let Some(node) = element.node().core().style_node_id() else {
                debug_assert!(false, "an arriving element has no identity");
                continue;
            };
            writes.record_ffi_tree_delta(&FfiTreeDelta {
                node: node.raw(),
                old_connected: false,
                new_connected: true,
                old_relations: DETACHED,
                new_relations: relations_of(element, PreviousSiblings::Arrived, arriving.assigned_slot),
            });
            ids.push((node, arriving.unique_node_id.cast_signed()));
        }
    }
    if !ids.is_empty() {
        host.queue_change(ArenaChange::Engine(EngineWrite::ElementUniqueNodeIds(ids.into())));
    }
}

/// A node C++ hands over as a GC::Ref<DOM::Node>.
#[repr(transparent)]
pub struct NodeRef<'a>(&'a NodeCell);

/// Links each element and text node of `nodes`, which have taken their identities, into its parent's child sequence of
/// the engine's tree, after the nearest previous sibling that has an identity. The nodes come in tree order, so each
/// previous sibling among them takes its place first.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, each node a live element or text node, and the DOM
/// must not change for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_link_nodes_in_dom_order(host: &DocumentHost, nodes: ak::Span<'_, NodeRef<'_>>) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    let links: Box<[u32]> = nodes
        .as_slice()
        .iter()
        // SAFETY: Guaranteed by the caller.
        .flat_map(|node| dom_order_link_of(unsafe { Node::from_cell(node.0, &main_thread) }))
        .collect();
    host.queue_change(ArenaChange::Engine(EngineWrite::DomOrderLinks(links)));
}

/// Writes the departure of the element `element` from the tree, as a tree delta to standing in no tree from where its
/// core places it, assigned to the slot whose identity is `assigned_slot`, or zero. Where `previous_siblings_departed`,
/// its previous siblings have left before it, as below the root of a departing subtree.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, `element` a live element that holds its identity,
/// and the DOM must not change for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_element_departure(
    host: &DocumentHost,
    element: &NodeCell,
    previous_siblings_departed: bool,
    assigned_slot: u32,
) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: Guaranteed by the caller.
    let element = unsafe { element_of(element, &main_thread) };
    let previous_siblings = match previous_siblings_departed {
        true => PreviousSiblings::Departed,
        false => PreviousSiblings::Arrived,
    };
    host.engine_memo()
        .projection
        .borrow_mut()
        .writes()
        .record_ffi_tree_delta(&FfiTreeDelta {
            node: element.node().core().style_node_id().map_or(0, StyleNodeID::raw),
            old_connected: true,
            new_connected: false,
            old_relations: relations_of(element, previous_siblings, assigned_slot),
            new_relations: DETACHED,
        });
}

/// Writes the departure of the shadow root `shadow_root` from the tree, as a tree delta to standing in no tree from
/// standing in its own scope.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, `shadow_root` a live shadow root that holds its
/// identity, and the DOM must not change for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_shadow_root_departure(host: &DocumentHost, shadow_root: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: Guaranteed by the caller.
    let shadow_root = unsafe { Node::from_cell(shadow_root, &main_thread) };
    host.engine_memo()
        .projection
        .borrow_mut()
        .writes()
        .record_ffi_tree_delta(&FfiTreeDelta {
            node: shadow_root.core().style_node_id().map_or(0, StyleNodeID::raw),
            old_connected: true,
            new_connected: false,
            old_relations: FfiTreeRelations {
                tree_scope: tree_scope_of(shadow_root).0,
                ..DETACHED
            },
            new_relations: DETACHED,
        });
}

/// Writes the relinking of the element `element`, which stays in the tree where its core places it, assigned to the
/// slot whose identity is `assigned_slot`, or zero: before, it was assigned to the slot whose identity is
/// `old_assigned_slot`, and stood in the tree scope `old_tree_scope` where one is given, and otherwise as it does now.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, `element` a live element that holds its identity,
/// and the DOM must not change for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_element_relinked(
    host: &DocumentHost,
    element: &NodeCell,
    assigned_slot: u32,
    old_assigned_slot: u32,
    old_tree_scope: Option<&u32>,
) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: Guaranteed by the caller.
    let element = unsafe { element_of(element, &main_thread) };
    let new_relations = relations_of(element, PreviousSiblings::Arrived, assigned_slot);
    let old_relations = FfiTreeRelations {
        assigned_slot: old_assigned_slot,
        tree_scope: old_tree_scope.copied().unwrap_or(new_relations.tree_scope),
        ..new_relations
    };
    host.engine_memo()
        .projection
        .borrow_mut()
        .writes()
        .record_ffi_tree_delta(&FfiTreeDelta {
            node: element.node().core().style_node_id().map_or(0, StyleNodeID::raw),
            old_connected: true,
            new_connected: true,
            old_relations,
            new_relations,
        });
}

/// Where an element stood before a move that kept it in the tree: its parent, and its previous and next siblings, each
/// as it is now or null, and the identity of the slot it was assigned to, or zero.
#[repr(C)]
pub struct FfiOldPlace<'a> {
    pub parent: FfiNullableNode<'a>,
    pub previous_sibling: FfiNullableNode<'a>,
    pub next_sibling: FfiNullableNode<'a>,
    pub assigned_slot: u32,
}

/// The relinking of the element `element`, which moved from `old_place` to where its core places it now, assigned to
/// the slot whose identity is `assigned_slot`, or zero, for the host to stage once it has published what else the move
/// moves. A sibling relation on either side names the nearest sibling that has an identity, starting at the sibling
/// itself.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, `element` a live element that holds its identity, the
/// nodes of `old_place` live, and the DOM must not change for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_element_move(
    host: &DocumentHost,
    element: &NodeCell,
    assigned_slot: u32,
    old_place: &FfiOldPlace<'_>,
) -> FfiTreeDelta {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: Guaranteed by the caller.
    let element = unsafe { element_of(element, &main_thread) };
    fn node<'a>(cell: Option<&'a NodeCell>, main_thread: &'a crate::stage::MainThread<'_>) -> Option<Node<'a>> {
        // SAFETY: Guaranteed by the caller of the entry.
        cell.map(|cell| unsafe { Node::from_cell(cell, main_thread) })
    }
    let raw = |node: Option<StyleNodeID>| node.map_or(0, StyleNodeID::raw);
    let new_relations = relations_of(element, PreviousSiblings::Arrived, assigned_slot);
    let old_parent = node(old_place.parent.0, &main_thread);
    // NB: The parent the element left is a node of the style tree whether or not it is an element: a shadow root is
    //     one, and reporting no parent at all would say the element crossed out of the tree rather than moved inside
    //     it.
    let old_relations = FfiTreeRelations {
        parent: raw(old_parent
            .filter(|parent| parent.is_element() || parent.is_shadow_root())
            .and_then(|parent| parent.core().style_node_id())),
        previous_element_sibling: raw(arrived_element_at_or_before(node(
            old_place.previous_sibling.0,
            &main_thread,
        ))),
        next_element_sibling: raw(arrived_element_at_or_after(node(
            old_place.next_sibling.0,
            &main_thread,
        ))),
        tree_scope: old_parent.map_or(new_relations.tree_scope, |parent| tree_scope_of(parent.root()).0),
        assigned_slot: old_place.assigned_slot,
    };
    FfiTreeDelta {
        node: raw(element.node().core().style_node_id()),
        old_connected: true,
        new_connected: true,
        old_relations,
        new_relations,
    }
}

/// Stages a change of one local fact of an element, where the element's initial features are published, as they say
/// what it is until they are. Answers whether this staged the host's first input since its last barrier.
fn stage_local_feature(host: &DocumentHost, is_published: bool, delta: &FfiLocalFeatureDelta) -> bool {
    if !is_published {
        return false;
    }
    let memo = host.engine_memo();
    let first = !memo.has_unpublished_writes();
    memo.projection.borrow_mut().writes().record_ffi_local_feature(delta);
    first
}

/// Stages the element `element`, whose identity is `node`, taking the ID its core holds now: as the fact its selectors
/// test, folded where `fold_ids_and_classes`, where `is_published` says its initial features are, and as the ID the
/// engine finds it by. Answers whether this staged the host's first input since its last barrier.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `element` a live element.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_element_id_change(
    host: &DocumentHost,
    element: &NodeCell,
    node: u32,
    is_published: bool,
    fold_ids_and_classes: bool,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: Guaranteed by the caller.
    let element = unsafe { Node::from_cell(element, &main_thread) }
        .as_element()
        .expect("an element changes its ID");
    let id = element.id().map(|id| {
        // SAFETY: The element's ID lives for the call.
        let name = unsafe { host_atoms::intern(host, id.raw_identity()) };
        let atom = match fold_ids_and_classes {
            // SAFETY: As above.
            true => unsafe { host_atoms::intern_id_or_class(host, id.raw_identity(), true) },
            false => name,
        };
        (atom, name)
    });
    let first = stage_local_feature(
        host,
        is_published,
        &FfiLocalFeatureDelta {
            node,
            feature_kind: FfiFeatureKind::Id,
            name_atom: 0,
            new_kind: id.map_or(FfiFeatureValueKind::Absent, |_| FfiFeatureValueKind::Atom),
            new_atom: id.map_or(0, |(atom, _)| atom.0),
        },
    );
    // NB: The engine finds an element by its ID as it is written, in every mode.
    if let Some(node) = StyleNodeID::from_raw(node) {
        host.queue_change(ArenaChange::Engine(EngineWrite::ElementIdName {
            node,
            name: id.map_or(StyleAtomID::NONE, |(_, name)| name),
        }));
    }
    first
}

/// Stages the element `element`, whose identity is `node`, taking the classes its core holds now in place of
/// `old_classes`: one fact per class that joined or left, folded where `fold_ids_and_classes`, where `is_published`
/// says its initial features are. Answers whether this staged the host's first input since its last barrier.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `element` a live element.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_element_class_change(
    host: &DocumentHost,
    element: &NodeCell,
    node: u32,
    is_published: bool,
    fold_ids_and_classes: bool,
    old_classes: ak::Span<'_, FlyStringField>,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: Guaranteed by the caller.
    let element = unsafe { Node::from_cell(element, &main_thread) }
        .as_element()
        .expect("an element changes its classes");
    let mut first = false;
    let record_membership = |class: usize, is_present: bool| {
        // SAFETY: The class, or its folding, lives for the call.
        let atom = unsafe { host_atoms::intern(host, class) };
        first |= stage_local_feature(
            host,
            is_published,
            &FfiLocalFeatureDelta {
                node,
                feature_kind: FfiFeatureKind::Class,
                name_atom: atom.0,
                new_kind: match is_present {
                    true => FfiFeatureValueKind::Present,
                    false => FfiFeatureValueKind::Absent,
                },
                new_atom: 0,
            },
        );
    };
    let old_classes = old_classes.as_slice();
    if !fold_ids_and_classes {
        for_each_class_membership_change(
            old_classes,
            element.classes(),
            FlyStringField::raw_identity,
            record_membership,
        );
        return first;
    }
    // NB: Two classes that differ only in case are one class where the document folds them, so membership is decided
    //     between the folded names rather than the written ones.
    let fold = |classes: &[FlyStringField]| -> Vec<(usize, Option<ak::Utf16FlyString>)> {
        classes
            .iter()
            .map(|class| {
                // SAFETY: The class lives for the call.
                let folded = unsafe { host_atoms::fold_id_or_class(class.raw_identity(), true) };
                let identity = folded
                    .as_ref()
                    .map_or(class.raw_identity(), ak::Utf16FlyString::raw_identity);
                (identity, folded)
            })
            .collect()
    };
    for_each_class_membership_change(
        &fold(old_classes),
        &fold(element.classes()),
        |&(identity, _)| identity,
        record_membership,
    );
    first
}

/// Calls `changed` with the identity of each class of `old_classes` missing from `new_classes`, and then of each of
/// `new_classes` missing from `old_classes`, with whether it is present now. A class on both sides is no change.
fn for_each_class_membership_change<T>(
    old_classes: &[T],
    new_classes: &[T],
    identity: impl Fn(&T) -> usize,
    mut changed: impl FnMut(usize, bool),
) {
    let contains = |classes: &[T], class: usize| classes.iter().any(|other| identity(other) == class);
    for class in old_classes.iter().map(&identity) {
        if !contains(new_classes, class) {
            changed(class, false);
        }
    }
    for class in new_classes.iter().map(&identity) {
        if !contains(old_classes, class) {
            changed(class, true);
        }
    }
}

/// Stages the element whose identity is `node` taking the value `value`, or none where it is gone, of its attribute in
/// `namespace`, zero for none, called `local_name`: as the fact its selectors test, where `is_published` says its
/// initial features are, unless nothing of the engine reads the name, in which case the name is withheld from the
/// engine instead. Answers whether this staged the host's first input since its last barrier.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, `local_name` the raw identity of a live
/// `AK::Utf16FlyString`, and `namespace` that of a live non-empty one, or zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_element_attribute_change(
    host: &DocumentHost,
    node: u32,
    is_published: bool,
    local_name: usize,
    namespace: usize,
    value: Option<&StringField>,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let name = unsafe { host_atoms::intern_attribute_name(host, local_name, namespace) };
    // OPTIMIZATION: Nothing of the engine reads this name, so the element's row need not hear of the change. The name
    //               is noted as an arrival withholding it is, and is published if something comes to read it.
    if host_atoms::attribute_is_known_unread(host, name) {
        // SAFETY: Guaranteed by the caller.
        unsafe { host_atoms::note_attribute_withheld(host, local_name, namespace) };
        return false;
    }
    let value = value.map(|value| {
        let value = value.to_fly_string();
        // SAFETY: The value lives for the call.
        unsafe { host_atoms::intern_attribute_value(host, name, value.raw_identity()) }
    });
    stage_local_feature(
        host,
        is_published,
        &FfiLocalFeatureDelta {
            node,
            feature_kind: FfiFeatureKind::Attribute,
            name_atom: name.0,
            new_kind: value.map_or(FfiFeatureValueKind::Absent, |_| FfiFeatureValueKind::Atom),
            new_atom: value.map_or(0, |value| value.0),
        },
    )
}

/// What the names of an arriving element are published as.
struct ElementNames {
    tag: StyleAtomID,
    folded_tag: StyleAtomID,
    namespace: StyleAtomID,
    /// The element's ID as its selectors test it, or none.
    id: StyleAtomID,
    /// The element's ID as it is written, where it has one.
    id_name: Option<StyleAtomID>,
    has_text_content: bool,
    classes: core::ops::Range<usize>,
    attributes: core::ops::Range<usize>,
}

/// What the names of one block of arriving elements are published as, with what is found out by name along the way.
/// Names repeat, so what one element answers holds for the next one asking.
struct ArrivingNames<'h> {
    host: &'h DocumentHost,
    fold_ids_and_classes: bool,
    /// The tag, folded tag and namespace atoms of each qualified name of an element.
    kinds: TableByName<(StyleAtomID, StyleAtomID, StyleAtomID), 64>,
    /// The atom of each qualified name of an attribute, and whether nothing of the engine reads it.
    attribute_names: TableByName<(StyleAtomID, bool), 32>,
    classes: Vec<StyleAtomID>,
    attributes: Vec<AttributeFact>,
}

impl<'h> ArrivingNames<'h> {
    fn new(host: &'h DocumentHost, fold_ids_and_classes: bool) -> Self {
        Self {
            host,
            fold_ids_and_classes,
            kinds: TableByName::default(),
            attribute_names: TableByName::default(),
            classes: Vec::new(),
            attributes: Vec::new(),
        }
    }

    fn of(&mut self, element: Element<'_>) -> ElementNames {
        let host = self.host;
        let name = element.core().qualified_name();
        let (tag, folded_tag, namespace) = self.kinds.get_or_insert(name.identity(), || {
            // SAFETY: The element's name lives for the call, and its parts with it.
            unsafe {
                let tag = host_atoms::intern(host, name.local_name().raw_identity());
                let lowercased = name.lowercased_local_name().raw_identity();
                let folded_tag = match lowercased == name.local_name().raw_identity() {
                    true => tag,
                    false => host_atoms::intern(host, lowercased),
                };
                let namespace = match name.namespace().filter(|namespace| !namespace.is_empty()) {
                    Some(namespace) => host_atoms::intern(host, namespace.raw_identity()),
                    None => StyleAtomID::NONE,
                };
                (tag, folded_tag, namespace)
            }
        });

        let (id, id_name) = match element.id() {
            Some(id) => {
                // SAFETY: The element's ID lives for the call.
                let name = unsafe { host_atoms::intern(host, id.raw_identity()) };
                let atom = match self.fold_ids_and_classes {
                    // SAFETY: As above.
                    true => unsafe { host_atoms::intern_id_or_class(host, id.raw_identity(), true) },
                    false => name,
                };
                (atom, Some(name))
            }
            None => (StyleAtomID::NONE, None),
        };

        let classes_start = self.classes.len();
        for class in element.classes() {
            // SAFETY: The class lives for the call.
            let atom = unsafe { host_atoms::intern_id_or_class(host, class.raw_identity(), self.fold_ids_and_classes) };
            self.classes.push(atom);
        }

        let attributes_start = self.attributes.len();
        for attribute in element.attributes() {
            let attribute_name = attribute.name();
            let (name, is_known_unread) = self.attribute_names.get_or_insert(attribute_name.identity(), || {
                let local_name = attribute_name.local_name().raw_identity();
                let namespace = attribute_name
                    .namespace()
                    .filter(|namespace| !namespace.is_empty())
                    .map_or(0, FlyStringField::raw_identity);
                // SAFETY: The attribute's name lives for the call, and its parts with it.
                let name = unsafe { host_atoms::intern_attribute_name(host, local_name, namespace) };
                let is_known_unread = host_atoms::attribute_is_known_unread(host, name);
                if is_known_unread {
                    // SAFETY: As above.
                    unsafe { host_atoms::note_attribute_withheld(host, local_name, namespace) };
                }
                (name, is_known_unread)
            });
            if is_known_unread {
                continue;
            }
            let value = attribute.value().to_fly_string();
            // SAFETY: The value lives for the call.
            let value = unsafe { host_atoms::intern_attribute_value(host, name, value.raw_identity()) };
            self.attributes.push(AttributeFact { name, value });
        }

        ElementNames {
            tag,
            folded_tag,
            namespace,
            id,
            id_name,
            has_text_content: element.node().children().any(|child| {
                matches!(child.node_type(), NodeType::TEXT_NODE | NodeType::CDATA_SECTION_NODE)
                    && !child.data().is_empty()
            }),
            classes: classes_start..self.classes.len(),
            attributes: attributes_start..self.attributes.len(),
        }
    }
}

/// A table of what was found out by name, in `SLOTS` slots, a power of two, each answering one name at a time: a name
/// that shares a slot with another only costs asking again.
struct TableByName<T, const SLOTS: usize> {
    slots: [(usize, T); SLOTS],
}

impl<T: Copy + Default, const SLOTS: usize> Default for TableByName<T, SLOTS> {
    fn default() -> Self {
        Self {
            slots: [(0, T::default()); SLOTS],
        }
    }
}

impl<T: Copy, const SLOTS: usize> TableByName<T, SLOTS> {
    /// What the table holds for the name named by `identity`, which is never zero, found out by `find` where it holds
    /// nothing for it.
    fn get_or_insert(&mut self, identity: usize, find: impl FnOnce() -> T) -> T {
        // Fibonacci hashing: the high bits of the product depend on all of an allocation's address.
        let hash = identity.wrapping_mul(0x9e37_79b9_7f4a_7c15) >> (usize::BITS - SLOTS.trailing_zeros());
        let slot = &mut self.slots[hash];
        if slot.0 != identity {
            *slot = (identity, find());
        }
        slot.1
    }
}
