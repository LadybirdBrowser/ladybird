/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The writes and reads the host makes of a document's style engine that the [`super::boundary`] table does not list.
//! A write is a change the document's render state applies to the engine in the order the host made them; a read is a
//! closure the host runs on the render state and waits for. Each entry takes the document host, never the engine, and
//! owns what it hands over: borrowed arrays are copied, and the host's objects are referenced, as the entry queues the
//! change.

use super::StyleEngine;
use super::atoms::{AtomKey, AtomLease};
use super::bridge::{
    FfiDemandedPseudoElement, FfiElementArrival, FfiElementDeclarationDelta, FfiLocalFeatureDelta,
    FfiPseudoElementRecordDemand, FfiRecordDemand, FfiRecordDemandAnswer, FfiStateDelta, FfiTreeDelta,
    FfiTreeRelations, borrow,
};
use super::font_resolution::{FontResolverHost, PublishedFontFaces};
use super::inputs::RetainedCustomPropertyData;
use super::instrumentation::Counter;
use super::pipeline::projection::ProjectionPublication;
use super::publication::RecordDemand;
use super::random_bases::ParkedBaseValues;
use super::tree::{StyleNodeID, TreeScopeID};
use super::{HashMap, StyleAtomID};
use crate::css::transition::{FfiTransitionAction, FfiTransitionInput, TransitionDecision};
use crate::render_state::{ArenaChange, BegunRead, DocumentHost};
use std::ffi::c_void;
use std::sync::Arc;

/// One hand-written write of the host to a document's style engine.
pub(crate) enum EngineWrite {
    /// The document's `@font-face` table and the memo of the cascades resolved from it, and the shadow tree scopes
    /// whose `@font-feature-values` the table carries beside the document's.
    PublishFontFaces {
        font_faces: PublishedFontFaces,
        feature_values_shadow_scopes: Box<[TreeScopeID]>,
    },
    /// The custom-property environment an element holds, or none.
    ElementCustomPropertyData {
        node: StyleNodeID,
        data: Option<RetainedCustomPropertyData>,
        identity: u64,
        sampled_over: Option<u64>,
    },
    /// The custom-property environment one of an element's synthetic pseudo-elements holds, or none.
    PseudoElementCustomPropertyData {
        node: StyleNodeID,
        pseudo: u8,
        data: Option<RetainedCustomPropertyData>,
        identity: u64,
        /// The environment its animation overlay lies over, where it holds one.
        sampled_over: Option<u64>,
    },
    /// The host minted these identities, which the engine's tree makes live ahead of anything recorded about them.
    MintStyleNodes(Box<[u32]>),
    /// The unique node ids the document names the elements of an arriving subtree by.
    ElementUniqueNodeIds(Box<[(StyleNodeID, i64)]>),
    /// The text nodes of an arriving subtree, with what each holds.
    TextArrivals(Box<[TextArrival]>),
    /// The host ended the style update it took last: the engine lets go of the records it pinned for the host's
    /// layout, and releases the identities no reader can name any more, which the job that applies it pays the host
    /// to mint again.
    EndStyleUpdate { pinned: Box<[u64]> },
    /// What the host wrote of its elements since its last barrier, published at this one.
    Publish(Box<HostPublication>),
    /// The hover moves to the element, or off the document for none, once what the host wrote before is in place.
    MoveHover(Option<StyleNodeID>),
    /// The host wrote a shadow host's root, which it did not have before where `attached`.
    ShadowRoot { host: StyleNodeID, attached: bool },
    /// A slot's assigned nodes moved, which can hide or show its fallback content under the hover.
    SlotAssignedNodes,
    /// The element moved into or out of the top layer, as its host facts the host published say.
    TopLayerMove { node: StyleNodeID },
    /// The element asks for another principal box, as its host facts the host published say.
    BoxKindChange { node: StyleNodeID },
    /// Text identities retire as their nodes disconnect.
    RetireTexts(Box<[StyleNodeID]>),
    /// The root of a shadow tree's scope.
    TreeScopeRoot { tree_scope: TreeScopeID, root: StyleNodeID },
    /// The characters a text node holds.
    TextData { node: u32, data: ak::Utf16String },
    /// The presentational hints of one kind an element declares, expanded to the longhands they decide.
    PresentationalHints {
        node: StyleNodeID,
        kind: super::bridge::FfiElementDeclarationKind,
        properties: Box<[crate::css::declaration_block::DeclaredProperty]>,
    },
    /// The custom properties an element's inline declaration block declares, which the engine names in atoms of its own.
    /// The rest of the block reaches it with the host's rows.
    InlineCustomProperties {
        node: StyleNodeID,
        data: Option<crate::css::declaration_block::SharedDeclarationBlock>,
    },
    /// A name the host interned, which the engine adopts as the atom the host's lease holds.
    AdoptAtom(AtomLease),
    /// An element loses its style node, and keeps the random base values of the keys that name it in `slot`.
    ParkRandomBaseValues { node: StyleNodeID, slot: ParkedBaseValues },
    /// An element's new style node takes back the random base values the element kept in `slot`.
    UnparkRandomBaseValues { node: StyleNodeID, slot: ParkedBaseValues },
    /// The store behind an environment an element holds, and what a child inherits of it, `inheritable` with its
    /// store.
    NoteCustomPropertyEnvironment {
        identity: u64,
        store: Option<std::sync::Arc<crate::css::custom_properties::CustomPropertyStore>>,
        inheritable: u64,
        inheritable_store: Option<std::sync::Arc<crate::css::custom_properties::CustomPropertyStore>>,
    },
    /// The `@keyframes` row of one style scope, named by the shadow root's pointer identity as well where it is one.
    AnimationKeyframes {
        tree_scope: TreeScopeID,
        row: super::animations::KeyframesRow,
    },
    /// The `@position-try` row of one style scope.
    PositionTryRules {
        tree_scope: TreeScopeID,
        row: super::position_try::PositionTryRow,
    },
    /// The effects one of an element's animation lists holds, in composite order, described for the engine to sample
    /// them from.
    AnimationEffectDescriptions {
        node: StyleNodeID,
        slot: super::animations::AnimationSlot,
        effects: Box<[super::effect_descriptions::PublishedEffect]>,
    },
    /// The timing the host sampled each of an element's own effects with, by identity, where it sampled without asking.
    AnimationEffectTimings {
        node: StyleNodeID,
        timings: super::animations::SampledEffectTimings,
    },
}

/// What the host publishes at a barrier: its projection rows, and what its elements arrived with and declare beside
/// them, which the render owner keeps elsewhere.
pub(crate) struct HostPublication {
    projection: ProjectionPublication,
    /// The elements published standing for an element-backed pseudo-element of their shadow host, which the render
    /// owner finds them by.
    backing_elements: Box<[StyleNodeID]>,
    declarations: Box<[FfiElementDeclarationDelta]>,
}

impl HostPublication {
    /// How many elements' links the publication wrote, each of which may deepen the tree by a level.
    pub(crate) fn link_rows_written(&self) -> usize {
        self.projection.link_rows_written()
    }
}

/// What the host writes beside its projection rows until its next barrier publishes them.
#[derive(Default)]
pub(crate) struct UnpublishedInputs {
    backing_elements: Vec<StyleNodeID>,
    declarations: Vec<FfiElementDeclarationDelta>,
}

impl EngineWrite {
    /// Whether the write may move a fact the host knows of the render state. What a text, an attribute name or an
    /// attribute value spells never does: it stages no style input and touches no layout box.
    pub(crate) fn may_move_facts(&self) -> bool {
        !matches!(self, Self::TextData { .. } | Self::EndStyleUpdate { .. })
    }

