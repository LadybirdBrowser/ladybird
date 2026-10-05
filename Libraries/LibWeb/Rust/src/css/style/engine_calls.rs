/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The writes and reads the host makes of a document's style engine that the boundary generator does not write for
//! it. A write is a change the document's render state applies to the engine in the order the host made them; a read
//! is a question the host waits for the answer to. Each entry takes the document host, never the engine, and owns
//! what it hands over: borrowed arrays are copied, and the host's objects are referenced, as the entry queues the
//! change.

use super::atoms::{AtomKey, AtomLease};
use super::bridge::{
    FfiDemandedPseudoElement, FfiElementArrival, FfiElementDeclarationDelta, FfiElementStyleInput,
    FfiLocalFeatureDelta, FfiPseudoElementRecordDemand, FfiRecordDemand, FfiRecordDemandAnswer, FfiStateDelta,
    FfiStyleInputTransaction, FfiTreeDelta, borrow, write_recording_element_style_inputs, write_recording_state_deltas,
    write_recording_tree_deltas,
};
use super::font_resolution::{FontResolverHost, PublishedFontFaces};
use super::inputs::RetainedCustomPropertyData;
use super::instrumentation::Counter;
use super::publication::RecordDemand;
use super::random_bases::{NamedBaseValue, ParkedBaseValues};
use super::record_replay::EventKind;
use super::tree::{StyleNodeID, TreeScopeID};
use super::{HashMap, StyleAtomID};
use super::{StyleEngine, StyleEngineHandle};
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
        declares: bool,
    },
    /// The custom-property environment one of an element's synthetic pseudo-elements holds, or none.
    PseudoElementCustomPropertyData {
        node: StyleNodeID,
        pseudo: u8,
        data: Option<RetainedCustomPropertyData>,
        identity: u64,
    },
    /// The host minted these identities, which the engine makes live ahead of anything recorded about them.
    MintStyleNodes(Box<[u32]>),
    /// The host's input transaction: the tree, feature, state and declaration deltas it recorded.
    ApplyTransaction(Box<InputTransaction>),
    /// The parts an element exposes, each with the shadow host it is exposed to.
    ElementParts { node: StyleNodeID, pairs: ElementParts },
    /// The characters a text node holds.
    TextData { node: u32, data: ak::Utf16String },
    /// The language an element resolves to, and the tag a `:lang()` range compares against.
    ElementLanguage { node: u32, language: u32, text: Box<[u16]> },
    /// The presentational hints of one kind an element declares, expanded to the longhands they decide.
    PresentationalHints {
        node: StyleNodeID,
        kind: super::bridge::FfiElementDeclarationKind,
        properties: Box<[crate::css::declaration_block::DeclaredProperty]>,
    },
    /// A benchmark marker the page set, which a recording of the engine's calls keeps in their order.
    #[cfg(feature = "style-recording")]
    BenchmarkMarker(Box<[u16]>),
    /// An element's inline declaration block, or none.
    InlineStyle {
        node: StyleNodeID,
        data: Option<Arc<crate::css::declaration_block::DeclarationBlockData>>,
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
        store: Option<super::custom_property_environments::RetainedCustomPropertyStore>,
        inheritable: u64,
        inheritable_store: Option<super::custom_property_environments::RetainedCustomPropertyStore>,
    },
    /// The host folded the style input `node` owes into the reaction it applies to it, as it knew it would fold
    /// (see [`DeferredInputs::absorb`]), which `absorbed` answered: the engine folds it the same.
    AbsorbElementStyleInput {
        node: StyleNodeID,
        reaction: u8,
        inherited_style_groups: u8,
        absorbs_any: bool,
        absorbed: u32,
    },
    /// What a custom property's name atom spells, and the fly string it is.
    NoteCustomPropertyName {
        name: StyleAtomID,
        string: ak::Utf16FlyString,
        text: Box<[u16]>,
    },
    /// The `@keyframes` row of one style scope, named by the shadow root's pointer identity as well where it is one.
    AnimationKeyframes {
        tree_scope: TreeScopeID,
        shadow_root_identity: usize,
        row: super::animations::KeyframesRow,
    },
    /// The effects one of an element's animation lists holds, in composite order, described for the engine to sample
    /// them from.
    AnimationEffectDescriptions {
        node: StyleNodeID,
        slot: super::animations::AnimationSlot,
        effects: Box<[super::effect_descriptions::PublishedEffect]>,
    },
}

/// An input transaction the host recorded, owned.
pub(crate) struct InputTransaction {
    tree: Box<[FfiTreeDelta]>,
    arrivals: Box<[FfiElementArrival]>,
    arrival_custom_state_atoms: Box<[u32]>,
    features: Box<[FfiLocalFeatureDelta]>,
    states: Box<[FfiStateDelta]>,
    declarations: Box<[FfiElementDeclarationDelta]>,
    element_style_inputs: Box<[FfiElementStyleInput]>,
}

