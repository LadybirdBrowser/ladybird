/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The user-agent sheets are the same objects in every document of the process, so the selectors
//! of their rules are compiled once per process. Every document that records the sheets attaches
//! the programs the first one compiled, and the routing registry its rules make.

use super::compiler::ScopeChain;
use super::inputs::compile_selector_program;
use super::rule_writes::NamespaceTexts;
use super::selector::{ProcessSelectorProgram, RoutingRegistry, SelectorEntryIDs, SharedSelectorIdentity};
use super::*;
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
    routings: Vec<UserAgentRouting>,
}

/// A rule of a user-agent sheet as its routes name it: by the engine's identities of the rule, of
/// its program and of the program's entries, and by the program the process shares.
#[derive(PartialEq, Eq)]
struct UserAgentRoute {
    rule: RuleID,
    program: SelectorProgramID,
    shared: SharedSelectorIdentity,
    entries: SelectorEntryIDs,
}

/// The routing registry a user-agent sheet's rules make, routed in order into the registry
/// `after` names: an empty one, or by its index the one another sheet's rules made. Routing is a
/// function of what is routed into what, so every engine that routes the same rules the same way
/// makes this registry.
struct UserAgentRouting {
    after: Option<usize>,
    routes: Vec<UserAgentRoute>,
    routing: RoutingRegistry,
}

/// How many routings the process keeps: one per set of user-agent sheets a document records,
/// which quirks mode, the HTML namespace and MathML or SVG content decide.
const MAX_USER_AGENT_ROUTINGS: usize = 16;

fn user_agent_selector_programs() -> MutexGuard<'static, UserAgentSelectorPrograms> {
    static PROGRAMS: OnceLock<Mutex<UserAgentSelectorPrograms>> = OnceLock::new();
    PROGRAMS
        .get_or_init(|| {
            Mutex::new(UserAgentSelectorPrograms {
                atoms: DocumentAtoms::for_live_engine(),
                programs: HashMap::default(),
                routings: Vec::new(),
            })
        })
        .lock()
        .unwrap()
}

impl StyleEngine {
    /// Whether this engine attaches the selector programs the process compiled for the user-agent
    /// sheets. One whose atoms are its own cannot read programs that name the process's.
    pub(super) fn shares_user_agent_selector_programs(&self) -> bool {
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
        namespaces: Option<&NamespaceTexts>,
    ) -> RuleID {
        debug_assert!(self.program.sheet_origin(sheet) == CascadeOrigin::UserAgent);
        let key = CompilationKey {
            rule: rule_identity,
            fold_id_and_class_name_case: self.fold_id_and_class_name_case,
            html_element_namespace: self.html_element_namespace,
        };
        let program = {
            let mut cache = user_agent_selector_programs();
            let UserAgentSelectorPrograms { atoms, programs, .. } = &mut *cache;
            match programs.entry(key) {
                Entry::Occupied(entry) => entry.get().clone(),
                Entry::Vacant(entry) => {
                    let namespaces = NamespaceTexts::scope(namespaces, |text| {
                        atoms.intern_raw(ak::Utf16FlyString::from_utf16(text).raw_identity())
                    });
                    let compiled = compile_selector_program(
                        atoms,
                        key.fold_id_and_class_name_case,
                        key.html_element_namespace,
                        selectors,
                        namespaces,
                        &ScopeChain::default(),
                        &self.retained.counters,
                    );
                    entry.insert(ProcessSelectorProgram::share(compiled)).clone()
                }
            }
        };
        self.add_style_rule_with(sheet, before, |engine, previous_program| {
            engine.note_selector_attribute_names(program.program());
            engine.note_relational_arrival_keys(program.program());
            let id = engine.retained.programs.add_process_program(&program);
            engine.retained.selector_programs_need_sweep |= previous_program.is_some();

            id
        })
    }

    /// Publish the rules of the sheet `sheet` with `publish`. A user-agent sheet's rules are
    /// routed together as the publication ends, so that an engine whose routing holds what
    /// another document's held when it routed them takes a copy of the registry they made.
    pub(super) fn publish_sheet_rules(&mut self, sheet: u32, publish: impl FnOnce(&mut Self)) {
        let user_agent = sheet
            .checked_sub(1)
            .is_some_and(|sheet| self.program.sheet_origin(SheetID(sheet)) == CascadeOrigin::UserAgent);
        if !user_agent || !self.shares_user_agent_selector_programs() {
            publish(self);
            return;
        }
        debug_assert!(self.retained.deferred_user_agent_routes.is_none());
        self.retained.deferred_user_agent_routes = Some(Vec::new());
        publish(self);
        let deferred = self.retained.deferred_user_agent_routes.take().unwrap();
        self.route_user_agent_rules(&deferred);
    }

    fn route_user_agent_rules(&mut self, deferred: &[(RuleID, SelectorProgramID)]) {
        if deferred.is_empty() {
            return;
        }
        let routes = deferred
            .iter()
            .map(|&(rule, program)| {
                let (shared, entries) = self.programs.shared_dispatch_identity(program)?;
                Some(UserAgentRoute {
                    rule,
                    program,
                    shared,
                    entries,
                })
            })
            .collect::<Option<Vec<_>>>();
        let after = {
            let cache = user_agent_selector_programs();
            let after = match self.routing.is_empty() {
                true => Some(None),
                false => cache
                    .routings
                    .iter()
                    .position(|made| self.routing.is_finished_copy_of(&made.routing))
                    .map(Some),
            };
            if let (Some(routes), Some(after)) = (&routes, after)
                && let Some(made) = cache
                    .routings
                    .iter()
                    .find(|made| made.after == after && made.routes == *routes)
            {
                let routing = made.routing.copy_for_another_engine();
                drop(cache);
                self.retained.routing = Arc::new(routing);
                return;
            }
            after.filter(|_| cache.routings.len() < MAX_USER_AGENT_ROUTINGS)
        };

        let retained = &mut self.retained;
        let routing = Arc::make_mut(&mut retained.routing);
        for &(rule, program) in deferred {
            routing.add_rule(rule, program, &retained.programs);
        }
        let (Some(routes), Some(after)) = (routes, after) else {
            return;
        };
        // Finish this engine's routing as well as the process's copy of it, so that the next user-agent sheet this
        // engine records finds its routing as the one these rules made.
        routing.finish_for_sharing();
        let made = routing.copy_for_another_engine();
        let mut cache = user_agent_selector_programs();
        if cache.routings.len() < MAX_USER_AGENT_ROUTINGS
            && !cache
                .routings
                .iter()
                .any(|known| known.after == after && known.routes == routes)
        {
            cache.routings.push(UserAgentRouting {
                after,
                routes,
                routing: made,
            });
        }
    }
}