    pub(crate) fn apply(self, engine: &mut StyleEngine) {
        match self {
            Self::EndStyleUpdate { pinned } => {
                for record in pinned {
                    engine.unpin_layout_style_record(record);
                }
                engine.release_style_update_identities();
            }
            Self::PublishFontFaces {
                font_faces,
                feature_values_shadow_scopes,
            } => {
                match &mut engine.host.font_resolver {
                    Some(resolver) => resolver.publish(font_faces),
                    None => engine.host.font_resolver = Some(FontResolverHost::new(font_faces)),
                }
                let font_resolution = engine.retained.font_resolution.get_or_insert_default();
                font_resolution.forget_earlier_generations();
                font_resolution.publish_feature_values_shadow_scopes(feature_values_shadow_scopes);
            }
            Self::ElementCustomPropertyData {
                node,
                data,
                identity,
                sampled_over,
            } => {
                // The children of an element style styles inherit what its animations
                // sample into its custom properties. Where an installation let go of its
                // composition, what moved is told once the host settled it again.
                let held_composition = match engine.composition_released(node) {
                    true => None,
                    false => engine.hold_composition(node),
                };
                engine.set_element_custom_property_data(node, data, identity, sampled_over);
                if let Some(held_composition) = held_composition {
                    engine.note_composition_moved(node, held_composition);
                }
            }
            Self::PseudoElementCustomPropertyData {
                node,
                pseudo,
                data,
                identity,
                sampled_over,
            } => engine.set_pseudo_element_custom_property_data(node, pseudo, data, identity, sampled_over),
            Self::MintStyleNodes(nodes) => engine.mint_style_nodes(&nodes),
            Self::ElementUniqueNodeIds(ids) => {
                for &(node, unique_node_id) in &ids {
                    engine.set_element_unique_node_id(node, unique_node_id);
                }
            }
            Self::TextArrivals(texts) => {
                for text in texts {
                    engine.set_text_is_ascii_whitespace(text.node, text.is_ascii_whitespace);
                    engine.set_text_is_in_user_agent_shadow_tree(text.node, text.is_in_user_agent_shadow_tree);
                    engine.set_text_is_password_input(text.node, text.is_password_input);
                    engine.set_text_data(text.node, text.data);
                }
            }
            Self::Publish(publication) => {
                let HostPublication {
                    projection,
                    backing_elements,
                    declarations,
                } = *publication;
                engine.adopt_host_publication(projection, &backing_elements, &declarations);
            }
            Self::MoveHover(target) => engine.set_hover(target),
            Self::ShadowRoot { host, attached } => engine.note_shadow_root(host, attached),
            Self::RetireTexts(nodes) => engine.retire_text_style_nodes(nodes),
            Self::TopLayerMove { node } => engine.note_top_layer_move(node),
            Self::BoxKindChange { node } => engine.note_box_kind_change(node),
            Self::TreeScopeRoot { tree_scope, root } => engine.set_tree_scope_root(tree_scope, root),
            Self::SlotAssignedNodes => {
                // Text-only assignments do not carry element tree deltas, but can hide or restore fallback content.
                if let Some(target) = engine.hover_target() {
                    engine.set_hover(Some(target));
                }
            }
            Self::TextData { node, data } => {
                if let Some(node) = StyleNodeID::from_raw(node) {
                    engine.set_text_data(node, data);
                }
            }
            Self::PresentationalHints { node, kind, properties } => {
                super::bridge::register_element_declared_properties(
                    engine,
                    node,
                    kind,
                    properties
                        .iter()
                        .map(crate::css::declaration_block::DeclaredProperty::borrowed),
                );
            }
            Self::AnimationKeyframes { tree_scope, row } => engine.animation_keyframes.set(tree_scope, row),
            Self::PositionTryRules { tree_scope, row } => engine.position_try_rules.set(tree_scope, row),
            Self::AnimationEffectDescriptions { node, slot, effects } => {
                engine.animation_effect_descriptions.set(node, slot, effects);
            }
            Self::AnimationEffectTimings { node, timings } => {
                for (identity, timing) in timings {
                    engine
                        .animation_effect_descriptions
                        .keep_timing(node, 0, identity, timing);
                }
            }
            Self::InlineCustomProperties { node, data } => {
                super::bridge::register_element_custom_properties(
                    engine,
                    node,
                    data.as_ref()
                        .map_or(Default::default(), |data| data.custom_properties()),
                );
            }
            Self::AdoptAtom(lease) => {
                debug_assert!(lease.is_of(engine.retained.atoms.process_atoms()));
                let atom = match lease.key() {
                    AtomKey::Raw(raw) => engine.intern_atom(raw).0,
                    AtomKey::Qualified(namespace, name) => engine.intern_qualified_atom(namespace, name).0,
                };
                // The engine took a reference of its own, so the global atom stays the one the lease held.
                assert_eq!(atom, lease.atom().0, "a document adopts the atom its host interned");
            }
            Self::ParkRandomBaseValues { node, slot } => {
                *slot.lock().expect("a parked row is never poisoned") =
                    engine.random_base_values.take_element_values(node);
            }
            Self::UnparkRandomBaseValues { node, slot } => {
                let row = std::mem::take(&mut *slot.lock().expect("a parked row is never poisoned"));
                engine.set_element_random_base_values(node, row);
            }
            Self::NoteCustomPropertyEnvironment {
                identity,
                store,
                inheritable,
                inheritable_store,
            } => {
                super::bridge::note_custom_property_environment(
                    engine,
                    identity,
                    store.as_ref(),
                    inheritable,
                    inheritable_store.as_ref(),
                );
            }
        }
    }
}

/// The parts the element `node` exposes, each with the shadow host it is exposed to, or none for no element.
fn element_parts(node: u32, names: &[u32], hosts: &[u32]) -> Option<(StyleNodeID, Vec<(StyleAtomID, StyleNodeID)>)> {
    let node = StyleNodeID::from_raw(node)?;
    let pairs = names
        .iter()
        .zip(hosts)
        .filter_map(|(name, part_host)| {
            StyleNodeID::from_raw(*part_host).map(|part_host| (StyleAtomID(*name), part_host))
        })
        .collect();
    Some((node, pairs))
}

/// Queues `write` for the render state of `host`'s document.
pub(super) fn queue(host: &DocumentHost, write: EngineWrite) {
    host.queue_change(ArenaChange::Engine(write));
}

/// Publishes the document's `@font-face` table and the memo of the cascades resolved from it, which every later font
/// resolution of the engine reads. Takes one reference to each. The shadow tree scopes that declare
/// `@font-feature-values` are the ones whose values the table carries beside the document's.
///
/// # Safety
/// `host` must be a live document host, `snapshot` and `memo` a live `Web::CSS::FontFaceSnapshot` and
/// `FontCascadeMemo`, and `feature_values_shadow_scopes` must point at `feature_values_shadow_scope_count` scopes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_publish_font_faces(
    host: &DocumentHost,
    snapshot: *const c_void,
    memo: *const c_void,
    feature_values_shadow_scopes: *const u32,
    feature_values_shadow_scope_count: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let font_faces = unsafe { PublishedFontFaces::adopt(snapshot, memo) };
    // SAFETY: Guaranteed by the caller.
    let feature_values_shadow_scopes =
        unsafe { borrow(feature_values_shadow_scopes, feature_values_shadow_scope_count) }
            .iter()
            .map(|&scope| TreeScopeID(scope))
            .collect();
    let write = EngineWrite::PublishFontFaces {
        font_faces,
        feature_values_shadow_scopes,
    };
    host.queue_change(ArenaChange::Engine(write));
}

/// Keeps the custom-property environment an element now holds, named by `identity`: for the element's animation
/// overlay, and the environment `base` its style resolved to beneath it. A null `data` is none.
///
/// # Safety
/// `host` must be a live document host, and `data` null or a live `Web::CSS::CustomPropertyData`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_custom_property_data(
    host: &DocumentHost,
    node: u32,
    data: *const c_void,
    identity: u64,
    is_animation_overlay: bool,
    base: u64,
) {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
    host.engine_memo()
        .held
        .borrow_mut()
        .follow_element(node, HeldEnvironment::new(data, is_animation_overlay));
    // SAFETY: Guaranteed by the caller.
    let data = (!data.is_null()).then(|| unsafe { RetainedCustomPropertyData::retain(data) });
    let write = EngineWrite::ElementCustomPropertyData {
        node,
        data,
        identity,
        sampled_over: is_animation_overlay.then_some(base),
    };
    host.queue_change(ArenaChange::Engine(write));
}

/// Keeps the custom-property environment one of an element's synthetic pseudo-elements now holds, named by
/// `identity`: for the pseudo-element's animation overlay, the environment `base` its style resolved to beneath it. A
/// null `data` is none.
///
/// # Safety
/// `host` must be a live document host, and `data` null or a live `Web::CSS::CustomPropertyData`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_pseudo_element_custom_property_data(
    host: &DocumentHost,
    node: u32,
    pseudo: u8,
    data: *const c_void,
    identity: u64,
    is_animation_overlay: bool,
    base: u64,
) {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
    host.engine_memo().held.borrow_mut().follow_pseudo_element(
        node,
        pseudo,
        HeldEnvironment::new(data, is_animation_overlay),
    );
    // SAFETY: Guaranteed by the caller.
    let data = (!data.is_null()).then(|| unsafe { RetainedCustomPropertyData::retain(data) });
    let write = EngineWrite::PseudoElementCustomPropertyData {
        node,
        pseudo,
        data,
        identity,
        sampled_over: is_animation_overlay.then_some(base),
    };
    host.queue_change(ArenaChange::Engine(write));
}

