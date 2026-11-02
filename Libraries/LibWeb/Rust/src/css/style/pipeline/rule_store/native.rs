/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::RuleID;
use crate::css::container_conditions::ContainerConditionsData;
use crate::css::style::RetainedState;
use std::hash::{Hash, Hasher};
use std::num::NonZeroU64;
use std::sync::Arc;

mod identities;
mod targets;
use identities::NativeRuleIdentities;
use targets::NativeRuleTargets;
pub(in crate::css::style) use targets::{NativeRuleTargetRef, TargetPages};

#[derive(Clone)]
pub(in crate::css::style) struct NativeRuleTarget {
    pub identity: NonZeroU64,
    pub source_identity: u64,
    conditions: Option<Box<NativeRuleConditions>>,
}

// NB: Every rule the host compiled has one. Its declarations are the block its rule holds.
const _: () = assert!(size_of::<NativeRuleTarget>() == 24);

#[derive(Clone)]
struct NativeRuleConditions {
    /// The qualified name of the layer, which every rule of the layer shares.
    layer_name: Arc<[u16]>,
    containers: Box<[Arc<ContainerConditionsData>]>,
}

#[derive(Clone, Default)]
pub(in crate::css::style) struct NativeRuleRegistry {
    pub targets: NativeRuleTargets,
    pub identities: NativeRuleIdentities,
}

impl PartialEq for NativeRuleTarget {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            identity,
            source_identity,
            conditions: _,
        } = self;
        *identity == other.identity
            && *source_identity == other.source_identity
            && self.layer_name() == other.layer_name()
            && self.containers().len() == other.containers().len()
            && self
                .containers()
                .iter()
                .zip(other.containers())
                .all(|(a, b)| Arc::ptr_eq(a, b))
    }
}

impl Eq for NativeRuleTarget {}

impl Hash for NativeRuleTarget {
    fn hash<H: Hasher>(&self, hasher: &mut H) {
        let Self {
            identity,
            source_identity,
            conditions: _,
        } = self;
        identity.hash(hasher);
        source_identity.hash(hasher);
        self.layer_name().hash(hasher);
        for container in self.containers() {
            Arc::as_ptr(container).hash(hasher);
        }
    }
}

impl NativeRuleTarget {
    pub fn layer_name(&self) -> &[u16] {
        self.conditions
            .as_ref()
            .map_or(&[], |conditions| &conditions.layer_name)
    }

    pub fn containers(&self) -> &[Arc<ContainerConditionsData>] {
        self.conditions
            .as_ref()
            .map_or(&[], |conditions| &conditions.containers)
    }
}

impl NativeRuleRegistry {
    pub fn remove(&mut self, id: RuleID) {
        if let Some(target) = self.targets.remove(&id) {
            let identity = target.identity.get();
            if self.identities.get(&identity) == Some(id) {
                self.identities.remove(&identity);
            }
        }
        self.reconcile_memory();
    }

    fn reconcile_memory(&mut self) {}
}

impl RetainedState {
    /// The identity of the native sheet a rule was compiled from: an imported sheet's own, not the
    /// importing sheet's.
    pub(crate) fn rule_source_identity(&self, id: RuleID) -> Option<u64> {
        self.rules.native_target(id).map(|target| target.source_identity)
    }

    pub(crate) fn native_rule_id(&self, identity: u64) -> Option<RuleID> {
        self.rules
            .native
            .identities
            .get(&identity)
            .or_else(|| self.rules.user_agent_rule_of_identity(identity))
    }

    /// Register immutable cascade inputs independently of document-local rule owners, with the container conditions
    /// the rule is inside, innermost first.
    pub(crate) fn register_native_rule(
        &mut self,
        id: RuleID,
        identity: u64,
        source_identity: u64,
        layer_name: Arc<[u16]>,
        containers: Box<[Arc<ContainerConditionsData>]>,
    ) {
        // Whole-sheet replacement can reuse the semantic ID with a new native rule. Retire the
        // old reverse lookup before installing the replacement; detached CSSOM rules keep their
        // native identity but no longer name a rule in this engine.
        self.rules.native.remove(id);
        let target = NativeRuleTarget {
            identity: NonZeroU64::new(identity).expect("native rule identities are nonzero"),
            source_identity,
            conditions: (!layer_name.is_empty() || !containers.is_empty())
                .then(|| Box::new(NativeRuleConditions { layer_name, containers })),
        };
        self.rules.native.identities.insert(identity, id);
        self.rules.native.targets.insert(id, target);
        self.rules.native.reconcile_memory();
    }
}

#[cfg(test)]
mod tests {
    use crate::css::media_list::MediaList;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::rule::{rust_rule_identity, rust_rule_list_clear};
    use crate::css::style::StyleEngine;
    use crate::css::style::pipeline::rule_store::{CascadeOrigin, RuleKind};
    use crate::css::style_sheet::NativeStyleSheet;
    use std::rc::Rc;

    fn source() -> Rc<NativeStyleSheet> {
        let units: Vec<_> = "@function --f() { result: 1px; }".encode_utf16().collect();
        let view = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
        // All context fields are booleans, integers, or nullable pointers.
        let context = ParseContext::default();
        let parsed = parse_shared_stylesheet(view, Some(&context));
        let rules = crate::css::rule::NativeRuleList::from_parsed(parsed);
        let media = MediaList::new(Default::default());
        NativeStyleSheet::new(rules, media)
    }

