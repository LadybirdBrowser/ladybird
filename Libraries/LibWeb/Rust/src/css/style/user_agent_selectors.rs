/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The user-agent sheets are the same objects in every document of the process, so the selectors
//! of their rules are compiled once per process. Every document that records the sheets attaches
//! the programs the first one compiled.

use super::compiler::ScopeChain;
use super::inputs::compile_selector_program;
use super::rule_writes::NamespaceTexts;
use super::selector::ProcessSelectorProgram;
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
            let UserAgentSelectorPrograms { atoms, programs } = &mut *cache;
            // UADBG
            if programs.contains_key(&key) {
                uadbg::hit(
                    &self.retained.counters,
                    key.fold_id_and_class_name_case,
                    key.html_element_namespace.0,
                );
            } else {
                let same_set = programs
                    .keys()
                    .filter(|other| {
                        other.fold_id_and_class_name_case == key.fold_id_and_class_name_case
                            && other.html_element_namespace == key.html_element_namespace
                    })
                    .count();
                let same_rule: Vec<_> = programs
                    .keys()
                    .filter(|other| other.rule == key.rule)
                    .map(|other| (other.fold_id_and_class_name_case, other.html_element_namespace.0))
                    .collect();
                uadbg::miss(
                    &self.retained.counters,
                    format!(
                        "rule={} fold={} ns={} sheet={} cache={} same_set={same_set} same_rule={same_rule:?} \
                         selectors={} first={:.300}",
                        key.rule,
                        key.fold_id_and_class_name_case,
                        key.html_element_namespace.0,
                        sheet.0,
                        programs.len(),
                        selectors.len(),
                        format!("{:?}", selectors.first())
                    ),
                );
            }
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
}

/// UADBG: Temporary instrumentation for the flake in css/user-agent-selectors-compile-once-per-process.html. Each
/// engine keeps a short log of what it did with its sheets and selectors, keyed by the address of its counters, and
/// prints it when its counters are read after it compiled selectors it should have shared.
pub(crate) mod uadbg {
    use super::super::instrumentation::{Counter, Counters};
    use std::collections::HashMap;
    use std::sync::{Mutex, MutexGuard, OnceLock};

    const MAX_EVENTS: usize = 256;

    #[derive(Default)]
    struct EngineLog {
        ua_hits: u64,
        hits_by_key: HashMap<(bool, u32), u64>,
        ua_misses: u64,
        non_author_compiles: u64,
        author_compiles: u64,
        author_sheets: u32,
        events: Vec<String>,
    }

    fn logs() -> MutexGuard<'static, HashMap<usize, EngineLog>> {
        static LOGS: OnceLock<Mutex<HashMap<usize, EngineLog>>> = OnceLock::new();
        LOGS.get_or_init(Default::default).lock().unwrap()
    }

    fn engine(counters: &Counters) -> usize {
        counters as *const Counters as usize
    }

    fn push(log: &mut EngineLog, event: String) {
        if log.events.len() < MAX_EVENTS {
            log.events.push(event);
        }
    }

    pub(crate) fn engine_starts(counters: &Counters) {
        logs().remove(&engine(counters));
    }

    pub(crate) fn event(counters: &Counters, event: String) {
        push(logs().entry(engine(counters)).or_default(), event);
    }

    pub(crate) fn sheet_added(counters: &Counters, sheet: u32, origin: u8) {
        let mut logs = logs();
        let log = logs.entry(engine(counters)).or_default();
        if origin == 0 {
            log.author_sheets += 1;
        }
        push(log, format!("add-sheet {sheet} origin={origin}"));
    }

    pub(crate) fn hit(counters: &Counters, fold: bool, namespace: u32) {
        let mut logs = logs();
        let log = logs.entry(engine(counters)).or_default();
        log.ua_hits += 1;
        *log.hits_by_key.entry((fold, namespace)).or_default() += 1;
    }

    pub(crate) fn miss(counters: &Counters, description: String) {
        let mut logs = logs();
        let log = logs.entry(engine(counters)).or_default();
        log.ua_misses += 1;
        let event = if log.ua_hits > 0 {
            format!(
                "ua-miss after {} hits: {description}\n{}",
                log.ua_hits,
                std::backtrace::Backtrace::force_capture()
            )
        } else {
            format!("ua-miss: {description}")
        };
        push(log, event);
    }

    pub(crate) fn compile(counters: &Counters, author: bool, description: String) {
        let mut logs = logs();
        let log = logs.entry(engine(counters)).or_default();
        if author {
            log.author_compiles += 1;
        } else {
            log.non_author_compiles += 1;
        }
        let event = if log.ua_hits > 0 && (!author || log.author_sheets == 0) {
            format!(
                "compile after {} hits: {description}\n{}",
                log.ua_hits,
                std::backtrace::Backtrace::force_capture()
            )
        } else {
            format!("compile: {description}")
        };
        push(log, event);
    }

    pub(crate) fn dump(counters: &Counters, fold: bool, namespace: u32) {
        let entries = counters.get(Counter::ExactSelectorEntries);
        let logs = logs();
        let Some(log) = logs.get(&engine(counters)) else {
            return;
        };
        let suspicious = entries > 0
            && log.ua_hits > 0
            && (log.ua_misses > 0 || log.non_author_compiles > 0 || log.author_sheets == 0);
        if !suspicious {
            return;
        }
        let pid = std::process::id();
        let id = engine(counters);
        eprintln!(
            "UADBG pid={pid} engine={id:#x} exactSelectorEntries={entries} fold={fold} ns={namespace} ua_hits={} \
             hits_by_key={:?} ua_misses={} non_author_compiles={} author_compiles={} author_sheets={} events={}",
            log.ua_hits,
            log.hits_by_key,
            log.ua_misses,
            log.non_author_compiles,
            log.author_compiles,
            log.author_sheets,
            log.events.len()
        );
        for event in &log.events {
            for line in event.lines() {
                eprintln!("UADBG pid={pid} engine={id:#x}   {line}");
            }
        }
    }
}