/// Mints an identity into each slot of `nodes`, text identities where `text`, otherwise element ones, and makes them
/// live in the engine.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_mint_style_nodes(host: &DocumentHost, text: bool, nodes: ak::SpanMut<'_, u32>) {
    let nodes = nodes.into_slice();
    host.engine_memo().identities.borrow_mut().mint(text, nodes);
    host.engine_memo()
        .projection
        .borrow_mut()
        .writes()
        .mint(nodes.iter().copied().filter_map(StyleNodeID::from_raw));
    host.queue_change(ArenaChange::Engine(EngineWrite::MintStyleNodes((&*nodes).into())));
}

/// A text node of an arriving subtree, with what it holds.
pub(crate) struct TextArrival {
    node: StyleNodeID,
    data: ak::Utf16String,
    is_ascii_whitespace: bool,
    is_in_user_agent_shadow_tree: bool,
    is_password_input: bool,
}

/// An element of an arriving subtree: its identity, the unique node id the document names it by, and how it stands in
/// the tree.
#[repr(C)]
pub struct FfiArrivingElement {
    pub unique_node_id: u64,
    pub node: u32,
    pub relations: FfiTreeRelations,
}

/// A text node of an arriving subtree: its identity, the characters it holds, as one reference to a live string, and
/// what it is.
#[repr(C)]
pub struct FfiArrivingText {
    pub data: usize,
    pub node: u32,
    pub is_ascii_whitespace: bool,
    pub is_in_user_agent_shadow_tree: bool,
    pub is_password_input: bool,
}

/// Writes the arrival of each element of an arriving subtree into the host's projection rows in one call, as a tree
/// delta from standing in no tree, and the unique node ids the document names them by.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_stage_element_arrivals(host: &DocumentHost, elements: ak::Span<'_, FfiArrivingElement>) {
    let elements = elements.as_slice();
    let detached = FfiTreeRelations {
        parent: 0,
        previous_element_sibling: 0,
        next_element_sibling: 0,
        tree_scope: 0,
        assigned_slot: 0,
    };
    {
        let mut projection = host.engine_memo().projection.borrow_mut();
        let mut writes = projection.writes();
        for element in elements {
            writes.record_ffi_tree_delta(&FfiTreeDelta {
                node: element.node,
                old_connected: false,
                new_connected: true,
                old_relations: detached,
                new_relations: element.relations,
            });
        }
    }
    let ids: Box<[(StyleNodeID, i64)]> = elements
        .iter()
        .filter_map(|element| {
            Some((
                StyleNodeID::from_raw(element.node)?,
                element.unique_node_id.cast_signed(),
            ))
        })
        .collect();
    if !ids.is_empty() {
        host.queue_change(ArenaChange::Engine(EngineWrite::ElementUniqueNodeIds(ids)));
    }
}

/// Writes each text node of an arriving subtree, with what it holds, in one write.
///
/// # Safety
/// `host` must be a live document host, and the caller transfers one reference to each text's live string `data`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_text_arrivals(host: &DocumentHost, texts: ak::Span<'_, FfiArrivingText>) {
    let texts: Box<[TextArrival]> = texts
        .as_slice()
        .iter()
        .filter_map(|text| {
            // SAFETY: Guaranteed by the caller.
            let data = unsafe { ak::Utf16String::from_raw_owned(text.data) };
            Some(TextArrival {
                node: StyleNodeID::from_raw(text.node)?,
                data,
                is_ascii_whitespace: text.is_ascii_whitespace,
                is_in_user_agent_shadow_tree: text.is_in_user_agent_shadow_tree,
                is_password_input: text.is_password_input,
            })
        })
        .collect();
    if !texts.is_empty() {
        host.queue_change(ArenaChange::Engine(EngineWrite::TextArrivals(texts)));
    }
}

/// Writes the tree delta `delta` into the host's projection rows.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_stage_tree_delta(host: &DocumentHost, delta: &FfiTreeDelta) {
    host.engine_memo()
        .projection
        .borrow_mut()
        .writes()
        .record_ffi_tree_delta(delta);
}

/// An element's facts arriving, with how it stands in the tree where it arrives there with them. Elements that took
/// their place in the tree earlier arrive with `has_relations` false.
#[repr(C)]
pub struct FfiElementArrivalRow {
    pub arrival: FfiElementArrival,
    pub unique_node_id: u64,
    pub relations: FfiTreeRelations,
    pub has_relations: bool,
}

/// Writes the arrival of each element of `rows`, with every fact its selectors test, into its projection row in one go,
/// and keeps what it arrives with beside its row for the host's next barrier. `atoms` holds each row's custom states,
/// classes and attributes in turn.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_stage_element_arrival_rows(
    host: &DocumentHost,
    rows: ak::Span<'_, FfiElementArrivalRow>,
    atoms: ak::Span<'_, u32>,
) {
    let rows = rows.as_slice();
    let mut atoms = atoms.as_slice();
    let memo = host.engine_memo();
    {
        let mut projection = memo.projection.borrow_mut();
        let mut writes = projection.writes();
        let mut unpublished = memo.unpublished.borrow_mut();
        for row in rows {
            let arrival = &row.arrival;
            let count = arrival.custom_state_count as usize
                + arrival.class_count as usize
                + 2 * arrival.attribute_count as usize;
            let (row_atoms, rest) = atoms.split_at(count);
            atoms = rest;
            let Some(node) = StyleNodeID::from_raw(arrival.node) else {
                debug_assert!(false, "an element arrival named an invalid style node");
                continue;
            };
            let relations = row.has_relations.then(|| row.relations.decode());
            writes.record_element_arrival(
                node,
                &super::bridge::element_arrival_facts(arrival, row_atoms),
                relations,
            );
            if arrival.associated_pseudo_kind_plus_one != 0 {
                unpublished.backing_elements.push(node);
            }
        }
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
}

/// Whether the host's projection row of `node` holds the attribute `name`.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_row_holds_attribute(host: &DocumentHost, node: u32, name: u32) -> bool {
    let name = StyleAtomID(name);
    StyleNodeID::from_raw(node).is_some_and(|node| {
        host.engine_memo()
            .projection
            .borrow()
            .written_facts(node)
            .is_some_and(|facts| facts.attributes().iter().any(|attribute| attribute.name == name))
    })
}

/// Writes the local feature delta `delta` into the host's projection rows.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_stage_local_feature_delta(host: &DocumentHost, delta: &FfiLocalFeatureDelta) {
    host.engine_memo()
        .projection
        .borrow_mut()
        .writes()
        .record_ffi_local_feature(delta);
}

/// Writes the state delta `delta` into the host's projection rows.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_stage_state_delta(host: &DocumentHost, delta: &FfiStateDelta) {
    host.engine_memo()
        .projection
        .borrow_mut()
        .writes()
        .record_ffi_state(delta);
}

/// Keeps the element declaration delta `delta` for the host's next barrier.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_stage_element_declaration_delta(
    host: &DocumentHost,
    delta: &FfiElementDeclarationDelta,
) {
    host.engine_memo().unpublished.borrow_mut().declarations.push(*delta);
}

/// Whether the host wrote anything of its elements it has not published yet.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_has_staged_input(host: &DocumentHost) -> bool {
    host.engine_memo().has_unpublished_writes()
}

/// Publishes what the host wrote of its elements since its last barrier, which the engine takes in order with the
/// host's other writes.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_submit_staged_input(host: &DocumentHost) {
    host.publish_element_writes();
}

/// Records the parts the element exposes, each with the shadow host at `hosts` it is exposed to.
///
/// # Safety
/// `host` must be a live document host, and `names` and `hosts` must each point at `count` readable values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_parts(
    host: &DocumentHost,
    node: u32,
    names: *const u32,
    hosts: *const u32,
    count: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let (names, hosts) = unsafe { (borrow(names, count), borrow(hosts, count)) };
    if let Some((node, pairs)) = element_parts(node, names, hosts) {
        host.engine_memo()
            .projection
            .borrow_mut()
            .writes()
            .set_element_parts(node, &pairs);
    }
}

/// Records the characters the text node holds.
///
/// # Safety
/// `host` must be a live document host, and the caller transfers one reference to the live string `data`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_text_data(host: &DocumentHost, node: u32, data: usize) {
    // SAFETY: Guaranteed by the caller.
    let data = unsafe { ak::Utf16String::from_raw_owned(data) };
    host.queue_change(ArenaChange::Engine(EngineWrite::TextData { node, data }));
}

/// The document host `host` names.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, which outlives the borrow.
pub(crate) unsafe fn document_host<'a>(host: *const DocumentHost) -> &'a DocumentHost {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }
}

/// Runs `call` on the style engine of `host`'s document in `read`, and answers what it answers. The engine is borrowed
/// for the call alone, so a host callback that reaches the engine again runs after it.
pub(crate) fn with_engine<R>(read: &BegunRead, host: &DocumentHost, call: impl FnOnce(&mut StyleEngine) -> R) -> R {
    with_engine_and_arena(read, host, |engine, _| call(engine))
}

