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

use super::StyleAtomID;
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
    /// What a custom property's name atom spells, and the fly string it is.
    NoteCustomPropertyName {
        name: StyleAtomID,
        string: ak::Utf16FlyString,
        text: Box<[u16]>,
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
    let data = (!data.is_null()).then(|| unsafe { RetainedCustomPropertyData::retain(data) });
    let write = EngineWrite::ElementCustomPropertyData {
        node,
        data,
        identity,
        sampled_over: is_animation_overlay.then_some(base),
        declares,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, write) };
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
    let data = (!data.is_null()).then(|| unsafe { RetainedCustomPropertyData::retain(data) });
    let write = EngineWrite::PseudoElementCustomPropertyData {
        node,
        pseudo,
        data,
        identity,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, write) };
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
    /// The custom-property environment an element holds.
    ElementCustomPropertyData(StyleNodeID),
    /// The custom-property environment one of an element's synthetic pseudo-elements holds.
    PseudoElementCustomPropertyData { node: StyleNodeID, pseudo: u8 },
    /// Which of an element's synthetic pseudo-elements hold a custom-property environment, as a bit set by kind.
    PseudoElementsWithCustomPropertyData(StyleNodeID),
    /// The nodes whose style depends on the viewport.
    ViewportDependentNodes,
    /// The record of an element or one of its pseudo-elements the host reads before the next style update.
    RecordDemand { node: StyleNodeID, demand: RecordDemand },
    /// What a style record's values depend on.
    StyleRecordDependencyFlags(u64),
    /// The identity of the custom-property environment a style record was computed in.
    StyleRecordCustomPropertyEnvironment(u64),
    /// The engine's id of the native rule the host names by `identity`.
    NativeRuleId(u64),
    /// The engine's counter at `index`.
    Counter(usize),
    /// The end of the transaction the host took last, which answers the identities it released.
    EndTransaction,
    /// What a transition step's change of records does to its target's transitions.
    DecideTransitions(crate::css::transition::TransitionDecision),
}

/// The answer to a [`StyleQuery`].
pub(crate) enum StyleAnswer {
    /// The address of a host object the engine holds, or 0.
    HostObject(usize),
    Number(u64),
    Nodes(Vec<u32>),
    RecordDemand(FfiRecordDemandAnswer),
    Counter(Option<(&'static str, u64)>),
    Transitions(Vec<crate::css::transition::DecidedTransition>),
}

impl StyleQuery {
    pub(crate) fn answer(self, engine: &mut StyleEngine) -> StyleAnswer {
        match self {
            Self::EndTransaction => StyleAnswer::Nodes(super::bridge::end_style_transaction(engine)),
            Self::DecideTransitions(decision) => StyleAnswer::Transitions(decision.answer(engine)),
            Self::ElementCustomPropertyData(node) => {
                StyleAnswer::HostObject(engine.element_custom_property_data(node).addr())
            }
            Self::PseudoElementCustomPropertyData { node, pseudo } => {
                StyleAnswer::HostObject(engine.pseudo_element_custom_property_data(node, pseudo).addr())
            }
            Self::PseudoElementsWithCustomPropertyData(node) => {
                StyleAnswer::Number(engine.pseudo_elements_with_custom_property_data(node))
            }
            Self::ViewportDependentNodes => {
                StyleAnswer::Nodes(engine.computed_group_sets.viewport_dependent_nodes(|environment| {
                    engine.custom_property_environments.reads_viewport(environment)
                }))
            }
            Self::RecordDemand { node, demand } => {
                StyleAnswer::RecordDemand(super::bridge::answer_record_demand(engine, node, demand))
            }
            Self::StyleRecordDependencyFlags(record) => {
                StyleAnswer::Number(engine.style_record_dependency_flags(record).unwrap_or(0).into())
            }
            Self::StyleRecordCustomPropertyEnvironment(record) => StyleAnswer::Number(
                engine
                    .computed_group_sets
                    .style_record_custom_property_environment(record)
                    .unwrap_or(0),
            ),
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
    crate::render_state::ask(read, host, crate::render_state::EngineCall(call))
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

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn ask_engine_host_object(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    query: StyleQuery,
) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    let StyleAnswer::HostObject(address) = (unsafe { ask_engine(host, read, query) }) else {
        unreachable!("the question is answered with a host object");
    };
    std::ptr::with_exposed_provenance(address)
}

/// The custom-property environment an element holds, or null.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_element_custom_property_data(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
) -> *const c_void {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return std::ptr::null();
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { ask_engine_host_object(host, read, StyleQuery::ElementCustomPropertyData(node)) }
}

/// The custom-property environment one of an element's synthetic pseudo-elements holds, or null.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_pseudo_element_custom_property_data(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    pseudo: u8,
) -> *const c_void {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return std::ptr::null();
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { ask_engine_host_object(host, read, StyleQuery::PseudoElementCustomPropertyData { node, pseudo }) }
}

/// Which of an element's synthetic pseudo-elements hold a custom-property environment, as a bit set by kind.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_pseudo_elements_with_custom_property_data(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
) -> u64 {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return 0;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { ask_engine_number(host, read, StyleQuery::PseudoElementsWithCustomPropertyData(node)) }
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
    let flags = unsafe { ask_engine_number(host, read, StyleQuery::StyleRecordDependencyFlags(style_record)) };
    u8::try_from(flags).expect("dependency flags fit a byte")
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
    unsafe {
        ask_engine_number(
            host,
            read,
            StyleQuery::StyleRecordCustomPropertyEnvironment(style_record),
        )
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
/// property, and the decision into `actions`.
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
    let properties = unsafe { std::slice::from_raw_parts_mut(input.properties, input.property_count) };
    let decision = TransitionDecision {
        before,
        after,
        context: input.context,
        element: (input.target_pseudo_kind == u8::MAX)
            .then(|| StyleNodeID::from_raw(input.target_node))
            .flatten(),
        properties: crate::render_state::Lent::new(properties),
    };
    // SAFETY: As above.
    let StyleAnswer::Transitions(decided) =
        (unsafe { ask_engine(host, read, StyleQuery::DecideTransitions(decision)) })
    else {
        unreachable!("a transition step is answered with its decisions");
    };
    for (index, (property, decided)) in properties.iter_mut().zip(decided).enumerate() {
        property.before_change_value = decided.before_change_value;
        property.after_change_value = decided.after_change_value;
        property.current_value = decided.current_value;
        // SAFETY: As above.
        unsafe { actions.add(index).write(decided.action) };
    }
}
