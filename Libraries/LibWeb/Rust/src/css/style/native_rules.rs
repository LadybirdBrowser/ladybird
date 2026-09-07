/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::capacity::ShallowCapacityBytes;
use super::memory::{MemoryCategory, MemoryController, MemoryLease};
use super::{HashMap, RuleID, StyleEngine};
use crate::css::container_conditions::ContainerConditionsData;
use crate::css::declaration_block::DeclarationBlockData;
use std::sync::Arc;

pub(super) struct NativeRuleTarget {
    pub identity: u64,
    pub declarations: Option<Arc<DeclarationBlockData>>,
    pub source_identity: u64,
    pub layer_name: Box<[u16]>,
    pub containers: Vec<Arc<ContainerConditionsData>>,
}

pub(super) struct NativeRuleRegistry {
    pub targets: HashMap<RuleID, NativeRuleTarget>,
    pub identities: HashMap<u64, RuleID>,
    owned_bytes: u64,
    memory: MemoryLease,
}

impl Default for NativeRuleRegistry {
    fn default() -> Self {
        Self {
            targets: HashMap::default(),
            identities: HashMap::default(),
            owned_bytes: 0,
            memory: MemoryLease::new(MemoryCategory::RuleProgram),
        }
    }
}

impl NativeRuleTarget {
    fn owned_bytes(&self) -> u64 {
        // The parsed graph is authoritative input. Charge only this owner's additional storage.
        self.layer_name.shallow_capacity_bytes() + self.containers.shallow_capacity_bytes()
    }
}

impl NativeRuleRegistry {
    pub fn remove(&mut self, id: RuleID, memory: &mut MemoryController) {
        if let Some(target) = self.targets.remove(&id) {
            self.owned_bytes -= target.owned_bytes();
            let identity = target.identity;
            if self.identities.get(&identity) == Some(&id) {
                self.identities.remove(&identity);
            }
        }
        self.reconcile_memory(memory);
    }

    fn reconcile_memory(&mut self, memory: &mut MemoryController) {
        self.memory.resize_required_to(
            memory,
            self.owned_bytes + self.targets.shallow_capacity_bytes() + self.identities.shallow_capacity_bytes(),
        );
    }
}

impl StyleEngine {
    pub(crate) fn native_rule_id(&self, identity: u64) -> Option<RuleID> {
        self.native_rules.identities.get(&identity).copied()
    }

