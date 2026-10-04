/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The user-agent sheets are the same objects in every document of the process, so the selectors
//! of their rules are compiled once per process. Every document that records the sheets attaches
//! the programs the first one compiled.

use super::compiler::{NamespaceScope, ScopeChain};
use super::inputs::compile_selector_program;
use super::selector::ProcessSelectorProgram;
use super::*;
use crate::css::rule::NativeRuleList;
use std::collections::hash_map::Entry;
use std::sync::{MutexGuard, OnceLock};

/// What a user-agent rule's selector program is compiled from. The rule's identity names its
/// selectors: no other rule ever takes it, and a user-agent rule is never edited. The rest is what
/// the compiler reads from the document.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct CompilationKey {
    rule: u64,
    fold_id_and_class_name_case: bool,
    html_element_namespace: StyleAtomID,
}

struct UserAgentSelectorPrograms {
    /// Holds every atom the programs name for as long as the process lives, so that none of them
    /// is ever given to another name.
    atoms: DocumentAtoms,
    programs: HashMap<CompilationKey, ProcessSelectorProgram>,
}

fn user_agent_selector_programs() -> MutexGuard<'static, UserAgentSelectorPrograms> {
    static PROGRAMS: OnceLock<Mutex<UserAgentSelectorPrograms>> = OnceLock::new();
    PROGRAMS
        .get_or_init(|| {
            Mutex::new(UserAgentSelectorPrograms {
                atoms: DocumentAtoms::for_live_engine(),
                programs: HashMap::default(),
            })
        })
        .lock()
        .unwrap()
}

impl StyleEngineState {
    /// Whether this engine attaches the selector programs the process compiled for the user-agent
    /// sheets. One that records compiles its own, so the recording sees each program arrive; one
    /// whose atoms are its own cannot read programs that name the process's.
    pub(super) fn shares_user_agent_selector_programs(&self) -> bool {
        #[cfg(feature = "style-recording")]
        if self.host.recording_id.is_some() {
            return false;
        }
        self.atoms.is_process_global()
    }

    /// Add a style rule of a user-agent sheet, compiling its selectors only if no document in the
    /// process has compiled them with the same inputs before.
    pub(super) fn add_user_agent_style_rule(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        rule_identity: u64,
        selectors: &[&CompiledSelector],
        rules: &NativeRuleList,
        counters: &mut Counters,
    ) -> RuleID {
        debug_assert!(self.program.sheet_origin(sheet) == CascadeOrigin::UserAgent);
        let key = CompilationKey {
            rule: rule_identity,
            fold_id_and_class_name_case: self.fold_id_and_class_name_case,
            html_element_namespace: self.html_element_namespace,
        };
        let program = {
            let mut cache = user_agent_selector_programs();
            let UserAgentSelectorPrograms { atoms, programs } = &mut *cache;
            match programs.entry(key) {
                Entry::Occupied(entry) => entry.get().clone(),
                Entry::Vacant(entry) => {
                    let namespaces = NamespaceScope::from_rule_list(rules, |text| {
                        atoms.intern_raw(ak::Utf16FlyString::from_utf16(text).raw_identity())
                    });
                    let compiled = compile_selector_program(
                        atoms,
                        key.fold_id_and_class_name_case,
                        key.html_element_namespace,
                        selectors,
                        namespaces,
                        &ScopeChain::default(),
                        counters,
                    );
                    entry.insert(ProcessSelectorProgram::share(compiled)).clone()
                }
            }
        };
        self.add_style_rule_with(sheet, before, counters, |engine, previous_program, _| {
            engine.note_attribute_value_text_names(program.program());
            let id = engine.retained.programs.add_process_program(&program);
            engine.retained.selector_programs_need_sweep |= previous_program.is_some();
            engine.retained.programs.settle_memory(&mut engine.retained.memory);
            id
        })
    }
}
