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
    FfiPseudoElementRecordDemand, FfiRecordDemand, FfiRecordDemandAnswer, FfiStateDelta, FfiTreeDelta, borrow,
};
use super::font_resolution::{FontResolverHost, PublishedFontFaces};
use super::inputs::RetainedCustomPropertyData;
use super::instrumentation::Counter;
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
    /// The tree, arrival, feature, state and declaration deltas the host staged, as one batch.
    ApplyTransaction(Box<StagedInput>),
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
    /// An element's inline declaration block, or none.
    InlineStyle {
        node: StyleNodeID,
        data: Option<Arc<crate::css::declaration_block::DeclarationBlockData>>,
    },
    /// A name the host interned, which the engine adopts as the atom the host's lease holds.
    AdoptAtom(AtomLease),
    /// The other names the attribute name `name` answers to, and the local name an `attr()` reads it by where it is in
    /// no namespace.
    AttributeNameForms {
        name: StyleAtomID,
        forms: super::index::AttributeNameForms,
        substitution_name: Option<Box<[u16]>>,
    },
    /// What a value of the attribute name `name` spells, which the engine keeps where something reads it.
    AttributeValueText {
        name: StyleAtomID,
        value: StyleAtomID,
        text: crate::css::retained_fly_string::RetainedUtf16FlyString,
    },
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
    /// The timing the host sampled each of an element's own effects with, by identity, where it sampled without asking.
    AnimationEffectTimings {
        node: StyleNodeID,
        timings: super::animations::SampledEffectTimings,
    },
}

/// The tree, arrival, feature, state and declaration deltas the host recorded since it last submitted them, which reach
/// the engine as one batch.
#[derive(Default)]
pub(crate) struct StagedInput {
    tree: Vec<FfiTreeDelta>,
    arrivals: Vec<FfiElementArrival>,
    /// The custom states of every arrival, which each names by its offset and count.
    arrival_custom_state_atoms: Vec<u32>,
    features: Vec<FfiLocalFeatureDelta>,
    states: Vec<FfiStateDelta>,
    declarations: Vec<FfiElementDeclarationDelta>,
}

impl StagedInput {
    fn is_empty(&self) -> bool {
        self.tree.is_empty()
            && self.arrivals.is_empty()
            && self.features.is_empty()
            && self.states.is_empty()
            && self.declarations.is_empty()
    }
}

impl EngineWrite {
    /// Whether the write may move a fact the host knows of the render state. What a text, an attribute name or an
    /// attribute value spells never does: it stages no style input and touches no layout box.
    pub(crate) fn may_move_facts(&self) -> bool {
        !matches!(
            self,
            Self::TextData { .. } | Self::AttributeNameForms { .. } | Self::AttributeValueText { .. }
        )
    }

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
            Self::MintStyleNodes(nodes) => engine.mint_style_nodes(&nodes),
            Self::ApplyTransaction(input) => engine.apply_transaction_batch(
                &input.tree,
                (&input.arrivals, &input.arrival_custom_state_atoms),
                &input.features,
                &input.states,
                &input.declarations,
                &[],
            ),
            Self::ElementParts { node, pairs } => engine.set_element_parts(node, &pairs),
            Self::TextData { node, data } => {
                if let Some(node) = StyleNodeID::from_raw(node) {
                    engine.set_text_data(node, data);
                }
            }
            Self::ElementLanguage { node, language, text } => {
                if language != 0 && !text.is_empty() {
                    // A range is not a name, so `:lang()` compares against the tag itself. It is recorded once per
                    // language rather than once per element.
                    engine.set_element_language_text(StyleAtomID(language), &text);
                }
                if let Some(node) = StyleNodeID::from_raw(node) {
                    engine.set_element_language(node, StyleAtomID(language));
                }
            }
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
            Self::AnimationEffectTimings { node, timings } => {
                for (identity, timing) in timings {
                    engine
                        .animation_effect_descriptions
                        .keep_timing(node, 0, identity, timing);
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
                    AtomKey::Raw(raw) => engine.intern_atom(raw).0,
                    AtomKey::Qualified(namespace, name) => engine.intern_qualified_atom(namespace, name).0,
                };
                // The engine took a reference of its own, so the global atom stays the one the lease held.
                assert_eq!(atom, lease.atom().0, "a document adopts the atom its host interned");
            }
            Self::AttributeNameForms {
                name,
                forms,
                substitution_name,
            } => {
                engine.note_attribute_name_forms(name, forms);
                if let Some(local_name) = substitution_name {
                    engine.note_attribute_substitution_name(name, &local_name);
                }
            }
            Self::AttributeValueText { name, value, text } => {
                if engine.attribute_name_requires_value_text(name) {
                    engine.set_attribute_value_text(value, &text.to_utf16());
                }
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
                let engine_absorbed =
                    engine.absorb_element_style_input(node, reaction, inherited_style_groups, absorbs_any);
                // A fold that merges nothing into the reaction answers the same as none.
                debug_assert!(
                    engine_absorbed == absorbed
                        || (absorbed == 0
                            && engine_absorbed == u32::from(reaction) | (u32::from(inherited_style_groups) << 8)),
                    "the engine folds a style input as the host did"
                );
            }
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

/// Queues `write` for the render state of `host`'s document.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
pub(super) unsafe fn queue(host: &DocumentHost, write: EngineWrite) {
    host.queue_change(ArenaChange::Engine(write));
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
/// overlay, the environment `base` its style resolved to beneath it, and whether `base` declares custom properties of its
/// own over the one it inherits. A null `data` is none.
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
    declares: bool,
    base: u64,
) {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
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
    host: &DocumentHost,
    node: u32,
    pseudo: u8,
    data: *const c_void,
    identity: u64,
) {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
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

/// Makes the `count` identities at `nodes`, which the host minted, live.
///
/// # Safety
/// `host` must be a live document host, and `nodes` must point at `count` readable `u32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_mint_style_nodes(host: &DocumentHost, nodes: *const u32, count: usize) {
    // SAFETY: Guaranteed by the caller.
    let nodes = unsafe { owned(nodes, count) };
    host.queue_change(ArenaChange::Engine(EngineWrite::MintStyleNodes(nodes)));
}

/// Stages the tree delta `delta` for the input the host submits next.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_tree_delta(host: &DocumentHost, delta: &FfiTreeDelta) {
    host.engine_memo().staged_input.borrow_mut().tree.push(*delta);
}