impl EngineWrite {
    pub(crate) fn apply(self, engine: &mut StyleEngine) {
        match self {
            Self::PublishFontFaces {
                font_faces,
                feature_values_shadow_scopes,
            } => {
                match &mut engine.host.font_resolver {
                    Some(resolver) => resolver.publish(font_faces),
                    None => engine.host.font_resolver = Some(FontResolverHost::new(font_faces)),
                }
                engine
                    .retained
                    .font_resolution
                    .get_or_insert_default()
                    .publish_feature_values_shadow_scopes(feature_values_shadow_scopes);
            }
            Self::ElementCustomPropertyData {
                node,
                data,
                identity,
                sampled_over,
                declares,
            } => engine.set_element_custom_property_data(node, data, identity, sampled_over, declares),
            Self::PseudoElementCustomPropertyData {
                node,
                pseudo,
                data,
                identity,
            } => engine.set_pseudo_element_custom_property_data(node, pseudo, data, identity),
            Self::MintStyleNodes(nodes) => mint_style_nodes(engine, &nodes),
            Self::ApplyTransaction(transaction) => apply_input_transaction(engine, &transaction),
            Self::ElementParts { node, pairs } => set_element_parts(engine, node, &pairs),
            Self::TextData { node, data } => {
                engine.record_boundary_call(EventKind::SetTextData, |payload| {
                    payload.write_u32(node);
                    payload.write_u16_slice(&data.to_utf16());
                });
                if let Some(node) = StyleNodeID::from_raw(node) {
                    engine.set_text_data(node, data);
                }
            }
            Self::ElementLanguage { node, language, text } => set_element_language(engine, node, language, &text),
            Self::PresentationalHints { node, kind, properties } => {
                super::bridge::register_element_declared_properties(engine, node, kind, &properties, &[]);
            }
            Self::AnimationKeyframes {
                tree_scope,
                shadow_root_identity,
                row,
            } => engine.animation_keyframes.set(tree_scope, shadow_root_identity, row),
            Self::AnimationEffectDescriptions { node, slot, effects } => {
                engine.animation_effect_descriptions.set(node, slot, effects);
            }
            #[cfg(feature = "style-recording")]
            Self::BenchmarkMarker(name) => {
                if engine.recording_id().is_some() {
                    engine.record_boundary_call(EventKind::BenchmarkMarker, |payload| payload.write_u16_slice(&name));
                }
            }
            Self::InlineStyle { node, data } => {
                super::bridge::register_element_declared_properties(
                    engine,
                    node,
                    super::bridge::FfiElementDeclarationKind::InlineStyle,
                    data.as_ref().map_or(&[], |data| data.properties.as_slice()),
                    data.as_ref().map_or(&[], |data| data.custom_properties.as_slice()),
                );
            }
            Self::AdoptAtom(lease) => {
                let atom = match lease.key() {
                    // SAFETY: The lease retains the fly string the raw identity names.
                    AtomKey::Raw(raw) => unsafe { super::bridge::intern_atom(engine, raw) },
                    AtomKey::Qualified(namespace, name) => {
                        super::bridge::operations::intern_qualified_atom(engine, namespace.0, name.0)
                    }
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
                if !row.is_empty() {
                    unpark_random_base_values(engine, node, &row);
                }
            }
            Self::NoteCustomPropertyEnvironment {
                identity,
                store,
                inheritable,
                inheritable_store,
            } => {
                let pointer = |store: &Option<super::custom_property_environments::RetainedCustomPropertyStore>| {
                    store.as_ref().map_or(std::ptr::null(), |store| store.pointer())
                };
                // SAFETY: The write retains both stores.
                unsafe {
                    super::bridge::note_custom_property_environment(
                        engine,
                        identity,
                        pointer(&store),
                        inheritable,
                        pointer(&inheritable_store),
                    );
                }
            }
            Self::AbsorbElementStyleInput {
                node,
                reaction,
                inherited_style_groups,
                absorbs_any,
                absorbed,
            } => {
                let engine_absorbed = super::bridge::operations::absorb_element_style_input(
                    engine,
                    node.raw(),
                    reaction,
                    inherited_style_groups,
                    absorbs_any,
                );
                // A fold that merges nothing into the reaction answers the same as none.
                debug_assert!(
                    engine_absorbed == absorbed
                        || (absorbed == 0
                            && engine_absorbed == u32::from(reaction) | (u32::from(inherited_style_groups) << 8)),
                    "the engine folds a style input as the host did"
                );
            }
            Self::NoteCustomPropertyName { name, string, text } => {
                // SAFETY: The write owns a reference to the fly string.
                unsafe { super::bridge::note_native_custom_property_name(engine, name, string.raw_identity(), &text) };
            }
        }
    }
}

// The writes a style replay makes as well, each kept apart so that a replay reaches no more of the engine's writes than
// it replays.

fn mint_style_nodes(engine: &mut StyleEngine, nodes: &[u32]) {
    engine.mint_style_nodes(nodes);
    engine.record_boundary_call(EventKind::MintStyleNodes, |payload| payload.write_u32_slice(nodes));
}

fn apply_input_transaction(engine: &mut StyleEngine, transaction: &InputTransaction) {
    engine.apply_transaction_batch(
        &transaction.tree,
        (&transaction.arrivals, &transaction.arrival_custom_state_atoms),
        &transaction.features,
        &transaction.states,
        &transaction.declarations,
        &transaction.element_style_inputs,
    );
    engine.record_boundary_call(EventKind::ApplyTransaction, |payload| {
        write_recording_tree_deltas(&transaction.tree, payload);
        payload.write_raw_slice(&transaction.arrivals);
        payload.write_u32_slice(&transaction.arrival_custom_state_atoms);
        payload.write_raw_slice(&transaction.features);
        write_recording_state_deltas(&transaction.states, payload);
        payload.write_raw_slice(&transaction.declarations);
        write_recording_element_style_inputs(&transaction.element_style_inputs, payload);
    });
}

/// Gives `node` the random base values of `row`, as one buffer of name code units with a length and a value per name,
/// which a replay reads as it was recorded.
fn unpark_random_base_values(engine: &mut StyleEngine, node: StyleNodeID, row: &[NamedBaseValue]) {
    let name_lengths = row
        .iter()
        .map(|(name, _)| u32::try_from(name.len()).expect("a random caching key's name fits in u32"))
        .collect::<Vec<_>>();
    let name_units = row
        .iter()
        .flat_map(|(name, _)| name.iter().copied())
        .collect::<Vec<_>>();
    let value_bits = row.iter().map(|&(_, value)| value.to_bits()).collect::<Vec<_>>();
    super::bridge::operations::set_element_random_base_values(
        engine,
        node.raw(),
        &name_lengths,
        &name_units,
        &value_bits,
    );
}

fn set_element_parts(engine: &mut StyleEngine, node: StyleNodeID, pairs: &[(StyleAtomID, StyleNodeID)]) {
    engine.set_element_parts(node, pairs);
    engine.record_boundary_call(EventKind::SetElementParts, |payload| {
        payload.write_u32(node.raw());
        payload.write_length(pairs.len());
        for (name, host) in pairs {
            payload.write_u32(name.0);
            payload.write_u32(host.raw());
        }
    });
}

fn set_element_language(engine: &mut StyleEngine, node: u32, language: u32, text: &[u16]) {
    if language != 0 && !text.is_empty() {
        // A range is not a name, so `:lang()` compares against the tag itself. It is recorded once per
        // language rather than once per element.
        engine.set_element_language_text(StyleAtomID(language), text);
    }
    if let Some(node) = StyleNodeID::from_raw(node) {
        engine.set_element_language(node, StyleAtomID(language));
    }
    engine.record_boundary_call(EventKind::SetElementLanguage, |payload| {
        payload.write_u32(node);
        payload.write_u32(language);
        payload.write_u16_slice(text);
    });
}

/// A copy of the input transaction `transaction` describes.
///
/// # Safety
/// Each pointer of `transaction` must cover its stated count.
unsafe fn input_transaction(transaction: &FfiStyleInputTransaction) -> InputTransaction {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        InputTransaction {
            tree: owned(transaction.tree_deltas, transaction.tree_delta_count),
            arrivals: owned(transaction.element_arrivals, transaction.element_arrival_count),
            arrival_custom_state_atoms: owned(
                transaction.arrival_custom_state_atoms,
                transaction.arrival_custom_state_atom_count,
            ),
            features: owned(transaction.local_feature_deltas, transaction.local_feature_delta_count),
            states: owned(transaction.state_deltas, transaction.state_delta_count),
            declarations: owned(
                transaction.element_declaration_deltas,
                transaction.element_declaration_delta_count,
            ),
            element_style_inputs: owned(transaction.element_style_inputs, transaction.element_style_input_count),
        }
    }
}