    #[test]
    fn native_rule_inputs_do_not_retain_document_local_sheets_or_rules() {
        let source = source();
        let rule = source.rules().rule_at(0);
        let identity = rust_rule_identity(&rule);
        let source_weak = Rc::downgrade(&source);
        let source_identity = source.identity();
        let rule_weak = Rc::downgrade(&rule);
        let mut engines = [StyleEngine::new(), StyleEngine::new()];
        let mut ids = Vec::new();
        for (index, engine) in engines.iter_mut().enumerate() {
            let sheet = engine.add_sheet(CascadeOrigin::Author);
            for _ in 0..index {
                engine.add_non_matching_rule(sheet, None, RuleKind::Function);
            }
            let id = engine.add_non_matching_rule(sheet, None, RuleKind::Function);
            engine.register_native_rule(
                id,
                identity,
                source.identity(),
                std::sync::Arc::default(),
                Box::default(),
            );
            assert_eq!(engine.native_rule_id(identity), Some(id));
            ids.push(id);
            assert_eq!(
                engine.rules.native.targets.get(&id).unwrap().source_identity,
                source_identity
            );
        }
        assert_ne!(ids[0], ids[1]);
        drop(rule);
        drop(source);
        assert!(rule_weak.upgrade().is_none());
        assert!(source_weak.upgrade().is_none());
        let replacement = self::source();
        assert_ne!(replacement.identity(), source_identity);
        assert_eq!(
            engines[1].rules.native.targets.get(&ids[1]).unwrap().source_identity,
            source_identity
        );
        engines[0].remove_style_rule(ids[0]);
        assert!(engines[0].rules.native.identities.is_empty());
        assert!(rule_weak.upgrade().is_none());
        assert!(source_weak.upgrade().is_none());
        assert_eq!(engines[1].rules.native.identities.get(&identity), Some(ids[1]));
        engines[1].remove_style_rule(ids[1]);
        assert!(engines[1].rules.native.identities.is_empty());
        assert!(rule_weak.upgrade().is_none());
        assert!(source_weak.upgrade().is_none());
    }

    #[test]
    fn declaration_edits_resolve_native_owners_and_use_engine_revisions() {
        use crate::css::style::rule_writes::publish_native_rule_declarations;
        let source = source();
        let rule = &source.rules().rule_at(0);
        let children = rule.child_rules().unwrap();
        let child = children.rule_at(0);
        let mut engine = StyleEngine::new();
        let sheet = engine.add_sheet(CascadeOrigin::Author);
        let id = engine.add_non_matching_rule(sheet, None, RuleKind::Function);
        engine.register_native_rule(
            id,
            rust_rule_identity(rule),
            source.identity(),
            std::sync::Arc::default(),
            Box::default(),
        );
        let mut notifications = Vec::<u32>::new();
        // What the engine does with the host's write: it publishes the edit through the rule that owns the declarations.
        let declarations_changed = |engine: &mut StyleEngine, notifications: &mut Vec<u32>| {
            if let Some(owner) = child
                .declaration_owner_identity()
                .and_then(|identity| engine.native_rule_id(identity))
            {
                notifications.push(owner.0 + 1);
                publish_native_rule_declarations(engine, child.cascade_declarations(), owner);
            }
        };
        let initial = engine.current_rule_version(id).declaration_block;
        declarations_changed(&mut engine, &mut notifications);
        let first = engine.current_rule_version(id).declaration_block;
        assert_ne!(initial, first);
        // Inline declarations and whole-sheet replacement use the same revision issuer.
        engine.next_declaration_block_version();
        declarations_changed(&mut engine, &mut notifications);
        let second = engine.current_rule_version(id).declaration_block;
        assert_ne!(first, second);
        assert_eq!(notifications, [id.0 + 1, id.0 + 1]);
        rust_rule_list_clear(children);
        declarations_changed(&mut engine, &mut notifications);
        assert_eq!(engine.current_rule_version(id).declaration_block, second);
        assert_eq!(notifications.len(), 2);
    }

    #[test]
    fn replacing_a_native_owner_retires_the_old_reverse_lookup() {
        let old_source = source();
        let new_source = source();
        let old_rule = &old_source.rules().rule_at(0);
        let new_rule = &new_source.rules().rule_at(0);
        let mut engine = StyleEngine::new();
        let sheet = engine.add_sheet(CascadeOrigin::Author);
        let id = engine.add_non_matching_rule(sheet, None, RuleKind::Function);
        engine.register_native_rule(
            id,
            rust_rule_identity(old_rule),
            old_source.identity(),
            std::sync::Arc::default(),
            Box::default(),
        );
        engine.register_native_rule(
            id,
            rust_rule_identity(new_rule),
            new_source.identity(),
            std::sync::Arc::default(),
            Box::default(),
        );
        assert!(
            !engine
                .rules
                .native
                .identities
                .contains_key(&rust_rule_identity(old_rule))
        );
        assert_eq!(
            engine.rules.native.identities.get(&rust_rule_identity(new_rule)),
            Some(id)
        );
        assert_eq!(engine.rules.native.targets.len(), 1);
    }
}