/// Like [`with_engine`], with the document's layout arena beside the engine, as of the same writes.
pub(crate) fn with_engine_and_arena<R>(
    read: &BegunRead,
    host: &DocumentHost,
    call: impl FnOnce(&mut StyleEngine, &crate::layout::LayoutNodeArena) -> R,
) -> R {
    host.ask(read, |state| {
        let (engine, arena) = state.engine_and_arena();
        call(engine, arena)
    })
}

/// The custom-property environment an element holds, or null.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_element_custom_property_data(host: &DocumentHost, node: u32) -> *const c_void {
    held_environment(host, node, None).data()
}

/// The custom-property environment one of an element's synthetic pseudo-elements holds, or null.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_pseudo_element_custom_property_data(
    host: &DocumentHost,
    node: u32,
    pseudo: u8,
) -> *const c_void {
    held_environment(host, node, Some(pseudo)).data()
}

/// Whether the custom-property environment an element holds, or one of its synthetic pseudo-elements of the kind
/// `pseudo` where it is not `u8::MAX`, is the overlay of what the holder's own animations sample.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_custom_property_data_is_animation_overlay(
    host: &DocumentHost,
    node: u32,
    pseudo: u32,
) -> bool {
    let pseudo = pseudo as u8;
    held_environment(host, node, (pseudo != u8::MAX).then_some(pseudo)).is_animation_overlay()
}

fn held_environment(host: &DocumentHost, node: u32, pseudo: Option<u8>) -> HeldEnvironment {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return HeldEnvironment::NONE;
    };
    let held = host.engine_memo().held.borrow();
    match pseudo {
        None => held.element(node),
        Some(pseudo) => held.pseudo_element(node, pseudo),
    }
}

/// The nodes whose style depends on the axes of the viewport `moved_axes` names, as `StyleEnvironment` bits, handed
/// to `append` one by one.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `append` must take each node synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_viewport_dependent_nodes(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    moved_axes: u8,
    context: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, u32),
) {
    let moved = super::input_types::StyleEnvironment::from_bits(moved_axes);
    let nodes = with_engine(read, host, |engine| {
        let mut nodes = engine.computed_group_sets.viewport_dependent_nodes();
        if let Some(live) = engine.retained.live_style.as_ref() {
            nodes.retain(|&node| StyleNodeID::from_raw(node).is_none_or(|node| live.reads_viewport_axes(node, moved)));
        }
        nodes
    });
    for node in nodes {
        // SAFETY: Guaranteed by the caller.
        unsafe { append(context, node) };
    }
}

/// Has the engine keep the random base values of the keys that name the element whose style node `node` was, as the
/// element loses it, in a slot the element holds, or answers null where no element has any.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_park_element_random_base_values(host: &DocumentHost, node: u32) -> *const c_void {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return std::ptr::null();
    };
    if !host.element_random_base_values_may_exist() {
        return std::ptr::null();
    }
    let slot = ParkedBaseValues::default();
    queue(
        host,
        EngineWrite::ParkRandomBaseValues {
            node,
            slot: Arc::clone(&slot),
        },
    );
    Arc::into_raw(slot).cast()
}

/// Gives the element's new style node `node` the random base values it kept in `slot`, which the call takes.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `slot` a slot
/// [`style_engine_park_element_random_base_values`] answered.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_unpark_element_random_base_values(
    host: &DocumentHost,
    node: u32,
    slot: *const c_void,
) {
    // SAFETY: Guaranteed by the caller.
    let slot = unsafe { ParkedBaseValues::from_raw(slot.cast()) };
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
    host.queue_change(ArenaChange::Engine(EngineWrite::UnparkRandomBaseValues { node, slot }));
}

/// Lets go of a slot of random base values the element that held it no longer needs.
///
/// # Safety
///
/// `slot` must be a slot [`style_engine_park_element_random_base_values`] answered.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_release_random_base_values(slot: *const c_void) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { ParkedBaseValues::from_raw(slot.cast()) });
}

/// Answer a read of one element's style the host makes before the next style update.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_answer_record_demand(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    demand: FfiRecordDemand,
) -> FfiRecordDemandAnswer {
    ask_record_demand(host, read, node, RecordDemand::Element(demand))
}

/// Answer a read of one of an element's pseudo-elements the host makes before the next style update. `highlight_name`
/// is the atom of the custom highlight name of a `::highlight()`, and zero for any other pseudo-element.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_answer_pseudo_element_record_demand(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    demand: FfiPseudoElementRecordDemand,
    pseudo_element: FfiDemandedPseudoElement,
    highlight_name: u32,
) -> FfiRecordDemandAnswer {
    ask_record_demand(
        host,
        read,
        node,
        RecordDemand::PseudoElement(demand, pseudo_element, StyleAtomID(highlight_name)),
    )
}

fn ask_record_demand(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    demand: RecordDemand,
) -> FfiRecordDemandAnswer {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return FfiRecordDemandAnswer::default();
    };
    with_engine(read, host, |engine| {
        super::bridge::answer_record_demand(engine, node, demand)
    })
}

/// What the host knows of its document's style engine, which answers the host's reads without asking the render owner.
pub(crate) struct EngineMemo {
    /// The record of each style record identity the host asked about: a published record never changes while it is
    /// live, and no other record ever takes its identity, as each identity carries the generation of its slot.
    /// The records the host holds for the records it asked for or a transaction carried, by record, which the engine
    /// holds while it holds the record.
    pub(crate) views: Memo<*const c_void>,
    /// The custom-property environments the engine holds for elements.
    pub(crate) held: std::cell::RefCell<HeldEnvironments>,
    /// The element style inputs the engine defers.
    pub(crate) deferred: std::cell::RefCell<DeferredInputs>,
    /// The versions of the animation effects the host described to the engine.
    pub(crate) described: std::cell::RefCell<super::effect_descriptions::DescribedVersions>,
    /// The transition baselines the host recorded in the engine.
    pub(crate) baselines: std::cell::RefCell<super::TransitionBaselines>,
    /// The atoms the host interned for the engine.
    pub(crate) atoms: std::cell::RefCell<super::host_atoms::HostAtoms>,
    /// The identity space of the document's style nodes, which the host mints from.
    identities: std::cell::RefCell<super::identities::StyleNodeIdAllocator>,
    /// The host's writer of the projection rows of its document's elements, which nothing else writes.
    pub(in crate::css::style) projection: std::cell::RefCell<super::pipeline::projection::ProjectionWriter>,
    /// What the host wrote beside the rows since its last barrier.
    unpublished: std::cell::RefCell<UnpublishedInputs>,
    /// While a batch's reactions are applied, a host's own style application may rewrite the declarations of an
    /// element in its shadow tree, after the engine computed that element's record from the ones it had. These name
    /// the elements whose declarations changed that way, while `applying_reactions` counts the batches being applied.
    declarations_changed_during_apply: std::cell::RefCell<super::HashSet<StyleNodeID>>,
    applying_reactions: std::cell::Cell<u32>,
    /// Whether the transaction the host takes is one a style update applies, whose reactions close over the elements
    /// they inherit through as the engine computes them.
    takes_transaction_to_apply: std::cell::Cell<bool>,
    /// What `LIBWEB_STYLE_SELFCHECK` asked for as the host started.
    pub(super) style_self_check: super::pipeline::SelfCheckSettings,
    /// Whether the self-check of a style update that flew is yet to run: it is the next update's to run.
    pub(super) flown_update_awaits_self_check: std::cell::Cell<bool>,
}

impl EngineMemo {
    /// Forgets the atoms the engine reclaimed, whose numbers may name other names from now on: those the host interned,
    /// and what the catalogs its projection rows are spelled with spell of them.
    pub(crate) fn forget_reclaimed_atoms(&self, reclaimed: &[super::atoms::ReclaimedStyleAtom]) {
        self.atoms.borrow_mut().forget(reclaimed);
        if !reclaimed.is_empty() {
            let reclaimed: Vec<StyleAtomID> = reclaimed.iter().map(|reclaimed| reclaimed.atom).collect();
            self.projection.borrow_mut().forget_atoms(&reclaimed);
        }
    }

    /// Counts what the memo holds toward the host's own owner, but for its projection writer, whose rows count toward
    /// the projection.
    pub(crate) fn report_memory(&self, report: &mut super::memory_report::MemoryReport) {
        use crate::heap_size::{set_bytes, vec_bytes};
        let unpublished = self.unpublished.borrow();
        let bytes = size_of::<Memo<*const c_void>>()
            + self.held.borrow().heap_size()
            + self.deferred.borrow().heap_size()
            + self.described.borrow().heap_size()
            + self.baselines.borrow().heap_size()
            + self.atoms.borrow().heap_size()
            + vec_bytes(&unpublished.backing_elements)
            + vec_bytes(&unpublished.declarations)
            + set_bytes(&self.declarations_changed_during_apply.borrow());
        report.add(super::memory_report::MemoryOwner::HostMemo, bytes);
    }