/// The parts an element exposes, each with the shadow host it is exposed to.
type ElementParts = Box<[(StyleAtomID, StyleNodeID)]>;

/// The parts the element `node` exposes, each with the shadow host it is exposed to, or none for no element.
fn element_parts(node: u32, names: &[u32], hosts: &[u32]) -> Option<(StyleNodeID, ElementParts)> {
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

impl EngineWrite {
    fn element_language(node: u32, language: u32, text: &[u16]) -> Self {
        let text = if language == 0 { Box::default() } else { text.into() };
        Self::ElementLanguage { node, language, text }
    }
}

/// Queues `write` for the render state of `host`'s document.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
pub(super) unsafe fn queue(host: *const DocumentHost, write: EngineWrite) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.queue_change(ArenaChange::Engine(write));
}

/// A copy of the `count` values at `values`.
///
/// # Safety
///
/// `values` must point at `count` readable values, or `count` be zero.
unsafe fn owned<T: Copy>(values: *const T, count: usize) -> Box<[T]> {
    // SAFETY: Guaranteed by the caller.
    unsafe { borrow(values, count) }.into()
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
    host: *const DocumentHost,
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
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, write) };
}

/// Keeps the custom-property environment an element now holds, named by `identity`: for the element's animation
/// overlay, the environment `base` its style resolved to beneath it, and whether `base` declares custom properties of its
/// own over the one it inherits. A null `data` is none.
///
/// # Safety
/// `host` must be a live document host, and `data` null or a live `Web::CSS::CustomPropertyData`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_custom_property_data(
    host: *const DocumentHost,
    node: u32,
    data: *const c_void,
    identity: u64,
    is_animation_overlay: bool,
    declares: bool,
    base: u64,
) {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    host.engine_memo().held.borrow_mut().follow_element(node, data);
    // SAFETY: Guaranteed by the caller.
    let data = (!data.is_null()).then(|| unsafe { RetainedCustomPropertyData::retain(data) });
    let write = EngineWrite::ElementCustomPropertyData {
        node,
        data,
        identity,
        sampled_over: is_animation_overlay.then_some(base),
        declares,
    };
    host.queue_change(ArenaChange::Engine(write));
}

/// Keeps the custom-property environment one of an element's synthetic pseudo-elements now holds, named by
/// `identity`. A null `data` is none.
///
/// # Safety
/// `host` must be a live document host, and `data` null or a live `Web::CSS::CustomPropertyData`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_pseudo_element_custom_property_data(
    host: *const DocumentHost,
    node: u32,
    pseudo: u8,
    data: *const c_void,
    identity: u64,
) {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    host.engine_memo()
        .held
        .borrow_mut()
        .follow_pseudo_element(node, pseudo, data);
    // SAFETY: Guaranteed by the caller.
    let data = (!data.is_null()).then(|| unsafe { RetainedCustomPropertyData::retain(data) });
    let write = EngineWrite::PseudoElementCustomPropertyData {
        node,
        pseudo,
        data,
        identity,
    };
    host.queue_change(ArenaChange::Engine(write));
}

/// Interns the name whose raw identity is `raw` for the document, without its engine: the host takes a reference to
/// the name's process-global atom, which the engine adopts later.
///
/// # Safety
/// `host` must be a live document host, and `raw` the raw identity of a live `AK::Utf16FlyString`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_intern_atom(host: *const DocumentHost, raw: usize) -> u32 {
    // SAFETY: Guaranteed by the caller.
    unsafe { adopt(host, AtomLease::acquire_raw(raw)) }
}

/// Interns `name` qualified by `namespace` for the document, as [`document_host_intern_atom`] does a name.
///
/// # Safety
/// `host` must be a live document host.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_intern_qualified_atom(
    host: *const DocumentHost,
    namespace: u32,
    name: u32,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        adopt(
            host,
            AtomLease::acquire_qualified(StyleAtomID(namespace), StyleAtomID(name)),
        )
    }
}

/// Queues the engine's adoption of the atom `lease` holds, and answers the atom.
///
/// # Safety
/// `host` must be a live document host.
unsafe fn adopt(host: *const DocumentHost, lease: AtomLease) -> u32 {
    let atom = lease.atom().0;
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, EngineWrite::AdoptAtom(lease)) };
    atom
}

/// Records what a custom property's name atom spells, and the fly string it is. The fly string is retained and never
/// recorded: a replay has no strings, and names its entries by atom alone.
///
/// # Safety
/// `host` must be a live document host, `raw` a live `AK::Utf16FlyString` raw representation, and `text` must name
/// `length` code units.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_note_custom_property_name(
    host: *const DocumentHost,
    name: u32,
    raw: usize,
    text: *const u16,
    length: usize,
) {
    // SAFETY: Guaranteed by the caller, which hands the write a reference of its own.
    let string = unsafe { ak::Utf16FlyString::from_raw_owned(raw) };
    if name == 0 {
        return;
    }
    let write = EngineWrite::NoteCustomPropertyName {
        name: StyleAtomID(name),
        string,
        // SAFETY: Guaranteed by the caller.
        text: unsafe { owned(text, length) },
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, write) };
}