    /// Register immutable cascade inputs independently of document-local rule owners.
    ///
    /// # Safety
    /// Containers must belong to live Arc allocations.
    pub(crate) unsafe fn register_native_rule(
        &mut self,
        id: RuleID,
        identity: u64,
        declarations: Option<Arc<DeclarationBlockData>>,
        source_identity: u64,
        layer_name: &[u16],
        containers: &[*const ContainerConditionsData],
    ) {
        // Whole-sheet replacement can reuse the semantic ID with a new native rule. Retire the
        // old reverse lookup before installing the replacement; detached CSSOM rules keep their
        // native identity but no longer name a rule in this engine.
        self.native_rules.remove(id, &mut self.memory);
        let target = unsafe {
            NativeRuleTarget {
                identity,
                declarations,
                source_identity,
                layer_name: layer_name.into(),
                containers: containers
                    .iter()
                    .rev()
                    .map(|&container| {
                        Arc::increment_strong_count(container);
                        Arc::from_raw(container)
                    })
                    .collect(),
            }
        };
        self.native_rules.owned_bytes += target.owned_bytes();
        self.native_rules.identities.insert(identity, id);
        self.native_rules.targets.insert(id, target);
        self.native_rules.reconcile_memory(&mut self.memory);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::media_list::MediaList;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::rule::{rust_rule_identity, rust_rule_list_at, rust_rule_list_clear, rust_rule_retain};
    use crate::css::style::bridge::style_engine_native_rule_id;
    use crate::css::style::memory::DeviceClass;
    use crate::css::style::program::{CascadeOrigin, RuleKind, StyleSheetObjectID};
    use crate::css::style_sheet::NativeStyleSheet;
    use std::rc::Rc;

    fn source() -> Rc<NativeStyleSheet> {
        let units: Vec<_> = "@function --f() { result: 1px; }".encode_utf16().collect();
        let view = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
        // All context fields are booleans, integers, or nullable pointers.
        let context: ParseContext = unsafe { std::mem::zeroed() };
        let parsed = unsafe { parse_shared_stylesheet(view, &context) };
        let rules = crate::css::rule::NativeRuleList::from_parsed(parsed);
        let media = MediaList::new(Default::default());
        NativeStyleSheet::new(rules, media)
    }

    #[test]
    fn native_rule_inputs_do_not_retain_document_local_sheets_or_rules() {
        let source = source();
        let rule = unsafe { Rc::from_raw(rust_rule_retain(rust_rule_list_at(source.rules(), 0))) };
        let identity = rust_rule_identity(&rule);
        let source_weak = Rc::downgrade(&source);
        let source_identity = source.identity();
        let rule_weak = Rc::downgrade(&rule);
        let mut engines = [
            StyleEngine::new(DeviceClass::ForegroundDesktop),
            StyleEngine::new(DeviceClass::ForegroundDesktop),
        ];
        let mut ids = Vec::new();
        for (index, engine) in engines.iter_mut().enumerate() {
            let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
            for _ in 0..index {
                engine.add_non_matching_rule(sheet, None, RuleKind::Function);
            }
            let id = engine.add_non_matching_rule(sheet, None, RuleKind::Function);
            unsafe {
                engine.register_native_rule(id, identity, rule.cascade_declarations(), source.identity(), &[], &[])
            };
            assert_eq!(
                unsafe { style_engine_native_rule_id(std::ptr::from_ref(engine).cast(), identity) },
                id.0 + 1
            );
            ids.push(id);
            assert_eq!(engine.native_rules.targets[&id].source_identity, source_identity);
        }
        assert_ne!(ids[0], ids[1]);
        drop(rule);
        drop(source);
        assert!(rule_weak.upgrade().is_none());
        assert!(source_weak.upgrade().is_none());
        let replacement = self::source();
        assert_ne!(replacement.identity(), source_identity);
        assert_eq!(
            engines[1].native_rules.targets[&ids[1]].source_identity,
            source_identity
        );
        engines[0].remove_style_rule(ids[0]);
        assert!(engines[0].native_rules.identities.is_empty());
        assert!(rule_weak.upgrade().is_none());
        assert!(source_weak.upgrade().is_none());
        assert_eq!(engines[1].native_rules.identities.get(&identity), Some(&ids[1]));
        engines[1].remove_style_rule(ids[1]);
        assert!(engines[1].native_rules.identities.is_empty());
        assert!(rule_weak.upgrade().is_none());
        assert!(source_weak.upgrade().is_none());
    }

    #[test]
    fn declaration_edits_resolve_native_owners_and_use_engine_revisions() {
        use crate::css::rule::rust_rule_children;
        use crate::css::style::bridge::style_engine_native_rule_declarations_changed;
        let source = source();
        let rule = unsafe { &*rust_rule_list_at(source.rules(), 0) };
        let children = unsafe { &*rust_rule_children(rule) };
        let child = unsafe { Rc::from_raw(rust_rule_retain(rust_rule_list_at(children, 0))) };
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        let id = engine.add_non_matching_rule(sheet, None, RuleKind::Function);
        unsafe {
            engine.register_native_rule(
                id,
                rust_rule_identity(rule),
                rule.cascade_declarations(),
                source.identity(),
                &[],
                &[],
            )
        };
        let mut notifications = Vec::<u32>::new();
        unsafe extern "C" fn notify(context: *mut std::ffi::c_void, rule: u32) {
            unsafe { &mut *context.cast::<Vec<u32>>() }.push(rule);
        }
        let initial = engine.current_rule_version(id).declaration_block;
        unsafe {
            style_engine_native_rule_declarations_changed(
                (&raw mut engine).cast(),
                Rc::as_ptr(&child).cast(),
                (&raw mut notifications).cast(),
                notify,
            );
        }
        let first = engine.current_rule_version(id).declaration_block;
        assert_ne!(initial, first);
        // Inline declarations and whole-sheet replacement use the same revision issuer.
        engine.next_declaration_block_version();
        unsafe {
            style_engine_native_rule_declarations_changed(
                (&raw mut engine).cast(),
                Rc::as_ptr(&child).cast(),
                (&raw mut notifications).cast(),
                notify,
            );
        }
        let second = engine.current_rule_version(id).declaration_block;
        assert_ne!(first, second);
        assert_eq!(notifications, [id.0 + 1, id.0 + 1]);
        rust_rule_list_clear(children);
        unsafe {
            style_engine_native_rule_declarations_changed(
                (&raw mut engine).cast(),
                Rc::as_ptr(&child).cast(),
                (&raw mut notifications).cast(),
                notify,
            );
        }
        assert_eq!(engine.current_rule_version(id).declaration_block, second);
        assert_eq!(notifications.len(), 2);
    }

    #[test]
    fn replacing_a_native_owner_retires_the_old_reverse_lookup() {
        let old_source = source();
        let new_source = source();
        let old_rule = unsafe { &*rust_rule_list_at(old_source.rules(), 0) };
        let new_rule = unsafe { &*rust_rule_list_at(new_source.rules(), 0) };
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        let id = engine.add_non_matching_rule(sheet, None, RuleKind::Function);
        unsafe {
            engine.register_native_rule(
                id,
                rust_rule_identity(old_rule),
                old_rule.cascade_declarations(),
                old_source.identity(),
                &[],
                &[],
            );
            engine.register_native_rule(
                id,
                rust_rule_identity(new_rule),
                new_rule.cascade_declarations(),
                new_source.identity(),
                &[],
                &[],
            );
        }
        assert!(
            !engine
                .native_rules
                .identities
                .contains_key(&rust_rule_identity(old_rule))
        );
        assert_eq!(
            engine.native_rules.identities.get(&rust_rule_identity(new_rule)),
            Some(&id)
        );
        assert_eq!(engine.native_rules.targets.len(), 1);
    }
}