    /// Note that `node` stands for an element-backed pseudo-element of its shadow host, which the render owner finds it
    /// by from the next barrier on.
    pub(crate) fn note_backing_element(&self, node: StyleNodeID) {
        self.unpublished.borrow_mut().backing_elements.push(node);
    }

    /// Whether the host wrote anything of its elements since its last barrier.
    pub(crate) fn has_unpublished_writes(&self) -> bool {
        let unpublished = self.unpublished.borrow();
        self.projection.borrow().has_unpublished_writes()
            || !unpublished.backing_elements.is_empty()
            || !unpublished.declarations.is_empty()
    }

    /// Takes what the host wrote of its elements since its last barrier, where it wrote anything, as published at
    /// this one.
    pub(crate) fn take_publication(&self) -> Option<HostPublication> {
        if !self.has_unpublished_writes() {
            return None;
        }
        let unpublished = std::mem::take(&mut *self.unpublished.borrow_mut());
        Some(HostPublication {
            projection: self.projection.borrow_mut().publish(),
            backing_elements: unpublished.backing_elements.into(),
            declarations: unpublished.declarations.into(),
        })
    }

    /// Forgets what the host follows of the style nodes the end of a transaction released, and mints their identities
    /// again first.
    pub(crate) fn take_back_released_style_nodes(&self, released: &[u32]) {
        if released.is_empty() {
            return;
        }
        self.held.borrow_mut().forget(released);
        self.described.borrow_mut().forget(released);
        self.baselines.borrow_mut().forget(released);
        self.identities.borrow_mut().release(released);
    }

    pub(crate) fn forget_released_animation_overlay_records(&self, released: &super::ReleasedAnimationOverlayRecords) {
        if released.overflowed() {
            let is_animation_overlay = |key: u64| {
                super::computed::FinalStyleRecordID::from_raw(key).is_some_and(|record| record.is_animation_overlay())
            };
            self.views.forget_where(is_animation_overlay);
            return;
        }
        for record in released.records() {
            self.views.forget(record);
        }
    }

    /// Follows `change`, a write the host queued for the engine, as the host makes it.
    pub(crate) fn follow(&self, change: &super::boundary::StyleChange) {
        self.baselines.borrow_mut().follow(change);
    }

    /// Follows `queued`, a write the host queued for the engine, as it joins the writes the engine applies next.
    pub(crate) fn follow_queued(&self, queued: crate::render_state::QueuedStyleChange<'_>) {
        self.deferred.borrow_mut().follow(queued);
    }

    /// Notes the declarations of `node` changed, where a batch's reactions are being applied.
    fn note_declarations_changed(&self, node: StyleNodeID) {
        if self.applying_reactions.get() > 0 {
            self.declarations_changed_during_apply.borrow_mut().insert(node);
        }
    }

    /// Marks the transactions the host takes until the answer is dropped as ones a style update applies.
    pub(crate) fn take_transactions_to_apply(&self) -> impl Drop + '_ {
        struct Taking<'a>(&'a EngineMemo);
        impl Drop for Taking<'_> {
            fn drop(&mut self) {
                self.0.takes_transaction_to_apply.set(false);
            }
        }
        self.takes_transaction_to_apply.set(true);
        Taking(self)
    }
}

impl Default for EngineMemo {
    fn default() -> Self {
        Self {
            views: Memo::new(std::ptr::null()),
            held: Default::default(),
            deferred: Default::default(),
            described: Default::default(),
            baselines: Default::default(),
            atoms: Default::default(),
            identities: Default::default(),
            projection: Default::default(),
            unpublished: Default::default(),
            declarations_changed_during_apply: Default::default(),
            applying_reactions: Default::default(),
            takes_transaction_to_apply: Default::default(),
            style_self_check: super::pipeline::SelfCheckSettings::from_environment(),
            flown_update_awaits_self_check: Default::default(),
        }
    }
}

/// The custom-property environment each element and each of its synthetic pseudo-elements holds in the engine, as the
/// host's object for it, which the engine keeps alive while it holds it. The host follows every write of them, so it
/// never asks: it makes them itself, but for a move of the environment some elements inherit, which answers the host
/// what it moved, and the retirement of a node, whose identity the host takes back from the transaction that retired
/// it.
#[derive(Default)]
pub(crate) struct HeldEnvironments {
    elements: HashMap<StyleNodeID, HeldEnvironment>,
    pseudo_elements: HashMap<StyleNodeID, Vec<(u8, HeldEnvironment)>>,
}

/// The custom-property environment an element or one of its synthetic pseudo-elements holds, as the address of the
/// host's object for it, null for none, with whether it is the overlay of what the holder's own animations sample.
/// That is the address's lowest bit, which the address of an object never sets.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct HeldEnvironment(usize);

impl HeldEnvironment {
    const IS_ANIMATION_OVERLAY: usize = 1;
    pub(crate) const NONE: Self = Self(0);

    pub(crate) fn new(data: *const c_void, is_animation_overlay: bool) -> Self {
        let address = data.expose_provenance();
        debug_assert_eq!(
            address & Self::IS_ANIMATION_OVERLAY,
            0,
            "an object's address is aligned"
        );
        Self(
            address
                | if is_animation_overlay {
                    Self::IS_ANIMATION_OVERLAY
                } else {
                    0
                },
        )
    }

    pub(crate) fn data(self) -> *const c_void {
        std::ptr::with_exposed_provenance(self.0 & !Self::IS_ANIMATION_OVERLAY)
    }

    pub(crate) fn is_animation_overlay(self) -> bool {
        self.0 & Self::IS_ANIMATION_OVERLAY != 0
    }
}

impl HeldEnvironments {
    fn element(&self, node: StyleNodeID) -> HeldEnvironment {
        self.elements.get(&node).copied().unwrap_or(HeldEnvironment::NONE)
    }

    fn pseudo_element(&self, node: StyleNodeID, pseudo: u8) -> HeldEnvironment {
        self.pseudo_elements
            .get(&node)
            .and_then(|environments| environments.iter().find(|&&(kind, _)| kind == pseudo))
            .map_or(HeldEnvironment::NONE, |&(_, held)| held)
    }

    /// Follows `held`, which the host hands the engine for `node`.
    fn follow_element(&mut self, node: StyleNodeID, held: HeldEnvironment) {
        if held.data().is_null() {
            self.elements.remove(&node);
        } else {
            self.elements.insert(node, held);
        }
    }

    /// Follows `held`, which the host hands the engine for a synthetic pseudo-element of `node`.
    fn follow_pseudo_element(&mut self, node: StyleNodeID, pseudo: u8, held: HeldEnvironment) {
        let environments = self.pseudo_elements.entry(node).or_default();
        environments.retain(|&(kind, _)| kind != pseudo);
        if !held.data().is_null() {
            environments.push((pseudo, held));
        }
        if environments.is_empty() {
            self.pseudo_elements.remove(&node);
        }
    }

    /// Forgets what the nodes a transaction retired held, whose identities it `released`.
    pub(crate) fn forget(&mut self, released: &[u32]) {
        for node in released.iter().filter_map(|&raw| StyleNodeID::from_raw(raw)) {
            self.elements.remove(&node);
            self.pseudo_elements.remove(&node);
        }
    }

    fn heap_size(&self) -> usize {
        use crate::heap_size::{map_bytes, vec_bytes};
        map_bytes(&self.elements)
            + map_bytes(&self.pseudo_elements)
            + self.pseudo_elements.values().map(vec_bytes).sum::<usize>()
    }
}

/// What one element owes of the style inputs the engine defers: its reactions and its inherited style groups.
pub(crate) type DeferredInput = (StyleNodeID, u8, u8);

/// The element style inputs the engine defers, by element, as the host knows them: the engine's own as of the last job
/// that moved them, followed over each write the host queued since that names what it defers. A write that defers
/// inputs for elements the host cannot name (the children of an element whose reaction it applied, a subtree, a
/// container's dependents, a rootless flush) adds to [`Self::unnamed`] instead, which bounds what the engine may owe
/// any element beyond what the host knows until the next job.
#[derive(Default)]
pub(crate) struct DeferredInputs {
    /// Sorted by element.
    inputs: Vec<DeferredInput>,
    /// The reactions and the inherited style groups that the writes queued since the last job may defer for elements
    /// the host cannot name.
    unnamed: (u8, u8),
}

impl DeferredInputs {
    fn heap_size(&self) -> usize {
        crate::heap_size::vec_bytes(&self.inputs)
    }

    const ALL: (u8, u8) = (u8::MAX, u8::MAX);

    fn search(&self, node: StyleNodeID) -> Result<usize, usize> {
        self.inputs.binary_search_by_key(&node, |&(node, ..)| node)
    }