/// Makes the `count` identities at `nodes`, which the host minted, live.
///
/// # Safety
/// `host` must be a live document host, and `nodes` must point at `count` readable `u32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_mint_style_nodes(host: *const DocumentHost, nodes: *const u32, count: usize) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, EngineWrite::MintStyleNodes(owned(nodes, count))) };
}

/// Applies the host's input transaction.
///
/// # Safety
/// `host` must be a live document host, and each pointer of `transaction` must cover its stated count.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_apply_transaction(
    host: *const DocumentHost,
    transaction: &FfiStyleInputTransaction,
) {
    // SAFETY: Guaranteed by the caller.
    let write = EngineWrite::ApplyTransaction(Box::new(unsafe { input_transaction(transaction) }));
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, write) };
}

/// Records the parts the element exposes, each with the shadow host at `hosts` it is exposed to.
///
/// # Safety
/// `host` must be a live document host, and `names` and `hosts` must each point at `count` readable values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_parts(
    host: *const DocumentHost,
    node: u32,
    names: *const u32,
    hosts: *const u32,
    count: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let (names, hosts) = unsafe { (borrow(names, count), borrow(hosts, count)) };
    if let Some((node, pairs)) = element_parts(node, names, hosts) {
        // SAFETY: Guaranteed by the caller.
        unsafe { queue(host, EngineWrite::ElementParts { node, pairs }) };
    }
}

/// Records the characters the text node holds.
///
/// # Safety
/// `host` must be a live document host, and the caller transfers one reference to the live string `data`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_text_data(host: *const DocumentHost, node: u32, data: usize) {
    // SAFETY: Guaranteed by the caller.
    let data = unsafe { ak::Utf16String::from_raw_owned(data) };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, EngineWrite::TextData { node, data }) };
}

/// Records the language the element resolves to, and the tag a `:lang()` range compares against.
///
/// # Safety
/// `host` must be a live document host, and `text` must point at `text_length` readable code units.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_language(
    host: *const DocumentHost,
    node: u32,
    language: u32,
    text: *const u16,
    text_length: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let text = unsafe { borrow(text, text_length) };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, EngineWrite::element_language(node, language, text)) };
}

/// Replays the recorded identities a host minted onto the replay engine `engine`.
///
/// # Safety
/// `engine` must be a live replay engine.
pub unsafe fn replay_mint_style_nodes(engine: StyleEngineHandle, nodes: &[u32]) {
    // SAFETY: Guaranteed by the caller.
    mint_style_nodes(unsafe { engine.get_mut() }, nodes);
}

/// Replays a recorded input transaction onto the replay engine `engine`.
///
/// # Safety
/// `engine` must be a live replay engine, and each pointer of `transaction` must cover its stated count.
pub unsafe fn replay_apply_transaction(engine: StyleEngineHandle, transaction: &FfiStyleInputTransaction) {
    // SAFETY: Guaranteed by the caller.
    unsafe { apply_input_transaction(engine.get_mut(), &input_transaction(transaction)) };
}

/// Replays the recorded parts of an element onto the replay engine `engine`.
///
/// # Safety
/// `engine` must be a live replay engine.
pub unsafe fn replay_set_element_parts(engine: StyleEngineHandle, node: u32, names: &[u32], hosts: &[u32]) {
    if let Some((node, pairs)) = element_parts(node, names, hosts) {
        // SAFETY: Guaranteed by the caller.
        set_element_parts(unsafe { engine.get_mut() }, node, &pairs);
    }
}

/// Replays the recorded language of an element onto the replay engine `engine`.
///
/// # Safety
/// `engine` must be a live replay engine.
pub unsafe fn replay_set_element_language(engine: StyleEngineHandle, node: u32, language: u32, text: &[u16]) {
    // SAFETY: Guaranteed by the caller.
    set_element_language(
        unsafe { engine.get_mut() },
        node,
        language,
        if language == 0 { &[] } else { text },
    );
}

/// A read of a document's style engine the host's style code makes.
pub(crate) enum StyleQuery {
    /// The nodes whose style depends on the viewport.
    ViewportDependentNodes,
    /// The record of an element or one of its pseudo-elements the host reads before the next style update.
    RecordDemand { node: StyleNodeID, demand: RecordDemand },
    /// The engine's id of the native rule the host names by `identity`.
    NativeRuleId(u64),
    /// The engine's counter at `index`.
    Counter(usize),
    /// The end of the transaction the host took last, which answers the identities it released.
    EndTransaction,
}

