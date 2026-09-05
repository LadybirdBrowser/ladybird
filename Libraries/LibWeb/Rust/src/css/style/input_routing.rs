/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Mapping normalized inputs to selector routing keys.
//!
//! The routing registry indexes immutable transpose routes by the semantic facts they read. A
//! normalized transaction input publishes the corresponding keys so planning can ask whether an
//! attached selector observes that input.

use super::index::FeatureValue;
use super::index::LocalFeatureKey;
use super::selector::RoutingKey;
use super::transaction::InputKey;
use super::transaction::InputValue;
use super::transaction::NormalizedInput;

/// The routing keys one normalized input publishes.
#[must_use]
pub(super) fn routing_keys_for_input(input: &NormalizedInput) -> Vec<RoutingKey> {
    let mut count = 0;
    for_each_routing_key(input, |_| count += 1);
    let mut keys = Vec::with_capacity(count);
    for_each_routing_key(input, |key| keys.push(key));
    keys
}

fn for_each_routing_key(input: &NormalizedInput, mut publish: impl FnMut(RoutingKey)) {
    match input.key {
        InputKey::LocalFeature(_, feature) => for_each_feature_routing_key(feature, input.old, input.new, publish),
        InputKey::State(_, fact) => publish(RoutingKey::State(fact)),
        _ => {}
    }
}

/// The routing keys one local-feature change publishes.
///
/// A tag or ID change publishes both its old and new atom, because a selector mentioning either can
/// change truth. A class or attribute key already names its atom. Attribute routes share the name
/// lookup, then classify presence and value truth from the selector node without another directory
/// probe.
fn for_each_feature_routing_key(
    feature: LocalFeatureKey,
    old: InputValue,
    new: InputValue,
    mut publish: impl FnMut(RoutingKey),
) {
    let atom_of = |value: InputValue| match value {
        InputValue::Feature(FeatureValue::Atom(atom)) => Some(atom),
        _ => None,
    };
    match feature {
        LocalFeatureKey::TagName | LocalFeatureKey::FoldedTagName => {
            if let Some(atom) = atom_of(old) {
                publish(RoutingKey::TagName(atom));
            }
            if let Some(atom) = atom_of(new) {
                publish(RoutingKey::TagName(atom));
            }
        }
        LocalFeatureKey::Id => {
            if let Some(atom) = atom_of(old) {
                publish(RoutingKey::Id(atom));
            }
            if let Some(atom) = atom_of(new) {
                publish(RoutingKey::Id(atom));
            }
        }
        LocalFeatureKey::Class(class) => publish(RoutingKey::Class(class)),
        LocalFeatureKey::Part(part) => publish(RoutingKey::Part(part)),
        LocalFeatureKey::CustomState(state) => publish(RoutingKey::CustomState(state)),
        // Routed to the element directly rather than through a transpose route.
        LocalFeatureKey::PartExposure => {}
        // Every `:lang()` entry registers under one key, because a range is not an atom.
        LocalFeatureKey::Language => publish(RoutingKey::Language),
        LocalFeatureKey::Directionality => {
            if let Some(atom) = atom_of(old) {
                publish(RoutingKey::Directionality(atom));
            }
            if let Some(atom) = atom_of(new) {
                publish(RoutingKey::Directionality(atom));
            }
        }
        LocalFeatureKey::HeadingLevel => publish(RoutingKey::Structural),
        LocalFeatureKey::Emptiness => publish(RoutingKey::Structural),
        // The facts an arrival folded onto one key are read back off the element by the engine,
        // which has the fact store this function does not.
        LocalFeatureKey::ArrivingFacts => {}
        LocalFeatureKey::Attribute(name) => publish(RoutingKey::AttributeName(name)),
    }
}