    /// Follows `queued`, in the order the engine applies the writes the host queues: a write held behind the drain of a
    /// style update's reactions reaches the engine after the drain's.
    fn follow(&mut self, queued: crate::render_state::QueuedStyleChange<'_>) {
        use super::boundary::StyleChange;
        use super::input_types::{STYLE_REACTION_PUBLISHED_STYLE, STYLE_REACTION_RECOMPUTE_STYLE};
        let (node, reaction, groups) = match *queued.change() {
            StyleChange::RecordDerivedElementStyleInput {
                node,
                reaction,
                inherited_style_groups,
            } => (node, reaction, inherited_style_groups),
            StyleChange::RecordContainerQueryInput { node } => {
                (node, STYLE_REACTION_PUBLISHED_STYLE | STYLE_REACTION_RECOMPUTE_STYLE, 0)
            }
            StyleChange::ConsumeElementStyleInput { node: Some(node) } => {
                if let Ok(index) = self.search(node) {
                    self.inputs.remove(index);
                }
                return;
            }
            StyleChange::RecordSizeContainerQueryDependents { .. }
            | StyleChange::EvaluateSizeContainersNeedingEvaluationAfterLayout => {
                self.add_unnamed((STYLE_REACTION_PUBLISHED_STYLE | STYLE_REACTION_RECOMPUTE_STYLE, 0));
                return;
            }
            StyleChange::Flush => {
                self.add_unnamed(Self::ALL);
                return;
            }
            _ => return,
        };
        let Some(node) = node else {
            return;
        };
        if reaction == 0 {
            return;
        }
        match self.search(node) {
            Ok(index) => {
                let (_, owed_reaction, owed_groups) = &mut self.inputs[index];
                *owed_reaction |= reaction;
                *owed_groups |= groups;
            }
            Err(index) => self.inputs.insert(index, (node, reaction, groups)),
        }
    }

    /// Whether the engine may defer an input for an element the host cannot name.
    fn may_owe_unnamed(&self) -> bool {
        self.unnamed != (0, 0)
    }

    fn add_unnamed(&mut self, (reaction, groups): (u8, u8)) {
        self.unnamed.0 |= reaction;
        self.unnamed.1 |= groups;
    }

    /// Learns what a job applied every write queued before it leaves the engine deferring, `inputs` where they moved.
    /// A write queued since, by a host callback of the job or beside a frame, is not followed over what the job left.
    pub(crate) fn follow_job(&mut self, inputs: Option<Vec<DeferredInput>>, writes_wait: bool) {
        match (inputs, writes_wait) {
            (Some(inputs), true) => {
                self.inputs = inputs;
                self.unnamed = Self::ALL;
            }
            (Some(inputs), false) => {
                self.inputs = inputs;
                self.unnamed = (0, 0);
            }
            (None, true) => {}
            (None, false) => self.unnamed = (0, 0),
        }
    }
}

/// The record the host holds for `style_record` the host has without asking: one it asked for before, or one the style
/// transaction it drains carried. A published record never changes while it is live.
pub(crate) fn known_host_record(host: &DocumentHost, style_record: u64) -> Option<*const c_void> {
    let memo = &host.engine_memo().views;
    if let Some(style) = memo.get(style_record) {
        return Some(style);
    }
    let style = host.transaction_record(style_record)?;
    memo.set(style_record, style);
    Some(style)
}

/// The before-change style the current style stabilization epoch decides the transitions of `node`'s element, or of
/// one of its pseudo-elements, against, or null before a pass has recorded one: a record the host takes a reference to
/// before it lets go of the baselines. The host knows it without asking: it follows every baseline it records in the
/// engine.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_transition_baseline(host: &DocumentHost, node: u32, pseudo_kind: u32) -> *const c_void {
    let pseudo_kind = pseudo_kind as u8;
    StyleNodeID::from_raw(node)
        .and_then(|node| {
            host.engine_memo()
                .baselines
                .borrow()
                .get(node, pseudo_kind)
                .map(std::sync::Arc::as_ptr)
        })
        .map_or(std::ptr::null(), |record| record.cast())
}

/// Whether the engine has a style update pending, which the host knows without asking where it wrote nothing
/// since its last job.
/// Hands the host every custom-property environment the engine holds for an element, with the element's style node,
/// for the host to name again what each hands down. The callbacks run outside the engine, so each may queue a write
/// to it or ask it.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_for_each_element_custom_property_environment(
    host: &DocumentHost,
    read: &BegunRead,
    context: *mut c_void,
    callback: extern "C" fn(*mut c_void, u32, *const c_void),
) {
    // The engine keeps each environment alive while an element holds it, and nothing moves one on before the
    // callbacks have run.
    let held: Vec<(u32, *const c_void)> = with_engine(read, host, |engine| {
        let retained = &engine.retained;
        let lying_over = &retained.element_custom_property_data;
        let mut named = super::fast_hash::FastSet::default();
        lying_over
            .iter()
            .map(|(node, held)| (node.raw(), held.data.data()))
            .chain(
                retained
                    .computed_group_sets
                    .installed_custom_property_environments()
                    .filter(|&(node, environment)| environment != 0 && !lying_over.contains_key(&node))
                    .filter(|&(_, environment)| named.insert(environment))
                    .map(|(node, environment)| (node.raw(), std::ptr::with_exposed_provenance(environment as usize))),
            )
            .collect()
    });
    for (node, data) in held {
        callback(context, node, data);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn style_engine_has_pending_transaction(host: &DocumentHost, read: &BegunRead) -> bool {
    match host.known_engine_facts() {
        Some(facts) => facts.has_pending_style_update,
        None => with_engine(read, host, |engine| engine.has_pending_transaction()),
    }
}

/// What an element's arrival has to carry to reach the anchor of a `:has()` argument, and where that is: the raw
/// identities of the tags, IDs, classes and attribute names, and the states, any of which reaches one. The spans hold
/// until the host next asks.
#[repr(C)]
pub struct FfiRelationalArrivalKeys {
    pub version: u64,
    pub reached_by_any_arrival: bool,
    pub names: *const usize,
    pub name_count: usize,
    pub states: *const u8,
    pub state_count: usize,
}

#[unsafe(no_mangle)]
pub extern "C" fn style_engine_relational_arrival_keys(
    host: &DocumentHost,
    read: &BegunRead,
) -> FfiRelationalArrivalKeys {
    let (version, keys) = host.known_relational_arrival_keys().unwrap_or_else(|| {
        with_engine(read, host, |engine| {
            engine.resolve_relational_arrival_keys();
            (
                engine.relational_arrival_keys_version(),
                Arc::clone(engine.relational_arrival_keys()),
            )
        })
    });
    let keys = host.lend_relational_arrival_keys(keys);
    FfiRelationalArrivalKeys {
        version,
        reached_by_any_arrival: keys.reached_by_any_arrival,
        names: keys.names.as_ptr(),
        name_count: keys.names.len(),
        states: keys.states.as_ptr(),
        state_count: keys.states.len(),
    }
}

/// Whether the engine defers any element style input, which the host knows without asking where it wrote nothing since
/// its last job.
/// Whether the host knows what the engine answers of itself without asking: no write it queued since the last job may
/// have moved it.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_knows_engine_facts(host: &DocumentHost) -> bool {
    host.known_engine_facts().is_some()
}

#[unsafe(no_mangle)]
pub extern "C" fn style_engine_has_deferred_element_style_inputs(host: &DocumentHost, read: &BegunRead) -> bool {
    match host.known_engine_facts() {
        Some(facts) => facts.has_deferred_element_style_inputs,
        None => with_engine(read, host, |engine| engine.has_deferred_element_style_inputs()),
    }
}

/// Whether `style_node` owes a style input the engine defers, which the host knows without asking where no input it
/// cannot name may be owed.
pub(crate) fn has_deferred_element_style_input(host: &DocumentHost, read: &BegunRead, style_node: StyleNodeID) -> bool {
    if host.knows_engine_between_jobs() {
        let deferred = host.engine_memo().deferred.borrow();
        if !deferred.may_owe_unnamed() {
            return deferred.search(style_node).is_ok();
        }
    }
    with_engine(read, host, |engine| engine.has_deferred_element_style_input(style_node))
}

/// Whether a size container waits for layout to be evaluated, which the host knows without asking where it wrote
/// nothing since its last job.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_has_size_containers_needing_evaluation_after_layout(
    host: &DocumentHost,
    read: &BegunRead,
) -> bool {
    match host.known_engine_facts() {
        Some(facts) => facts.has_size_containers_needing_evaluation_after_layout,
        None => with_engine(read, host, |engine| {
            engine.has_size_containers_needing_evaluation_after_layout()
        }),
    }
}