/// The answer to a [`StyleQuery`].
pub(crate) enum StyleAnswer {
    Number(u64),
    Nodes(Vec<u32>),
    RecordDemand(FfiRecordDemandAnswer),
    Counter(Option<(&'static str, u64)>),
}

impl StyleQuery {
    pub(crate) fn answer(self, engine: &mut StyleEngine) -> StyleAnswer {
        match self {
            Self::EndTransaction => StyleAnswer::Nodes(super::bridge::end_style_transaction(engine)),
            Self::ViewportDependentNodes => {
                StyleAnswer::Nodes(engine.computed_group_sets.viewport_dependent_nodes(|environment| {
                    engine.custom_property_environments.reads_viewport(environment)
                }))
            }
            Self::RecordDemand { node, demand } => {
                StyleAnswer::RecordDemand(super::bridge::answer_record_demand(engine, node, demand))
            }
            Self::NativeRuleId(identity) => {
                StyleAnswer::Number(engine.native_rule_id(identity).map_or(0, |id| u64::from(id.0) + 1))
            }
            Self::Counter(index) => {
                let retired = engine.computed_group_sets.retired_animation_overlay_records();
                engine
                    .counters
                    .set(Counter::RetiredAnimationOverlayRecords, retired as u64);
                let counter = engine.counters().iter().nth(index);
                engine.record_boundary_call(EventKind::Counter, |payload| {
                    payload.write_u64(u64::try_from(index).expect("counter index exceeds u64"));
                    payload.write_bool(counter.is_some());
                    if let Some((name, value)) = counter {
                        payload.write_bytes(name.as_bytes());
                        payload.write_u64(value);
                    }
                });
                StyleAnswer::Counter(counter)
            }
        }
    }
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
    crate::render_state::ask(read, host, crate::render_state::EngineCall(call)).0
}

/// Asks the style engine of `host`'s document `query`, in `read`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
pub(crate) unsafe fn ask_engine(host: *const DocumentHost, read: &BegunRead, query: StyleQuery) -> StyleAnswer {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    crate::render_state::ask(read, unsafe { &*host }, query)
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn ask_engine_number(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    query: StyleQuery,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let StyleAnswer::Number(number) = (unsafe { ask_engine(host, read, query) }) else {
        unreachable!("the question is answered with a number");
    };
    number
}

/// The custom-property environment an element holds, or null.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_element_custom_property_data(
    host: *const DocumentHost,
    node: u32,
) -> *const c_void {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return std::ptr::null();
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { document_host(host) }.engine_memo().held.borrow().element(node)
}

/// The custom-property environment one of an element's synthetic pseudo-elements holds, or null.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_pseudo_element_custom_property_data(
    host: *const DocumentHost,
    node: u32,
    pseudo: u8,
) -> *const c_void {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return std::ptr::null();
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { document_host(host) }
        .engine_memo()
        .held
        .borrow()
        .pseudo_element(node, pseudo)
}

/// Which of an element's synthetic pseudo-elements hold a custom-property environment, as a bit set by kind.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_pseudo_elements_with_custom_property_data(
    host: *const DocumentHost,
    node: u32,
) -> u64 {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return 0;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { document_host(host) }
        .engine_memo()
        .held
        .borrow()
        .pseudo_element_kinds(node)
}

/// The nodes whose style depends on the viewport, handed to `append` one by one.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `append` must take each node synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_viewport_dependent_nodes(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    context: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, u32),
) {
    // SAFETY: Guaranteed by the caller.
    let StyleAnswer::Nodes(nodes) = (unsafe { ask_engine(host, read, StyleQuery::ViewportDependentNodes) }) else {
        unreachable!("viewport dependent nodes are answered with nodes");
    };
    for node in nodes {
        // SAFETY: Guaranteed by the caller.
        unsafe { append(context, node) };
    }
}

/// Has the engine keep the random base values of the keys that name the element whose style node `node` was, as the
/// element loses it, in a slot the element holds, or answers null where no element has any.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_park_element_random_base_values(
    host: *const DocumentHost,
    node: u32,
) -> *const c_void {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return std::ptr::null();
    };
    // SAFETY: Guaranteed by the caller.
    if !unsafe { document_host(host) }.element_random_base_values_may_exist() {
        return std::ptr::null();
    }
    let slot = ParkedBaseValues::default();
    // SAFETY: Guaranteed by the caller.
    unsafe {
        queue(
            host,
            EngineWrite::ParkRandomBaseValues {
                node,
                slot: Arc::clone(&slot),
            },
        );
    }
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
    host: *const DocumentHost,
    node: u32,
    slot: *const c_void,
) {
    // SAFETY: Guaranteed by the caller.
    let slot = unsafe { ParkedBaseValues::from_raw(slot.cast()) };
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, EngineWrite::UnparkRandomBaseValues { node, slot }) };
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
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_answer_record_demand(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    demand: FfiRecordDemand,
) -> FfiRecordDemandAnswer {
    // SAFETY: Guaranteed by the caller.
    unsafe { ask_record_demand(host, read, node, RecordDemand::Element(demand)) }
}

/// Answer a read of one of an element's pseudo-elements the host makes before the next style update.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_answer_pseudo_element_record_demand(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    demand: FfiPseudoElementRecordDemand,
    pseudo_element: FfiDemandedPseudoElement,
) -> FfiRecordDemandAnswer {
    // SAFETY: Guaranteed by the caller.
    unsafe { ask_record_demand(host, read, node, RecordDemand::PseudoElement(demand, pseudo_element)) }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn ask_record_demand(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    demand: RecordDemand,
) -> FfiRecordDemandAnswer {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return FfiRecordDemandAnswer::default();
    };
    // SAFETY: Guaranteed by the caller.
    let StyleAnswer::RecordDemand(answer) =
        (unsafe { ask_engine(host, read, StyleQuery::RecordDemand { node, demand }) })
    else {
        unreachable!("a record demand is answered with a record");
    };
    answer
}

/// What a style record's values depend on.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_style_record_dependency_flags(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    style_record: u64,
) -> u8 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    let memo = &host.engine_memo().dependency_flags;
    if let Some(flags) = memo.get(style_record) {
        return flags;
    }
    if let Some(view) = host
        .engine_memo()
        .views
        .get(style_record)
        .or_else(|| host.transaction_record(style_record).map(|(view, _)| view))
    {
        return view.dependency_flags;
    }
    let Some(flags) = with_engine(read, host, |engine| engine.style_record_dependency_flags(style_record)) else {
        return 0;
    };
    memo.set(style_record, flags);
    flags
}

/// The identity of the custom-property environment a style record was computed in, or 0.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_style_record_custom_property_environment(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    style_record: u64,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    let memo = &host.engine_memo().record_environments;
    if let Some(environment) = memo.get(style_record) {
        return environment;
    }
    if let Some((_, Some(environment))) = host.transaction_record(style_record) {
        memo.set(style_record, environment);
        return environment;
    }
    let Some(environment) = with_engine(read, host, |engine| {
        engine
            .computed_group_sets
            .style_record_custom_property_environment(style_record)
    }) else {
        return 0;
    };
    memo.set(style_record, environment);
    environment
}

