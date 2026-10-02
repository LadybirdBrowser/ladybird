/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The writes the host makes to a document's style engine that the boundary generator does not write for it, as
//! changes the document's render state applies to the engine in the order the host made them. Each entry takes the
//! document host, never the engine, and owns what it hands over: borrowed arrays are copied, and the host's objects
//! are referenced, as the entry queues the change.

use super::StyleAtomID;
use super::bridge::{
    FfiElementArrival, FfiElementDeclarationDelta, FfiElementStyleInput, FfiLocalFeatureDelta, FfiStateDelta,
    FfiStyleInputTransaction, FfiTreeDelta, borrow, write_recording_element_style_inputs, write_recording_state_deltas,
    write_recording_tree_deltas,
};
use super::font_resolution::{FontResolutionCache, FontResolverHost, PublishedFontFaces};
use super::inputs::RetainedCustomPropertyData;
use super::record_replay::EventKind;
use super::tree::StyleNodeID;
use super::{StyleEngine, StyleEngineHandle};
use crate::render_state::{ArenaChange, DocumentHost};
use std::ffi::c_void;

/// One hand-written write of the host to a document's style engine.
pub(crate) enum EngineWrite {
    /// The document's `@font-face` table and the memo of the cascades resolved from it.
    PublishFontFaces(PublishedFontFaces),
    /// The custom-property environment an element holds, or none.
    ElementCustomPropertyData {
        node: StyleNodeID,
        data: Option<RetainedCustomPropertyData>,
        identity: u64,
        is_animation_overlay: bool,
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
            Self::PublishFontFaces(font_faces) => match &mut engine.host.font_resolver {
                Some(resolver) => resolver.publish(font_faces),
                None => {
                    engine.host.font_resolver = Some(FontResolverHost::new(font_faces));
                    engine.retained.font_resolution = Some(FontResolutionCache::default());
                }
            },
            Self::ElementCustomPropertyData {
                node,
                data,
                identity,
                is_animation_overlay,
                declares,
            } => engine.set_element_custom_property_data(node, data, identity, is_animation_overlay, declares),
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
unsafe fn queue(host: *const DocumentHost, write: EngineWrite) {
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
/// resolution of the engine reads. Takes one reference to each.
///
/// # Safety
/// `host` must be a live document host, and `snapshot` and `memo` a live `Web::CSS::FontFaceSnapshot` and
/// `FontCascadeMemo`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_publish_font_faces(
    host: *const DocumentHost,
    snapshot: *const c_void,
    memo: *const c_void,
) {
    // SAFETY: Guaranteed by the caller.
    let font_faces = unsafe { PublishedFontFaces::adopt(snapshot, memo) };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, EngineWrite::PublishFontFaces(font_faces)) };
}

/// Keeps the custom-property environment an element now holds, named by `identity`: whether it is the element's
/// animation overlay, and whether the environment its style resolves to declares custom properties of its own. A null
/// `data` is none.
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
        is_animation_overlay,
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