/// Notes that the host rewrote the declarations of `node`, which a batch of reactions being applied reads.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_note_element_declarations_changed(host: &DocumentHost, node: u32) {
    if let Some(node) = StyleNodeID::from_raw(node) {
        host.engine_memo().note_declarations_changed(node);
    }
}

type MemoSlot<T> = std::cell::Cell<(u64, T)>;

/// A direct-mapped memo of one answer per key, which keeps the answers of the keys the host asked about last. It doubles
/// with the answers it is told, up to its full size, so that a document of a few elements holds a small one.
pub(crate) struct Memo<T: Copy> {
    slots: std::cell::RefCell<Box<[MemoSlot<T>]>>,
    answers_told: std::cell::Cell<usize>,
    empty: T,
}

impl<T: Copy> Memo<T> {
    const FIRST_SLOTS: usize = 8;
    const MAX_SLOTS: usize = 256;
    /// No record has this identity: a base record's leaves the top bit clear, and an overlay's generation is below it.
    const NO_KEY: u64 = u64::MAX;

    fn new(empty: T) -> Self {
        Self {
            slots: std::cell::RefCell::new(Self::empty_slots(Self::FIRST_SLOTS, empty)),
            answers_told: std::cell::Cell::new(0),
            empty,
        }
    }

    fn empty_slots(count: usize, empty: T) -> Box<[MemoSlot<T>]> {
        (0..count)
            .map(|_| std::cell::Cell::new((Self::NO_KEY, empty)))
            .collect()
    }

    fn slot(slots: &[MemoSlot<T>], key: u64) -> &MemoSlot<T> {
        &slots[(key ^ (key >> 32)) as usize & (slots.len() - 1)]
    }

    pub(crate) fn get(&self, key: u64) -> Option<T> {
        let (held, answer) = Self::slot(&self.slots.borrow(), key).get();
        (held == key).then_some(answer)
    }

    pub(crate) fn set(&self, key: u64, answer: T) {
        let told = self.answers_told.get() + 1;
        self.answers_told.set(told);
        let mut slots = self.slots.borrow_mut();
        if told > slots.len() && slots.len() < Self::MAX_SLOTS {
            let grown = Self::empty_slots(slots.len() * 2, self.empty);
            for slot in slots.iter() {
                let (held, kept) = slot.get();
                if held != Self::NO_KEY {
                    Self::slot(&grown, held).set((held, kept));
                }
            }
            *slots = grown;
        }
        Self::slot(&slots, key).set((key, answer));
    }

    fn forget(&self, key: u64) {
        let slots = self.slots.borrow();
        let slot = Self::slot(&slots, key);
        let (held, answer) = slot.get();
        if held == key {
            slot.set((Self::NO_KEY, answer));
        }
    }

    fn forget_where(&self, forgets: impl Fn(u64) -> bool) {
        for slot in self.slots.borrow().iter() {
            let (held, answer) = slot.get();
            if held != Self::NO_KEY && forgets(held) {
                slot.set((Self::NO_KEY, answer));
            }
        }
    }
}

/// The engine's id of the native rule `identity` names, plus one, or 0 for none.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_native_rule_id(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    identity: u64,
) -> u32 {
    with_engine(read, host, |engine| {
        engine.native_rule_id(identity).map_or(0, |id| id.0 + 1)
    })
}

/// The name of the style memory owner at `index`, with its length written out, or null past the last one.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_memory_owner_name(index: usize, out_name_length: &mut usize) -> *const u8 {
    let Some(owner) = super::memory_report::MemoryOwner::ALL.get(index) else {
        return std::ptr::null();
    };
    let name = owner.name();
    *out_name_length = name.len();
    name.as_ptr()
}

/// What the document's style engine holds on the heap, by owner, as a job on the render owner: the bytes of each owner
/// in the order `style_engine_memory_owner_name` names them, with the elements the document holds and those of them
/// holding a style record.
///
/// # Safety
///
/// `out_bytes` must point at room for `capacity` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_memory_report(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    out_bytes: *mut u64,
    capacity: usize,
    out_elements: &mut u64,
    out_styled_elements: &mut u64,
) -> usize {
    use super::memory_report::MemoryOwner;
    let mut report = with_engine_and_arena(read, host, |engine, arena| engine.memory_report(arena));
    // NB: The host's rows share every chunk they were not written in since the barrier with the
    //     published projection the engine counted.
    host.engine_memo().projection.borrow().report_memory(&mut report);
    host.engine_memo().report_memory(&mut report);
    // SAFETY: Guaranteed by the caller.
    let out = unsafe { ak::slice_from_raw_parts_mut(out_bytes, capacity) };
    for (slot, owner) in out.iter_mut().zip(MemoryOwner::ALL) {
        *slot = report.bytes(*owner) as u64;
    }
    *out_elements = report.elements as u64;
    *out_styled_elements = report.styled_elements as u64;
    MemoryOwner::COUNT
}

/// A record of the allocations reports have counted, for [`style_engine_memory_beyond`].
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_memory_sizer_create() -> *mut c_void {
    Box::into_raw(Box::<crate::heap_size::HeapSizer>::default()).cast()
}

/// # Safety
///
/// `sizer` must come from [`style_engine_memory_sizer_create`], and is not used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_memory_sizer_destroy(sizer: *mut c_void) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { Box::from_raw(sizer.cast::<crate::heap_size::HeapSizer>()) });
}

/// What the document's style engine holds on the heap that the reports `sizer` has been through have not counted.
///
/// # Safety
///
/// `sizer` must come from [`style_engine_memory_sizer_create`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_memory_beyond(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    sizer: *mut c_void,
) -> u64 {
    use super::memory_report::{MemoryOwner, MemoryReport};
    // SAFETY: Guaranteed by the caller.
    let sizer = unsafe { &mut *sizer.cast::<crate::heap_size::HeapSizer>() };
    let report = MemoryReport::past(std::mem::take(sizer));
    let mut report = with_engine_and_arena(read, host, |engine, arena| engine.memory_report_into(report, arena));
    host.engine_memo().projection.borrow().report_memory(&mut report);
    host.engine_memo().report_memory(&mut report);
    *sizer = std::mem::take(&mut report.sizer);
    MemoryOwner::ALL.iter().map(|&owner| report.bytes(owner) as u64).sum()
}

/// The name of the engine's counter at `index`, with its length and value written out, or null past the last one.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_counter(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    index: usize,
    out_value: &mut u64,
    out_name_length: &mut usize,
) -> *const u8 {
    let counter = with_engine(read, host, |engine| {
        let retired = engine.computed_group_sets.retired_animation_overlay_records();
        engine
            .counters
            .set(Counter::RetiredAnimationOverlayRecords, retired as u64);
        engine.counters().iter().nth(index)
    });
    let Some((name, value)) = counter else {
        return std::ptr::null();
    };
    *out_value = value;
    *out_name_length = name.len();
    name.as_ptr()
}

/// Decides what the transition step of `input`'s target does to its transitions, as the target's style changes from the
/// record `before` to the record `after` it installed, and hands `on_actions` one action per property it decided over.
/// The style update the host drains decided the step beside the row of an element whose step nothing but the
/// engine's records decides; any other the render owner decides, reading the transform reference box of the target's
/// element's box as it does.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, `before` and `after` records the host holds, and
/// `input` must be valid. The values the actions point at live until `on_actions` returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_decide_transitions(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    before: *const c_void,
    after: *const c_void,
    input: &FfiTransitionInput,
    on_actions: unsafe extern "C" fn(*mut c_void, *const FfiTransitionAction, usize),
    context: *mut c_void,
) {
    crate::css::ffi_stats::note_transition_decision();
    // SAFETY: Guaranteed by the caller.
    let existing = unsafe { ak::slice_from_raw_parts(input.existing_transitions, input.existing_transition_count) };
    // SAFETY: As above.
    let hand_over = |actions: &[FfiTransitionAction]| unsafe { on_actions(context, actions.as_ptr(), actions.len()) };
    // SAFETY: As above.
    let (Some(before), Some(after)) = (unsafe { super::bridge::retain_host_record(before) }, unsafe {
        super::bridge::retain_host_record(after)
    }) else {
        debug_assert!(false, "a transition step decides over two records");
        return;
    };
    let decision = TransitionDecision {
        before,
        after,
        element: (input.target_pseudo_kind == u8::MAX)
            .then(|| StyleNodeID::from_raw(input.target_node))
            .flatten(),
    };
    if decision.element.is_some()
        && host.answer_decided_transition_step(input.target_node, &decision, existing, hand_over)
    {
        return;
    }
    let element_box = crate::layout::node_data::NodeSlotId {
        index: input.element_box_slot,
    };
    let (mut actions, custom_properties) = with_engine_and_arena(read, host, |engine, arena| {
        let reference_box =
            crate::painting::ffi::committed_transform_reference_box(&arena.paintable_rows(), element_box);
        (
            decision.decide(engine, reference_box, existing),
            decision.decide_custom_properties(engine, existing),
        )
    });
    actions.extend_from_slice(&custom_properties.actions);
    hand_over(&actions);
}