/// What the host knows of its document's style engine, which answers the host's reads without asking the render owner.
pub(crate) struct EngineMemo {
    /// The view of each style record the host asked about, the custom-property environment it was computed in, and
    /// what its values depend on: a published record never changes while it is live, and no other record ever takes
    /// its identity, as each identity carries the generation of its slot.
    pub(crate) views: Memo<super::bridge::FfiStyleRecordView>,
    pub(crate) record_environments: Memo<u64>,
    pub(crate) dependency_flags: Memo<u8>,
    /// The custom-property environments the engine holds for elements.
    pub(crate) held: std::cell::RefCell<HeldEnvironments>,
    /// The element style inputs the engine defers.
    pub(crate) deferred: std::cell::RefCell<DeferredInputs>,
    /// The versions of the animation effects the host described to the engine.
    pub(crate) described: std::cell::RefCell<super::effect_descriptions::DescribedVersions>,
    /// The transition baselines the host recorded in the engine.
    pub(crate) baselines: std::cell::RefCell<super::TransitionBaselines>,
}

impl EngineMemo {
    /// Follows `change`, a write the host queued for the engine.
    pub(crate) fn follow(&self, change: &super::bridge::StyleChange) {
        self.deferred.borrow_mut().follow(change);
        self.baselines.borrow_mut().follow(change);
    }
}

impl Default for EngineMemo {
    fn default() -> Self {
        Self {
            views: Memo::new(super::bridge::FfiStyleRecordView::missing()),
            record_environments: Memo::new(0),
            dependency_flags: Memo::new(0),
            held: Default::default(),
            deferred: Default::default(),
            described: Default::default(),
            baselines: Default::default(),
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
    elements: HashMap<StyleNodeID, *const c_void>,
    pseudo_elements: HashMap<StyleNodeID, Vec<(u8, *const c_void)>>,
}

/// An environment a move left an element (`None`) or one of its synthetic pseudo-elements, by the address of the host's
/// object for it. A moved element's entries come together, its own first.
pub(crate) type MovedEnvironment = (StyleNodeID, Option<u8>, usize);

impl HeldEnvironments {
    fn element(&self, node: StyleNodeID) -> *const c_void {
        self.elements.get(&node).copied().unwrap_or(std::ptr::null())
    }

    fn pseudo_element(&self, node: StyleNodeID, pseudo: u8) -> *const c_void {
        self.pseudo_elements
            .get(&node)
            .and_then(|environments| environments.iter().find(|&&(kind, _)| kind == pseudo))
            .map_or(std::ptr::null(), |&(_, data)| data)
    }

    fn pseudo_element_kinds(&self, node: StyleNodeID) -> u64 {
        self.pseudo_elements.get(&node).map_or(0, |environments| {
            environments.iter().fold(0, |kinds, &(kind, _)| kinds | (1 << kind))
        })
    }

    /// Follows `data`, null for none, which the host hands the engine for `node`.
    fn follow_element(&mut self, node: StyleNodeID, data: *const c_void) {
        if data.is_null() {
            self.elements.remove(&node);
        } else {
            self.elements.insert(node, data);
        }
    }

    /// Follows `data`, null for none, which the host hands the engine for a synthetic pseudo-element of `node`.
    fn follow_pseudo_element(&mut self, node: StyleNodeID, pseudo: u8, data: *const c_void) {
        let environments = self.pseudo_elements.entry(node).or_default();
        environments.retain(|&(kind, _)| kind != pseudo);
        if !data.is_null() {
            environments.push((pseudo, data));
        }
        if environments.is_empty() {
            self.pseudo_elements.remove(&node);
        }
    }

    /// Follows what a move left the elements it moved.
    pub(crate) fn follow_moved(&mut self, moved: &[MovedEnvironment]) {
        for &(node, pseudo, address) in moved {
            let data = std::ptr::with_exposed_provenance(address);
            match pseudo {
                None => {
                    self.follow_element(node, data);
                    self.pseudo_elements.remove(&node);
                }
                Some(pseudo) => self.follow_pseudo_element(node, pseudo, data),
            }
        }
    }

    /// Forgets what the nodes a transaction retired held, whose identities it `released`.
    pub(crate) fn forget(&mut self, released: &[u32]) {
        for node in released.iter().filter_map(|&raw| StyleNodeID::from_raw(raw)) {
            self.elements.remove(&node);
            self.pseudo_elements.remove(&node);
        }
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
    const ALL: (u8, u8) = (u8::MAX, u8::MAX);

    fn search(&self, node: StyleNodeID) -> Result<usize, usize> {
        self.inputs.binary_search_by_key(&node, |&(node, ..)| node)
    }

    /// Folds the input `node` owes into the reaction the host is about to apply to it, as
    /// [`super::StyleEngineState::absorb_element_style_input`] does, where the host knows the answer: `None` where an
    /// input it cannot name may move it.
    fn absorb(
        &mut self,
        node: StyleNodeID,
        reaction: u8,
        inherited_style_groups: u8,
        absorbs_any: bool,
    ) -> Option<u32> {
        use super::transaction::STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES as RECOMPUTE_DESCENDANTS;
        let known = self.search(node).ok();
        let (unnamed_reaction, unnamed_groups) = self.unnamed;
        if unnamed_reaction | unnamed_groups != 0 {
            let certain = match known {
                // The host knows nothing the element owes: a reaction that does not fold in every reaction an unnamed
                // input may carry answers the same, but for a descendant recompute, which folding adds to it.
                None if absorbs_any => {
                    unnamed_reaction & !reaction == 0 && unnamed_groups & !inherited_style_groups == 0
                }
                None => unnamed_reaction & RECOMPUTE_DESCENDANTS == 0 || reaction & RECOMPUTE_DESCENDANTS != 0,
                Some(_) => false,
            };
            if !certain {
                return None;
            }
        }
        let Some(index) = known else {
            return Some(0);
        };
        let (_, owed_reaction, owed_groups) = self.inputs[index];
        if !absorbs_any
            && (owed_reaction & !reaction & !RECOMPUTE_DESCENDANTS != 0 || owed_groups & !inherited_style_groups != 0)
        {
            return Some(0);
        }
        self.inputs.remove(index);
        Some(u32::from(reaction | owed_reaction) | (u32::from(inherited_style_groups | owed_groups) << 8))
    }

    /// Follows `change`, which the host queues for the engine.
    pub(crate) fn follow(&mut self, change: &super::bridge::StyleChange) {
        use super::bridge::StyleChange;
        use super::transaction::{STYLE_REACTION_PUBLISHED_STYLE, STYLE_REACTION_RECOMPUTE_STYLE};
        let (node, reaction, groups) = match *change {
            StyleChange::RecordDerivedElementStyleInput {
                node,
                reaction,
                inherited_style_groups,
            } => (node, reaction, inherited_style_groups),
            StyleChange::RecordContainerQueryInput { node } => {
                (node, STYLE_REACTION_PUBLISHED_STYLE | STYLE_REACTION_RECOMPUTE_STYLE, 0)
            }
            StyleChange::ConsumeElementStyleInput { node } => {
                if let Some(node) = StyleNodeID::from_raw(node)
                    && let Ok(index) = self.search(node)
                {
                    self.inputs.remove(index);
                }
                return;
            }
            StyleChange::NoteStyleReactionApplied {
                reaction,
                inherited_style_groups_changed,
                facts,
                ..
            } => {
                self.add_unnamed(super::child_reactions::derivable_child_reactions(
                    reaction,
                    inherited_style_groups_changed,
                    facts,
                ));
                return;
            }
            StyleChange::RecordFlatTreeDescendantStyleInputs {
                reaction,
                inherited_style_groups,
                ..
            } => {
                self.add_unnamed((reaction, inherited_style_groups));
                return;
            }
            StyleChange::RecordSizeContainerQueryDependents { .. }
            | StyleChange::EvaluateSizeContainersNeedingEvaluationAfterLayout {} => {
                self.add_unnamed((STYLE_REACTION_PUBLISHED_STYLE | STYLE_REACTION_RECOMPUTE_STYLE, 0));
                return;
            }
            StyleChange::Flush {} => {
                self.add_unnamed(Self::ALL);
                return;
            }
            _ => return,
        };
        let Some(node) = StyleNodeID::from_raw(node) else {
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

/// The view of `style_record` the host has without asking: one it asked for before, or one the style transaction it
/// drains carried. A published record never changes while it is live.
pub(crate) fn known_style_record_view(
    host: &DocumentHost,
    style_record: u64,
) -> Option<super::bridge::FfiStyleRecordView> {
    let memo = &host.engine_memo().views;
    if let Some(view) = memo.get(style_record) {
        return Some(view);
    }
    let (view, _) = host.transaction_record(style_record)?;
    memo.set(style_record, view);
    Some(view)
}

/// The before-change style the current style stabilization epoch decides the transitions of `node`'s element, or of
/// one of its pseudo-elements, against, or 0 before a pass has recorded one. The host knows it without asking: it
/// follows every baseline it records in the engine.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_transition_baseline(
    host: *const DocumentHost,
    node: u32,
    pseudo_kind: u8,
) -> u64 {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return 0;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { document_host(host) }
        .engine_memo()
        .baselines
        .borrow()
        .get(node, pseudo_kind)
}

/// Whether the engine has a style transaction pending, which the host knows without asking where it wrote nothing
/// since its last job.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_has_pending_transaction(host: *const DocumentHost, read: &BegunRead) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    match host.known_facts() {
        Some(facts) => facts.has_pending_style_transaction,
        None => with_engine(read, host, |engine| {
            super::bridge::operations::has_pending_transaction(engine)
        }),
    }
}

/// Whether the engine defers any element style input, which the host knows without asking where it wrote nothing since
/// its last job.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_has_deferred_element_style_inputs(
    host: *const DocumentHost,
    read: &BegunRead,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    match host.known_facts() {
        Some(facts) => facts.has_deferred_element_style_inputs,
        None => with_engine(read, host, |engine| {
            super::bridge::operations::has_deferred_element_style_inputs(engine)
        }),
    }
}

/// The synthetic pseudo-elements `node` has rules for, as a C++ record's pseudo-style mask, or no bits when the engine
/// holds no answer for the node. The host knows it without asking for an element whose row it composes in the
/// transaction it took last, which answered it beside the row.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_published_pseudo_style_mask(
    host: *const DocumentHost,
    read: &BegunRead,
    node: u32,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    host.transaction_pseudo_styles(node).map_or_else(
        || {
            with_engine(read, host, |engine| {
                super::bridge::operations::published_pseudo_style_mask(engine, node)
            })
        },
        |styles| styles.mask,
    )
}

/// The synthetic pseudo-elements `node` has rules for that settling its pseudo-elements over the composition of its
/// record leaves alone, where the style transaction the host drains composes its row on the host, and none otherwise.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_inert_pseudo_kinds(host: *const DocumentHost, node: u32) -> u64 {
    // SAFETY: Guaranteed by the caller.
    unsafe { document_host(host) }
        .transaction_pseudo_styles(node)
        .map_or(0, |styles| styles.inert)
}