/// Stages the arrival of an element with the custom states at `custom_states` for the input the host submits next.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `custom_states` must point at
/// `custom_state_count` readable atoms.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_element_arrival(
    host: &DocumentHost,
    arrival: &FfiElementArrival,
    custom_states: *const u32,
    custom_state_count: usize,
) {
    let mut staged = host.engine_memo().staged_input.borrow_mut();
    let mut arrival = *arrival;
    arrival.custom_state_offset =
        u32::try_from(staged.arrival_custom_state_atoms.len()).expect("an input stages fewer than 2^32 custom states");
    arrival.custom_state_count =
        u32::try_from(custom_state_count).expect("an element has fewer than 2^32 custom states");
    // SAFETY: Guaranteed by the caller.
    staged
        .arrival_custom_state_atoms
        .extend_from_slice(unsafe { borrow(custom_states, custom_state_count) });
    staged.arrivals.push(arrival);
}

/// Stages the local feature delta `delta` for the input the host submits next.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_local_feature_delta(host: &DocumentHost, delta: &FfiLocalFeatureDelta) {
    host.engine_memo().staged_input.borrow_mut().features.push(*delta);
}

/// Stages the state delta `delta` for the input the host submits next.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_state_delta(host: &DocumentHost, delta: &FfiStateDelta) {
    host.engine_memo().staged_input.borrow_mut().states.push(*delta);
}

/// Stages the element declaration delta `delta` for the input the host submits next.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_stage_element_declaration_delta(
    host: &DocumentHost,
    delta: &FfiElementDeclarationDelta,
) {
    host.engine_memo().staged_input.borrow_mut().declarations.push(*delta);
}

/// Whether the host staged deltas it has not submitted yet.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_has_staged_input(host: &DocumentHost) -> bool {
    !host.engine_memo().staged_input.borrow().is_empty()
}

/// Submits the deltas the host staged as one batch, which the engine applies in order with the host's other writes.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_submit_staged_input(host: &DocumentHost) {
    let staged = std::mem::take(&mut *host.engine_memo().staged_input.borrow_mut());
    if !staged.is_empty() {
        host.queue_change(ArenaChange::Engine(EngineWrite::ApplyTransaction(Box::new(staged))));
    }
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
        host.queue_change(ArenaChange::Engine(EngineWrite::ElementParts { node, pairs }));
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
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_element_custom_property_data(host: &DocumentHost, node: u32) -> *const c_void {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return std::ptr::null();
    };
    host.engine_memo().held.borrow().element(node)
}

/// The custom-property environment one of an element's synthetic pseudo-elements holds, or null.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_pseudo_element_custom_property_data(
    host: &DocumentHost,
    node: u32,
    pseudo: u8,
) -> *const c_void {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return std::ptr::null();
    };
    host.engine_memo().held.borrow().pseudo_element(node, pseudo)
}