#[cfg(test)]
mod hover_replay_tests {
    use super::*;
    use crate::css::style::bridge::{FfiTreeRelations, element_adjustment_fact};
    use crate::css::style::input_types::TreeRelations;
    use crate::css::style::tests::{
        assert_staged_hover_facts, discard_transaction, staged_hover_pending_shadow_fixture,
        staged_hover_shadow_fixture,
    };

    fn encoded(row: TreeRelations) -> FfiTreeRelations {
        FfiTreeRelations {
            parent: row.parent.map_or(0, StyleNodeID::raw),
            previous_element_sibling: row.previous_element_sibling.map_or(0, StyleNodeID::raw),
            next_element_sibling: row.next_element_sibling.map_or(0, StyleNodeID::raw),
            tree_scope: row.tree_scope.0,
            assigned_slot: row.assigned_slot.map_or(0, StyleNodeID::raw),
        }
    }

    fn replay(
        engine: &mut StyleEngine,
        node: StyleNodeID,
        before: TreeRelations,
        after: Option<TreeRelations>,
        hover: Option<Option<StyleNodeID>>,
    ) {
        let delta = FfiTreeDelta {
            node: node.raw(),
            old_connected: true,
            new_connected: after.is_some(),
            old_relations: encoded(before),
            new_relations: encoded(after.unwrap_or(before)),
        };
        engine.apply_transaction_batch(&[delta], (&[], &[]), &[], &[], &[], &[]);
        if let Some(target) = hover {
            EngineWrite::MoveHover(target).apply(engine);
        }
        discard_transaction(engine);
    }

    fn move_hover(engine: &mut StyleEngine, target: Option<StyleNodeID>) {
        EngineWrite::MoveHover(target).apply(engine);
        discard_transaction(engine);
    }

    #[test]
    fn staged_hover_transaction_rejects_a_descendant_of_an_excluded_ancestor() {
        let (mut engine, nodes) = staged_hover_shadow_fixture();
        let before = engine.settled_tree_relations(nodes[9]);
        let mut after = before;
        after.parent = Some(nodes[6]);
        after.tree_scope = TreeScopeID::DOCUMENT;
        replay(&mut engine, nodes[9], before, Some(after), Some(Some(nodes[9])));
        assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 6, 9]);
        let before = engine.settled_tree_relations(nodes[6]);
        let mut after = before;
        after.assigned_slot = None;
        engine.set_slot_assigned_nodes(nodes[5], &[]);
        replay(&mut engine, nodes[6], before, Some(after), None);
        assert_staged_hover_facts(&engine, &nodes, &[]);
        engine.set_slot_assigned_nodes(nodes[5], &[nodes[6]]);
        replay(&mut engine, nodes[6], after, Some(before), None);
        assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 6, 9]);
    }

    #[test]
    fn staged_hover_transaction_restores_fallback_without_another_hover_move() {
        let (mut engine, nodes) = staged_hover_shadow_fixture();
        let assigned = engine.settled_tree_relations(nodes[6]);
        let mut unassigned = assigned;
        unassigned.assigned_slot = None;
        engine.set_slot_assigned_nodes(nodes[5], &[]);
        replay(&mut engine, nodes[6], assigned, Some(unassigned), Some(Some(nodes[9])));
        assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 9]);
        engine.set_slot_assigned_nodes(nodes[5], &[nodes[6]]);
        replay(&mut engine, nodes[6], unassigned, Some(assigned), None);
        assert_staged_hover_facts(&engine, &nodes, &[]);
        let suppressed_identity = engine.hover_target();
        engine.set_slot_assigned_nodes(nodes[5], &[]);
        replay(&mut engine, nodes[6], assigned, Some(unassigned), None);
        assert_eq!(
            (suppressed_identity, engine.hover_target()),
            (Some(nodes[9]), Some(nodes[9]))
        );
        assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 9]);
    }

    #[test]
    fn staged_hover_transaction_stops_validation_at_the_top_layer() {
        let (mut engine, nodes) = staged_hover_shadow_fixture();
        move_hover(&mut engine, Some(nodes[6]));
        let mut root = [0];
        engine.allocate_style_nodes(&mut root);
        let root = StyleNodeID::from_raw(root[0]).unwrap();
        engine.record_tree_delta(root, None, Some(TreeRelations::detached(TreeScopeID(2))));
        engine.set_shadow_root(nodes[1], root);
        engine.set_element_adjustment_facts(nodes[4], element_adjustment_fact::RENDERED_IN_TOP_LAYER);
        let row = engine.settled_tree_relations(nodes[2]);
        replay(&mut engine, nodes[2], row, Some(row), None);
        assert_staged_hover_facts(&engine, &nodes, &[4, 5, 6]);
        engine.set_element_adjustment_facts(nodes[4], 0);
        assert_staged_hover_facts(&engine, &nodes, &[]);
        engine.set_element_adjustment_facts(nodes[4], element_adjustment_fact::RENDERED_IN_TOP_LAYER);
        assert_staged_hover_facts(&engine, &nodes, &[4, 5, 6]);
        move_hover(&mut engine, None);
        engine.set_element_adjustment_facts(nodes[4], 0);
        assert_eq!(engine.hover_target(), None);
        assert_staged_hover_facts(&engine, &nodes, &[]);
    }

    #[test]
    fn staged_hover_transaction_rejects_a_parentless_element_component() {
        let (mut engine, nodes) = staged_hover_shadow_fixture();
        move_hover(&mut engine, Some(nodes[6]));
        let before = engine.settled_tree_relations(nodes[2]);
        let mut after = before;
        after.parent = None;
        replay(&mut engine, nodes[2], before, Some(after), None);
        assert_staged_hover_facts(&engine, &nodes, &[]);
        replay(&mut engine, nodes[2], after, Some(before), None);
        assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 6]);
    }

    #[test]
    fn staged_hover_pending_document_element_arrival_accepts_the_chain() {
        let (mut engine, nodes) = staged_hover_pending_shadow_fixture();
        assert_eq!(engine.projection.parent(nodes[2]), None);
        EngineWrite::MoveHover(Some(nodes[6])).apply(&mut engine);
        assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 6]);
        discard_transaction(&mut engine);
        assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 6]);
    }

    #[test]
    fn staged_hover_pending_document_element_departure_rejects_the_chain() {
        let (mut engine, nodes) = staged_hover_shadow_fixture();
        move_hover(&mut engine, Some(nodes[6]));
        assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 6]);
        let before = engine.settled_tree_relations(nodes[1]);
        let delta = FfiTreeDelta {
            node: nodes[1].raw(),
            old_connected: true,
            new_connected: false,
            old_relations: encoded(before),
            new_relations: encoded(before),
        };
        engine.apply_transaction_batch(&[delta], (&[], &[]), &[], &[], &[], &[]);
        assert!(engine.retained.projection.written_relations(nodes[1]).is_none());
        assert_staged_hover_facts(&engine, &nodes, &[]);
        discard_transaction(&mut engine);
    }

    #[test]
    fn staged_hover_slot_list_replays_text_only_suppression_without_tree_input() {
        let (mut engine, nodes) = staged_hover_shadow_fixture();
        let before = engine.settled_tree_relations(nodes[6]);
        let mut after = before;
        after.assigned_slot = None;
        engine.set_slot_assigned_nodes(nodes[5], &[]);
        replay(&mut engine, nodes[6], before, Some(after), Some(Some(nodes[9])));
        assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 9]);
        let mut text = [0];
        engine.allocate_text_style_nodes(&mut text);
        let text = StyleNodeID::from_raw(text[0]).unwrap();
        engine.set_slot_assigned_nodes(nodes[5], &[text]);
        EngineWrite::SlotAssignedNodes.apply(&mut engine);
        assert_staged_hover_facts(&engine, &nodes, &[]);
        assert_eq!(engine.hover_target(), Some(nodes[9]));
        engine.set_slot_assigned_nodes(nodes[5], &[]);
        EngineWrite::SlotAssignedNodes.apply(&mut engine);
        assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 9]);
    }

    #[test]
    fn staged_hover_transaction_retirement_clears_the_requested_identity() {
        let (mut engine, nodes) = staged_hover_shadow_fixture();
        move_hover(&mut engine, Some(nodes[6]));
        let before = engine.settled_tree_relations(nodes[6]);
        engine.set_slot_assigned_nodes(nodes[5], &[]);
        replay(&mut engine, nodes[6], before, None, None);
        assert_eq!(engine.hover_target(), None);
        assert_staged_hover_facts(&engine, &nodes, &[]);
    }
}