/// Whether a size container waits for layout to be evaluated, which the host knows without asking where it wrote
/// nothing since its last job.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_has_size_containers_needing_evaluation_after_layout(
    host: *const DocumentHost,
    read: &BegunRead,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    match host.known_facts() {
        Some(facts) => facts.has_size_containers_needing_evaluation_after_layout,
        None => with_engine(read, host, |engine| {
            super::bridge::operations::has_size_containers_needing_evaluation_after_layout(engine)
        }),
    }
}

/// Where the engine's requirements of attribute value text are, which the host knows without asking where it queued no
/// rule since its last job.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_attribute_value_text_requirements_version(
    host: *const DocumentHost,
    read: &BegunRead,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    match host.known_selector_attribute_value_text_requirements_version() {
        Some(version) => super::inputs::with_attr_names_read(version),
        None => with_engine(read, host, |engine| {
            super::bridge::operations::attribute_value_text_requirements_version(engine)
        }),
    }
}

/// Whether the host records what the values of the attribute name `name` spell, which answers to the other forms the host
/// published with it, and which an `attr()` reads by `local_name` if it has one. The host knows where it queued no rule
/// since its last job: it holds the names the engine's selectors read the value text of, and what `attr()`s read is
/// the process's.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `local_name` null or `local_name_length` code
/// units.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_attribute_name_requires_value_text(
    host: *const DocumentHost,
    read: &BegunRead,
    name: u32,
    any_namespace: u32,
    folded_name: u32,
    folded_local: u32,
    local_name: *const u16,
    local_name_length: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    let keys = [name, any_namespace, folded_name, folded_local].map(StyleAtomID);
    match host.known_selectors_read_value_text_of(&keys) {
        Some(selectors_read) => {
            selectors_read
                || (!local_name.is_null()
                    // SAFETY: Guaranteed by the caller.
                    && crate::css::parser::arbitrary_substitution::attr_may_read_name(unsafe {
                        std::slice::from_raw_parts(local_name, local_name_length)
                    }))
        }
        None => with_engine(read, host, |engine| {
            super::bridge::operations::attribute_name_requires_value_text(engine, name)
        }),
    }
}