/// Which of an element's synthetic pseudo-elements hold a custom-property environment, as a bit set by kind.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_pseudo_elements_with_custom_property_data(host: &DocumentHost, node: u32) -> u64 {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return 0;
    };
    host.engine_memo().held.borrow().pseudo_element_kinds(node)
}

/// The nodes whose style depends on the viewport, handed to `append` one by one.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `append` must take each node synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_viewport_dependent_nodes(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    context: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, u32),
) {
    let nodes = with_engine(read, host, |engine| {
        engine
            .computed_group_sets
            .viewport_dependent_nodes(|environment| engine.custom_property_environments.reads_viewport(environment))
    });
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
pub unsafe extern "C" fn style_engine_park_element_random_base_values(host: &DocumentHost, node: u32) -> *const c_void {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return std::ptr::null();
    };
    if !host.element_random_base_values_may_exist() {
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
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_answer_record_demand(
    host: &DocumentHost,
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
    host: &DocumentHost,
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

/// What a style record's values depend on.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_style_record_dependency_flags(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    style_record: u64,
) -> u8 {
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
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    style_record: u64,
) -> u64 {
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
    /// The atoms the host interned for the engine.
    pub(crate) atoms: std::cell::RefCell<super::host_atoms::HostAtoms>,
    /// The deltas the host recorded since it last submitted them.
    pub(crate) staged_input: std::cell::RefCell<StagedInput>,
    /// The elements that arrived, moved or retired beside the style transaction that flew, which knows nothing of
    /// them, until the drain of its reactions ends.
    pub(crate) beside_flown_transaction: std::cell::RefCell<super::HashSet<StyleNodeID>>,
    /// While a batch's reactions are applied, a host's own style application may rewrite the declarations of an
    /// element in its shadow tree, after the engine computed that element's record from the ones it had. These name
    /// the elements whose declarations changed that way, while `applying_reactions` counts the batches being applied.
    declarations_changed_during_apply: std::cell::RefCell<super::HashSet<StyleNodeID>>,
    applying_reactions: std::cell::Cell<u32>,
    /// Whether the transaction the host takes is one a style update applies, whose reactions close over the elements
    /// they inherit through as the engine computes them.
    takes_transaction_to_apply: std::cell::Cell<bool>,
}

impl EngineMemo {
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

    pub(crate) fn declarations_changed_during_apply(&self, node: StyleNodeID) -> bool {
        self.declarations_changed_during_apply.borrow().contains(&node)
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

    /// The elements a transaction the host takes now leaves to the next transaction as its reactions close over the
    /// elements they inherit through, or none where the host does not apply its reactions.
    pub(crate) fn closes_taken_transaction_beside(&self) -> Option<super::HashSet<StyleNodeID>> {
        self.takes_transaction_to_apply
            .get()
            .then(|| self.beside_flown_transaction.borrow().clone())
    }

    /// Notes the elements whose declarations change until the answer is dropped, beside the batches being applied
    /// already.
    pub(crate) fn note_declaration_changes_during_apply(&self) -> impl Drop + '_ {
        struct Noting<'a>(&'a EngineMemo);
        impl Drop for Noting<'_> {
            fn drop(&mut self) {
                let applying = self.0.applying_reactions.get() - 1;
                self.0.applying_reactions.set(applying);
                if applying == 0 {
                    self.0.declarations_changed_during_apply.borrow_mut().clear();
                }
            }
        }
        self.applying_reactions.set(self.applying_reactions.get() + 1);
        Noting(self)
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
            atoms: Default::default(),
            staged_input: Default::default(),
            beside_flown_transaction: Default::default(),
            declarations_changed_during_apply: Default::default(),
            applying_reactions: Default::default(),
            takes_transaction_to_apply: Default::default(),
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
    /// [`super::StyleEngine::absorb_element_style_input`] does, where the host knows the answer: `None` where an
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

    /// Follows `queued`, in the order the engine applies the writes the host queues: a write held behind the drain of a
    /// style transaction's reactions reaches the engine after the drain's.
    fn follow(&mut self, queued: crate::render_state::QueuedStyleChange<'_>) {
        use super::boundary::StyleChange;
        use super::transaction::{STYLE_REACTION_PUBLISHED_STYLE, STYLE_REACTION_RECOMPUTE_STYLE};
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
pub unsafe extern "C" fn style_engine_transition_baseline(host: &DocumentHost, node: u32, pseudo_kind: u8) -> u64 {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return 0;
    };
    host.engine_memo().baselines.borrow().get(node, pseudo_kind)
}

/// Whether the engine has a style transaction pending, which the host knows without asking where it wrote nothing
/// since its last job.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_has_pending_transaction(host: &DocumentHost, read: &BegunRead) -> bool {
    match host.known_engine_facts() {
        Some(facts) => facts.has_pending_style_transaction,
        None => with_engine(read, host, |engine| engine.has_pending_transaction()),
    }
}

/// Whether the engine defers any element style input, which the host knows without asking where it wrote nothing since
/// its last job.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_has_deferred_element_style_inputs(host: &DocumentHost, read: &BegunRead) -> bool {
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

/// The synthetic pseudo-elements `node` has rules for, as a C++ record's pseudo-style mask, or no bits when the engine
/// holds no answer for the node. The host knows it without asking for an element whose row it composes in the
/// transaction it took last, which answered it beside the row.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_published_pseudo_style_mask(
    host: &DocumentHost,
    read: &BegunRead,
    node: u32,
) -> u64 {
    host.transaction_pseudo_styles(node).map_or_else(
        || {
            with_engine(read, host, |engine| {
                StyleNodeID::from_raw(node).map_or(0, |node| engine.published_pseudo_style_mask(node))
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
pub unsafe extern "C" fn style_engine_inert_pseudo_kinds(host: &DocumentHost, node: u32) -> u64 {
    host.transaction_pseudo_styles(node).map_or(0, |styles| styles.inert)
}

/// Whether a size container waits for layout to be evaluated, which the host knows without asking where it wrote
/// nothing since its last job.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_has_size_containers_needing_evaluation_after_layout(
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

/// Folds the style input `node` owes into the reaction the host is about to apply to it, where the reaction covers it,
/// and answers the merged reaction in the low byte and the merged inherited style groups in the next, or zero, as
/// [`super::StyleEngine::absorb_element_style_input`] does. The host answers where it knows the answer, and the
/// engine folds the input the same as it applies the host's writes.
pub(crate) fn absorb_element_style_input(
    host: &DocumentHost,
    read: &BegunRead,
    style_node: StyleNodeID,
    reaction: u8,
    inherited_style_groups: u8,
    absorbs_any: bool,
) -> u32 {
    // A job of the engine, or a frame in flight, moves what it defers beside what the host knows, and a fold held
    // behind the drain of a style transaction's reactions reaches the engine after the drain's writes.
    let known = (host.knows_engine_between_jobs() && !host.holds_style_writes()).then(|| {
        host.engine_memo()
            .deferred
            .borrow_mut()
            .absorb(style_node, reaction, inherited_style_groups, absorbs_any)
    });
    let Some(Some(absorbed)) = known else {
        return with_engine(read, host, |engine| {
            engine.absorb_element_style_input(style_node, reaction, inherited_style_groups, absorbs_any)
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

/// Notes that the host rewrote the declarations of `node`, which a batch of reactions being applied reads.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_note_element_declarations_changed(host: &DocumentHost, node: u32) {
    if let Some(node) = StyleNodeID::from_raw(node) {
        host.engine_memo().note_declarations_changed(node);
    }
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
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    identity: u64,
) -> u32 {
    with_engine(read, host, |engine| {
        engine.native_rule_id(identity).map_or(0, |id| id.0 + 1)
    })
}

/// The name of the engine's counter at `index`, with its length and value written out, or null past the last one.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and the out pointers writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_counter(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    index: usize,
    out_value: *mut u64,
    out_name_length: *mut usize,
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
    // SAFETY: Guaranteed by the caller.
    unsafe {
        out_value.write(value);
        out_name_length.write(name.len());
    }
    name.as_ptr()
}

/// Decides what the transition step of `input`'s target does to its transitions, as the target's style changes from the
/// record `before` to the record `after` it installed, and hands `on_actions` one action per property it decided over.
/// The style transaction the host drains decided the step beside the row of an element whose step nothing but the
/// engine's records decides; any other the render owner decides, reading the transform reference box of the target's
/// element's box as it does.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `input` must be valid. The values the actions
/// point at live until `on_actions` returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_decide_transitions(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    before: u64,
    after: u64,
    input: &FfiTransitionInput,
    on_actions: unsafe extern "C" fn(*mut c_void, *const FfiTransitionAction, usize),
    context: *mut c_void,
) {
    crate::css::ffi_stats::note_transition_decision();
    // SAFETY: Guaranteed by the caller.
    let existing = unsafe {
        crate::css::custom_properties::ffi_slice(input.existing_transitions, input.existing_transition_count)
    };
    // SAFETY: As above.
    let hand_over = |actions: &[FfiTransitionAction]| unsafe { on_actions(context, actions.as_ptr(), actions.len()) };
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
    let actions = with_engine_and_arena(read, host, |engine, arena| {
        let reference_box =
            crate::painting::ffi::committed_transform_reference_box(&arena.paintable_rows(), element_box);
        decision.decide(engine, reference_box, existing)
    });
    hand_over(&actions);
}