/// Folds the style input `node` owes into the reaction the host is about to apply to it, where the reaction covers it,
/// and answers the merged reaction in the low byte and the merged inherited style groups in the next, or zero, as
/// [`super::StyleEngineState::absorb_element_style_input`] does. The host answers where it knows the answer, and the
/// engine folds the input the same as it applies the host's writes.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_absorb_element_style_input(
    host: *const DocumentHost,
    read: &BegunRead,
    node: u32,
    reaction: u8,
    inherited_style_groups: u8,
    absorbs_any: bool,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    let Some(style_node) = StyleNodeID::from_raw(node) else {
        return 0;
    };
    // A job of the engine, or a frame in flight, moves what it defers beside what the host knows.
    let known = host.knows_engine_between_jobs().then(|| {
        host.engine_memo()
            .deferred
            .borrow_mut()
            .absorb(style_node, reaction, inherited_style_groups, absorbs_any)
    });
    let Some(Some(absorbed)) = known else {
        return with_engine(read, host, |engine| {
            super::bridge::operations::absorb_element_style_input(
                engine,
                node,
                reaction,
                inherited_style_groups,
                absorbs_any,
            )
        });
    };
    // The engine folds what the host did, and an input the host knows nothing of that folding merges nothing into the
    // reaction, which only an unnamed one is.
    if absorbed != 0 || host.engine_memo().deferred.borrow().may_owe_unnamed() {
        host.queue_change(ArenaChange::Engine(EngineWrite::AbsorbElementStyleInput {
            node: style_node,
            reaction,
            inherited_style_groups,
            absorbs_any,
            absorbed,
        }));
    }
    absorbed
}

/// A direct-mapped memo of one answer per key, which keeps the answers of the keys the host asked about last.
pub(crate) struct Memo<T: Copy> {
    slots: [std::cell::Cell<(u64, T)>; Memo::<()>::SLOTS],
}

impl<T: Copy> Memo<T> {
    const SLOTS: usize = 256;
    /// No record has this identity: a base record's leaves the top bit clear, and an overlay's generation is below it.
    const NO_KEY: u64 = u64::MAX;

    fn new(empty: T) -> Self {
        Self {
            slots: std::array::from_fn(|_| std::cell::Cell::new((Self::NO_KEY, empty))),
        }
    }

    fn slot(&self, key: u64) -> &std::cell::Cell<(u64, T)> {
        &self.slots[(key ^ (key >> 32)) as usize % Self::SLOTS]
    }

    pub(crate) fn get(&self, key: u64) -> Option<T> {
        let (held, answer) = self.slot(key).get();
        (held == key).then_some(answer)
    }

    pub(crate) fn set(&self, key: u64, answer: T) {
        self.slot(key).set((key, answer));
    }
}

/// The engine's id of the native rule `identity` names, plus one, or 0 for none.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_native_rule_id(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    identity: u64,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let id = unsafe { ask_engine_number(host, read, StyleQuery::NativeRuleId(identity)) };
    u32::try_from(id).expect("a native rule id fits 32 bits")
}

/// The name of the engine's counter at `index`, with its length and value written out, or null past the last one.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and the out pointers writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_counter(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    index: usize,
    out_value: *mut u64,
    out_name_length: *mut usize,
) -> *const u8 {
    // SAFETY: Guaranteed by the caller.
    let StyleAnswer::Counter(counter) = (unsafe { ask_engine(host, read, StyleQuery::Counter(index)) }) else {
        unreachable!("a counter is answered with a counter");
    };
    let Some((name, value)) = counter else {
        return std::ptr::null();
    };
    // SAFETY: Guaranteed by the caller.
    unsafe {
        out_value.write(value);
        out_name_length.write(name.len());
    }
    name.as_ptr()
}

/// Decides what each property `input` prepared does to the transitions of its target, as the target's style changes
/// from the record `before` to the record `after` it installed. Writes the values each decision compared into its
/// property, and the decision into `actions`. The style transaction the host drains decided the step beside the row of
/// an element whose step nothing but the engine's records decides; any other the render owner decides, reading the
/// transform reference box of the target's element's box as it does.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, `input` must be valid, and `actions` must point at
/// writable storage for one action per property.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_decide_transitions(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    before: u64,
    after: u64,
    input: *const FfiTransitionInput,
    actions: *mut FfiTransitionAction,
) {
    crate::css::ffi_stats::rust_style_ffi_note_transition_decision();
    // SAFETY: Guaranteed by the caller.
    let input = unsafe { &*input };
    if input.property_count == 0 {
        return;
    }
    // SAFETY: As above.
    let (properties, actions) = unsafe {
        (
            std::slice::from_raw_parts_mut(input.properties, input.property_count),
            std::slice::from_raw_parts_mut(actions, input.property_count),
        )
    };
    let decision = TransitionDecision {
        before,
        after,
        context: input.context,
        element: (input.target_pseudo_kind == u8::MAX)
            .then(|| StyleNodeID::from_raw(input.target_node))
            .flatten(),
    };
    // SAFETY: As above.
    let host = unsafe { document_host(host) };
    if decision.element.is_some()
        && host.answer_decided_transition_step(input.target_node, &decision, properties, actions)
    {
        return;
    }
    let element_box = crate::layout::node_data::NodeSlotId {
        index: input.element_box_slot,
    };
    with_engine_and_arena(read, host, |engine, arena| {
        let reference_box =
            crate::painting::ffi::committed_transform_reference_box(&arena.paintable_rows(), element_box);
        decision.decide(engine, reference_box, properties, actions);
    });
}
