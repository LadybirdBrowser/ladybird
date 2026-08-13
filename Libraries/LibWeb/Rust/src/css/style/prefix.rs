/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Top-down state for ordinary selector chains over all four tree axes.
//!
//! A selector such as `.a > .b .c` is a small match automaton. Matching `.a` enables the `.b`
//! child step for one edge; matching `.b` enables the `.c` descendant step until that step matches.
//! A preorder traversal can propagate those enabled steps once instead of making every `.c`
//! candidate rediscover the same prefix by walking left through the tree.
//!
//! Sibling combinators are the same machine turned right. `.a + .b ~ .c` enables the `.b` step at
//! exactly the next sibling and the `.c` step at every later one, so a node's transition reads two
//! entering states - the parent's downward state and the previous sibling's rightward state - and
//! produces two output states in the same interned space. A following step persists along a child
//! sequence exactly as a descendant step persists down the tree, so the state representation, the
//! interner, and the retained form serve both axes unchanged.

use super::capacity::capacity_bytes;
use super::fast_hash::FastMap as HashMap;
use super::fast_hash::fast_hasher;
use std::hash::Hash;
use std::hash::Hasher;
use std::mem::size_of;
use std::num::NonZeroU32;

use super::ScopeProgramID;
use super::column::Column;
use super::column::EpochColumn;
use super::column::advance_epoch;
use super::index::DispatchEntryID;
use super::index::DispatchKey;
use super::index::StyleNodeFacts;
use super::instrumentation::Counter;
use super::instrumentation::Counters;
use super::memory::CapacityGuard;
use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::memory::MemoryLease;
use super::partial_view::Lookup;
use super::program::SelectorProgramID;
use super::selector::AttributeOperator;
use super::selector::FeatureTest;
use super::selector::Incomplete;
use super::selector::MatchEvaluator;
use super::selector::NamespaceTest;
use super::selector::PrefixStructuralTest;
use super::selector::SelectorPrefixAxis;
use super::selector::SelectorPrefixLocal;
use super::selector::SelectorPrefixStep;
use super::selector::SelectorPrograms;
use super::tree::StyleNodeID;
use super::tree::StyleNodeTree;

define_id! { struct PrefixCompoundID(); }

define_id! { pub(super) struct PrefixStepID(); }

#[derive(Clone, Copy)]
pub(super) struct PrefixProducer {
    pub step: PrefixStepID,
}

impl PrefixProducer {
    pub(super) fn index(self) -> usize {
        self.step.0 as usize
    }
}

define_id! { default pub(super) struct PrefixMatchSetID(); }

impl PrefixMatchSetID {
    pub(super) fn index(self) -> usize {
        self.0 as usize
    }
}

impl super::intern_table::InternIdentity for PrefixMatchSetID {
    fn index(self) -> usize {
        self.0 as usize - 1
    }
}

define_id! { default struct PrefixTruthSetID(); }

impl super::intern_table::InternIdentity for PrefixTruthSetID {
    fn index(self) -> usize {
        self.0 as usize - 1
    }
}

define_id! { default struct PrefixResultID(); }

impl super::intern_table::InternIdentity for PrefixResultID {
    fn index(self) -> usize {
        self.0 as usize - 1
    }
}

#[derive(Clone, Copy)]
struct PrefixResult {
    matches: PrefixMatchSetID,
    truth: PrefixTruthSetID,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct PrefixStepKey {
    compound: PrefixCompoundID,
    predecessor: Option<PrefixStepID>,
    axis: SelectorPrefixAxis,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
enum PrefixPredicateKey {
    /// A canonical compound: interned feature tests answered from the fact row, plus the
    /// structural tests it requires, answered from the node's precomputed truth bits. Keeping
    /// the structural part as a bit mask rather than a program reference lets equal shapes
    /// share one compound across programs and keeps the match evaluator out of transitions.
    Features {
        features: Box<[FeatureTest]>,
        required_positional_bits: u32,
    },
    Program {
        program: SelectorProgramID,
        local: SelectorPrefixLocal,
    },
}

#[derive(Clone)]
enum PrefixPredicate {
    Features {
        feature_start: u32,
        feature_len: u32,
        required_positional_bits: u32,
    },
    Program {
        program: SelectorProgramID,
        local: SelectorPrefixLocal,
    },
}

#[derive(Clone)]
struct PrefixCompound {
    predicate: PrefixPredicate,
    dispatch_key: DispatchKey,
}

#[derive(Clone)]
struct PrefixStep {
    compound: PrefixCompoundID,
    /// The step's terminals and four successor kinds, compiled into one immutable action stream.
    output_start: u32,
    output_len: u32,
}

/// Mutable output storage used only while selector chains are registered. Keeping it separate
/// lets the finished automaton discard five vector headers per step.
#[derive(Clone, Default)]
struct PrefixStepOutputBuilder {
    child_successors: Vec<PrefixStepID>,
    descendant_successors: Vec<PrefixStepID>,
    adjacent_successors: Vec<PrefixStepID>,
    following_successors: Vec<PrefixStepID>,
    terminals: Vec<DispatchEntryID>,
}

#[derive(Clone, Copy)]
#[repr(u8)]
enum PrefixOutputKind {
    UniqueTerminal,
    SharedTerminal,
    Child,
    Descendant,
    NextSibling,
    FollowingSibling,
}

#[derive(Clone, Copy)]
struct PrefixOutput {
    target: u32,
    kind: PrefixOutputKind,
}

#[derive(Clone)]
struct PrefixEntryPath {
    terminal: DispatchEntryID,
    steps: Box<[PrefixStepID]>,
}

#[derive(Clone)]
struct PrefixEntryPaths {
    key: (SelectorProgramID, u32),
    paths: Vec<PrefixEntryPath>,
}

#[derive(Clone, Default)]
struct PrefixDispatchBucket {
    root_steps: Vec<PrefixStepID>,
    first_step: u32,
    end_step: u32,
}

/// Immutable prefix program attached to one selector dispatch.
#[derive(Clone, Default)]
pub(super) struct PrefixAutomaton {
    compounds: Vec<PrefixCompound>,
    compound_ids: HashMap<PrefixPredicateKey, PrefixCompoundID>,
    features: Vec<FeatureTest>,
    steps: Vec<PrefixStep>,
    step_output_builders: Vec<PrefixStepOutputBuilder>,
    outputs: Vec<PrefixOutput>,
    step_ids: HashMap<PrefixStepKey, PrefixStepID>,
    buckets: HashMap<DispatchKey, PrefixDispatchBucket>,
    /// Runtime lookup is a packed immutable table sorted by selector entry.
    entry_paths: Vec<PrefixEntryPaths>,
    /// Builder-only index discarded when the immutable table is finished.
    entry_path_indices: HashMap<(SelectorProgramID, u32), usize>,
    entry_paths_finished: bool,
    /// The producer of each non-root step, retained in the storage formerly used by the finished
    /// dispatch-order builder so warm removal edits can find shadowing local output in O(1).
    step_predecessors: Vec<u32>,
    /// Whether any registered chain uses a sibling combinator. Transitions of a sibling-free
    /// automaton never read the previous sibling, so the down-only walk stays as cheap as it was.
    has_sibling_steps: bool,
    /// The deduplicated structural tests carried by registered chains, in registration order.
    /// Their per-node answers form the positional bits of every transition and completion key.
    positional_tests: Vec<(SelectorProgramID, PrefixStructuralTest)>,
}

/// The part of an immutable prefix automaton that one transaction can change.
pub(super) struct PrefixSelection {
    steps: Box<[bool]>,
    terminals: Box<[bool]>,
}

impl PrefixAutomaton {
    /// Restore the builder indices and output lists discarded when this automaton was frozen.
    /// The caller owns a deep clone, so extending it cannot mutate the retained template.
    pub(super) fn prepare_to_extend(&mut self) {
        assert!(
            self.entry_paths_finished,
            "only a finished prefix automaton can be extended"
        );
        self.compound_ids = self
            .compounds
            .iter()
            .enumerate()
            .map(|(index, compound)| {
                let key = match &compound.predicate {
                    PrefixPredicate::Features {
                        feature_start,
                        feature_len,
                        required_positional_bits,
                    } => {
                        let start = *feature_start as usize;
                        PrefixPredicateKey::Features {
                            features: self.features[start..start + *feature_len as usize].into(),
                            required_positional_bits: *required_positional_bits,
                        }
                    }
                    PrefixPredicate::Program { program, local } => PrefixPredicateKey::Program {
                        program: *program,
                        local: *local,
                    },
                };
                (
                    key,
                    PrefixCompoundID(u32::try_from(index).expect("selector prefix compound space exhausted")),
                )
            })
            .collect();
        self.step_output_builders = self
            .steps
            .iter()
            .map(|step| {
                let mut builder = PrefixStepOutputBuilder::default();
                for output in self.outputs_for(step) {
                    match output.kind {
                        PrefixOutputKind::UniqueTerminal | PrefixOutputKind::SharedTerminal => builder
                            .terminals
                            .push(DispatchEntryID::from_index(output.target as usize)),
                        PrefixOutputKind::Child => builder.child_successors.push(PrefixStepID(output.target)),
                        PrefixOutputKind::Descendant => builder.descendant_successors.push(PrefixStepID(output.target)),
                        PrefixOutputKind::NextSibling => builder.adjacent_successors.push(PrefixStepID(output.target)),
                        PrefixOutputKind::FollowingSibling => {
                            builder.following_successors.push(PrefixStepID(output.target));
                        }
                    }
                }
                builder
            })
            .collect();
        self.step_ids.clear();
        for (index, step) in self.steps.iter().enumerate() {
            let predecessor = self.step_predecessors[index];
            let predecessor = (predecessor != u32::MAX).then_some(PrefixStepID(predecessor));
            let id = PrefixStepID(u32::try_from(index).expect("selector prefix step space exhausted"));
            let axis = predecessor.map_or(SelectorPrefixAxis::Root, |predecessor| {
                let outputs = &self.step_output_builders[predecessor.0 as usize];
                if outputs.child_successors.contains(&id) {
                    SelectorPrefixAxis::Child
                } else if outputs.descendant_successors.contains(&id) {
                    SelectorPrefixAxis::Descendant
                } else if outputs.adjacent_successors.contains(&id) {
                    SelectorPrefixAxis::NextSibling
                } else if outputs.following_successors.contains(&id) {
                    SelectorPrefixAxis::FollowingSibling
                } else {
                    unreachable!("a non-root prefix step must be an output of its predecessor")
                }
            });
            self.step_ids.insert(
                PrefixStepKey {
                    compound: step.compound,
                    predecessor,
                    axis,
                },
                id,
            );
        }
        self.entry_path_indices = self
            .entry_paths
            .iter()
            .enumerate()
            .map(|(index, paths)| (paths.key, index))
            .collect();
        for bucket in self.buckets.values_mut() {
            bucket.first_step = 0;
            bucket.end_step = 0;
        }
        self.outputs.clear();
        self.entry_paths_finished = false;
    }

    pub(super) fn add_entry(
        &mut self,
        programs: &SelectorPrograms,
        program_id: SelectorProgramID,
        selector_entry: u32,
        chain: &[SelectorPrefixStep],
        entry: DispatchEntryID,
        structural_tests_admissible: bool,
    ) -> bool {
        assert!(!self.entry_paths_finished, "cannot add to a finished prefix automaton");
        let program = programs.get(program_id);
        // Every transition and completion key carries one truth bit per registered structural
        // test, so an automaton holds at most 32 of them. Pre-scan the chain's tests before
        // mutating anything: a chain that would overflow the bit space is refused whole and
        // stays with its routes, and a chain whose caller does not admit structural tests at
        // all is refused the same way. Canonical structural tests never reference their
        // registering program, so deduplication is by test value across programs.
        let mut needs_structural_tests = false;
        let mut new_positional_tests: Vec<PrefixStructuralTest> = Vec::new();
        let canonical_steps: Vec<_> = chain
            .iter()
            .map(|step| program.canonical_prefix_features_and_position(step.local))
            .collect();
        for (_, tests) in canonical_steps.iter().flatten() {
            for &test in tests {
                needs_structural_tests = true;
                if !self.positional_tests.iter().any(|&(_, existing)| existing == test)
                    && !new_positional_tests.contains(&test)
                {
                    new_positional_tests.push(test);
                }
            }
        }
        if needs_structural_tests && !structural_tests_admissible {
            return false;
        }
        if self.positional_tests.len() + new_positional_tests.len() > 32 {
            return false;
        }
        if chain.iter().any(|step| {
            matches!(
                step.axis,
                SelectorPrefixAxis::NextSibling | SelectorPrefixAxis::FollowingSibling
            )
        }) {
            self.has_sibling_steps = true;
        }
        let mut predecessor = None;
        let mut path = Vec::with_capacity(chain.len());
        for (&chain_step, canonical) in chain.iter().zip(canonical_steps) {
            let predicate = match canonical {
                Some((features, tests)) => {
                    let mut required_positional_bits = 0_u32;
                    for test in tests {
                        let index = match self.positional_tests.iter().position(|&(_, existing)| existing == test) {
                            Some(index) => index,
                            None => {
                                self.positional_tests.push((program_id, test));
                                self.positional_tests.len() - 1
                            }
                        };
                        required_positional_bits |= 1_u32 << index;
                    }
                    PrefixPredicateKey::Features {
                        features: features.into_boxed_slice(),
                        required_positional_bits,
                    }
                }
                None => PrefixPredicateKey::Program {
                    program: program_id,
                    local: chain_step.local,
                },
            };
            let compound = match self.compound_ids.get(&predicate).copied() {
                Some(compound) => compound,
                None => {
                    let compound = PrefixCompoundID(
                        u32::try_from(self.compounds.len()).expect("selector prefix compound space exhausted"),
                    );
                    let dispatch_key = program.prefix_dispatch_key(chain_step.local);
                    let runtime_predicate = match &predicate {
                        PrefixPredicateKey::Features {
                            features,
                            required_positional_bits,
                        } => {
                            let feature_start =
                                u32::try_from(self.features.len()).expect("selector prefix feature space exhausted");
                            self.features.extend_from_slice(features);
                            PrefixPredicate::Features {
                                feature_start,
                                feature_len: u32::try_from(features.len())
                                    .expect("selector prefix feature space exhausted"),
                                required_positional_bits: *required_positional_bits,
                            }
                        }
                        PrefixPredicateKey::Program { program, local } => PrefixPredicate::Program {
                            program: *program,
                            local: *local,
                        },
                    };
                    self.compounds.push(PrefixCompound {
                        predicate: runtime_predicate,
                        dispatch_key,
                    });
                    self.buckets.entry(dispatch_key).or_default();
                    self.compound_ids.insert(predicate, compound);
                    compound
                }
            };
            let key = PrefixStepKey {
                compound,
                predecessor,
                axis: chain_step.axis,
            };
            let step = match self.step_ids.get(&key).copied() {
                Some(step) => step,
                None => {
                    let step =
                        PrefixStepID(u32::try_from(self.steps.len()).expect("selector prefix step space exhausted"));
                    self.steps.push(PrefixStep {
                        compound,
                        output_start: 0,
                        output_len: 0,
                    });
                    self.step_predecessors.push(predecessor.map_or(u32::MAX, |step| step.0));
                    self.step_output_builders.push(PrefixStepOutputBuilder::default());
                    self.step_ids.insert(key, step);
                    // Only root steps dispatch through buckets: a continuation step is reachable
                    // exactly when its predecessor put it in the parent state, so transitions find
                    // it by walking the (small) active set and asking whether the row carries its
                    // key, instead of scanning every bucket member for the few enabled ones.
                    if chain_step.axis == SelectorPrefixAxis::Root {
                        self.buckets
                            .entry(self.compounds[compound.0 as usize].dispatch_key)
                            .or_default()
                            .root_steps
                            .push(step);
                    }
                    if let Some(predecessor) = predecessor {
                        let predecessor = &mut self.step_output_builders[predecessor.0 as usize];
                        match chain_step.axis {
                            SelectorPrefixAxis::Child => predecessor.child_successors.push(step),
                            SelectorPrefixAxis::Descendant => predecessor.descendant_successors.push(step),
                            SelectorPrefixAxis::NextSibling => predecessor.adjacent_successors.push(step),
                            SelectorPrefixAxis::FollowingSibling => predecessor.following_successors.push(step),
                            SelectorPrefixAxis::Root => {
                                unreachable!("only the first selector prefix is a root")
                            }
                        }
                    }
                    step
                }
            };
            predecessor = Some(step);
            path.push(step);
        }
        let terminal_step = predecessor.expect("a selector chain is not empty");
        self.step_output_builders[terminal_step.0 as usize]
            .terminals
            .push(entry);
        let key = (program_id, selector_entry);
        let index = match self.entry_path_indices.get(&key).copied() {
            Some(index) => index,
            None => {
                let index = self.entry_paths.len();
                self.entry_paths.push(PrefixEntryPaths { key, paths: Vec::new() });
                self.entry_path_indices.insert(key, index);
                index
            }
        };
        self.entry_paths[index].paths.push(PrefixEntryPath {
            terminal: entry,
            steps: path.into_boxed_slice(),
        });
        true
    }

    pub(super) fn finish(&mut self) {
        assert!(!self.entry_paths_finished, "cannot finish a prefix automaton twice");
        if !self.entry_paths.is_sorted_by_key(|entry| entry.key) {
            self.entry_paths.sort_unstable_by_key(|entry| entry.key);
        }
        self.entry_path_indices = HashMap::default();
        self.compound_ids = HashMap::default();
        self.step_ids = HashMap::default();
        let mut step_by_dispatch_order: Vec<_> = (0..self.steps.len())
            .map(|step| PrefixStepID(u32::try_from(step).expect("selector prefix step space exhausted")))
            .collect();
        step_by_dispatch_order.sort_unstable_by_key(|step| {
            let compound = self.steps[step.0 as usize].compound;
            (self.compounds[compound.0 as usize].dispatch_key, *step)
        });
        // Number immutable steps in dispatch order so every retained state payload is directly
        // searchable by bucket range. Remap all builder references once here instead of loading a
        // separate rank through the step table for every runtime state search and sort.
        let mut remap = vec![0_u32; self.steps.len()];
        for (order, &step) in step_by_dispatch_order.iter().enumerate() {
            remap[step.0 as usize] = u32::try_from(order).expect("selector prefix step space exhausted");
        }
        let old_steps = std::mem::take(&mut self.steps);
        self.steps = step_by_dispatch_order
            .iter()
            .map(|step| old_steps[step.0 as usize].clone())
            .collect();
        let mut old_builders = std::mem::take(&mut self.step_output_builders);
        self.step_output_builders = step_by_dispatch_order
            .iter()
            .map(|step| std::mem::take(&mut old_builders[step.0 as usize]))
            .collect();
        let old_predecessors = std::mem::take(&mut self.step_predecessors);
        self.step_predecessors = step_by_dispatch_order
            .iter()
            .map(|step| {
                let predecessor = old_predecessors[step.0 as usize];
                if predecessor == u32::MAX {
                    u32::MAX
                } else {
                    remap[predecessor as usize]
                }
            })
            .collect();
        let remap_steps = |steps: &mut Vec<PrefixStepID>| {
            for step in steps {
                step.0 = remap[step.0 as usize];
            }
        };
        for builder in &mut self.step_output_builders {
            remap_steps(&mut builder.child_successors);
            remap_steps(&mut builder.descendant_successors);
            remap_steps(&mut builder.adjacent_successors);
            remap_steps(&mut builder.following_successors);
        }
        for bucket in self.buckets.values_mut() {
            remap_steps(&mut bucket.root_steps);
        }
        for entry in &mut self.entry_paths {
            for path in &mut entry.paths {
                for step in &mut path.steps {
                    step.0 = remap[step.0 as usize];
                }
            }
        }
        for (order, step) in self.steps.iter_mut().enumerate() {
            let order = u32::try_from(order).expect("selector prefix step space exhausted");
            let compound = step.compound;
            let bucket = self
                .buckets
                .get_mut(&self.compounds[compound.0 as usize].dispatch_key)
                .expect("every prefix compound has a dispatch bucket");
            if bucket.end_step == 0 {
                bucket.first_step = order;
            }
            bucket.end_step = order + 1;
        }
        assert_eq!(self.steps.len(), self.step_output_builders.len());
        let mut terminal_producers = HashMap::<DispatchEntryID, u32>::default();
        for (step_index, builder) in self.step_output_builders.iter().enumerate() {
            let step_index = u32::try_from(step_index).expect("selector prefix step space exhausted");
            for &terminal in &builder.terminals {
                match terminal_producers.get_mut(&terminal) {
                    Some(producer) if *producer != step_index => *producer = u32::MAX,
                    Some(_) => {}
                    None => {
                        terminal_producers.insert(terminal, step_index);
                    }
                }
            }
        }
        for (step_index, (step, builder)) in self
            .steps
            .iter_mut()
            .zip(std::mem::take(&mut self.step_output_builders))
            .enumerate()
        {
            let step_index = u32::try_from(step_index).expect("selector prefix step space exhausted");
            step.output_start = u32::try_from(self.outputs.len()).expect("selector prefix output space exhausted");
            self.outputs
                .extend(builder.terminals.into_iter().map(|terminal| PrefixOutput {
                    target: u32::try_from(terminal.index()).expect("dispatch entry space exhausted"),
                    kind: match terminal_producers[&terminal] == step_index {
                        true => PrefixOutputKind::UniqueTerminal,
                        false => PrefixOutputKind::SharedTerminal,
                    },
                }));
            self.outputs
                .extend(builder.child_successors.into_iter().map(|step| PrefixOutput {
                    target: step.0,
                    kind: PrefixOutputKind::Child,
                }));
            self.outputs
                .extend(builder.descendant_successors.into_iter().map(|step| PrefixOutput {
                    target: step.0,
                    kind: PrefixOutputKind::Descendant,
                }));
            self.outputs
                .extend(builder.adjacent_successors.into_iter().map(|step| PrefixOutput {
                    target: step.0,
                    kind: PrefixOutputKind::NextSibling,
                }));
            self.outputs
                .extend(builder.following_successors.into_iter().map(|step| PrefixOutput {
                    target: step.0,
                    kind: PrefixOutputKind::FollowingSibling,
                }));
            step.output_len =
                u32::try_from(self.outputs.len()).expect("selector prefix output space exhausted") - step.output_start;
        }
        self.entry_paths_finished = true;
    }

    fn sort_steps_by_dispatch_order(&self, steps: &mut [PrefixStepID]) {
        if !steps.is_sorted() {
            steps.sort_unstable();
        }
    }

    #[must_use]
    pub(super) fn is_empty(&self) -> bool {
        self.steps.is_empty()
    }

    #[must_use]
    pub(super) fn has_sibling_steps(&self) -> bool {
        self.has_sibling_steps
    }

    #[must_use]
    pub(super) fn positional_tests(&self) -> &[(SelectorProgramID, PrefixStructuralTest)] {
        &self.positional_tests
    }

    fn bucket(&self, key: DispatchKey) -> Option<&PrefixDispatchBucket> {
        self.buckets.get(&key)
    }

    fn outputs_for(&self, step: &PrefixStep) -> &[PrefixOutput] {
        let start = step.output_start as usize;
        &self.outputs[start..start + step.output_len as usize]
    }

    fn predecessor_of(&self, step: PrefixStepID) -> Option<PrefixStepID> {
        let predecessor = self.step_predecessors[step.0 as usize];
        (predecessor != u32::MAX).then_some(PrefixStepID(predecessor))
    }

    fn features_for(&self, start: u32, len: u32) -> &[FeatureTest] {
        let start = start as usize;
        &self.features[start..start + len as usize]
    }

    fn paths_for(&self, key: (SelectorProgramID, u32)) -> Option<&[PrefixEntryPath]> {
        assert!(self.entry_paths_finished, "cannot query an unfinished prefix automaton");
        let index = self.entry_paths.binary_search_by_key(&key, |entry| entry.key).ok()?;
        Some(&self.entry_paths[index].paths)
    }

    pub(super) fn contains_entry(&self, program: SelectorProgramID, entry: u32) -> bool {
        self.paths_for((program, entry)).is_some()
    }

    pub(super) fn select_entries(
        &self,
        entries: impl IntoIterator<Item = (SelectorProgramID, u32)>,
        terminal_count: usize,
    ) -> PrefixSelection {
        let mut selection = PrefixSelection {
            steps: vec![false; self.steps.len()].into_boxed_slice(),
            terminals: vec![false; terminal_count].into_boxed_slice(),
        };
        for entry in entries {
            let Some(paths) = self.paths_for(entry) else {
                continue;
            };
            for path in paths {
                selection.terminals[path.terminal.index()] = true;
                for step in &path.steps {
                    if !selection.steps[step.0 as usize] {
                        selection.steps[step.0 as usize] = true;
                    }
                }
            }
        }
        selection
    }

    pub(super) fn append_route_producers(
        &self,
        program: SelectorProgramID,
        entry: u32,
        inverse_path_length: usize,
        into: &mut Vec<PrefixProducer>,
    ) -> bool {
        let Some(paths) = self.paths_for((program, entry)) else {
            return false;
        };
        let mut found = false;
        for path in paths {
            let Some(index) = path.steps.len().checked_sub(inverse_path_length.saturating_add(1)) else {
                continue;
            };
            if let Some(&step) = path.steps.get(index) {
                into.push(PrefixProducer { step });
                found = true;
            }
        }
        found
    }

    #[must_use]
    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.compounds,
                self.compound_ids,
                self.features,
                self.steps,
                self.step_output_builders,
                self.outputs,
                self.step_ids,
                self.buckets,
                self.entry_paths,
                self.step_predecessors,
                self.entry_path_indices,
            ];
            cached [];
            nested [
                self
                .compound_ids
                .keys()
                .map(|predicate| match predicate {
                    PrefixPredicateKey::Features { features, .. } => features.len() * size_of::<FeatureTest>(),
                    PrefixPredicateKey::Program { .. } => 0,
                })
                .sum::<usize>(),
                self
                .step_output_builders
                .iter()
                .map(|builder| {
                    (builder.child_successors.capacity()
                        + builder.descendant_successors.capacity()
                        + builder.adjacent_successors.capacity()
                        + builder.following_successors.capacity())
                        * size_of::<PrefixStepID>()
                        + builder.terminals.capacity() * size_of::<DispatchEntryID>()
                })
                .sum::<usize>(),
                self
                .buckets
                .values()
                .map(|bucket| bucket.root_steps.capacity() * size_of::<PrefixStepID>())
                .sum::<usize>(),
                self
                .entry_paths
                .iter()
                .map(|entry| {
                    entry.paths.capacity() * size_of::<PrefixEntryPath>()
                        + entry
                            .paths
                            .iter()
                            .map(|path| path.steps.len() * size_of::<PrefixStepID>())
                            .sum::<usize>()
                })
                .sum::<usize>(),
            ];
            skip [self.entry_paths_finished];
        }
    }
}

impl PrefixSelection {
    fn contains_step(&self, step: PrefixStepID) -> bool {
        self.steps[step.0 as usize]
    }

    fn contains_terminal(&self, terminal: DispatchEntryID) -> bool {
        self.terminals[terminal.index()]
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [];
            nested [self.steps.len(), self.terminals.len()];
            skip [];
        }
    }
}

/// One interned set of active continuation steps, partitioned into a persisting part and an
/// expiring part, stored as a delta over the state it extends.
///
/// A transition constructs every output state as its entering state plus a handful of new
/// admissions, and rightward states are position-unique unions that defeat content
/// deduplication, so the representation IS the construction: the persisting set is the base's
/// persisting set plus this state's sorted additions, and only the additions and the (one-edge)
/// expiring steps are stored. Enumerating a state walks its base chain; each level's additions
/// are disjoint from every deeper level's by construction, so the chain is at most one level
/// per persisting step.
///
/// A downward state's persisting part holds descendant steps and its expiring part holds child
/// steps; a rightward state holds following-sibling and adjacent steps. Both share this
/// representation because their algebra is identical, so the field names read "descendant" for
/// the persisting partition even when a state travels right.
#[derive(Default)]
struct PrefixState {
    /// The state whose persisting set this one extends; the empty state extends itself.
    base: u32,
    /// The delta payload in `delta_steps`: the dispatch-ordered persisting additions, all
    /// genuinely new relative to the base's persisting set, followed by the dispatch-ordered
    /// expiring steps.
    payload_start: u32,
    additions_len: u32,
    expiring_len: u32,
    /// The total persisting step count: the base's plus this state's additions.
    descendant_len: u32,
    /// Order-independent sum of the persisting steps' mixed hashes over the whole chain. A
    /// transition that adds no persisting step inherits it; one that does extends it in
    /// O(additions).
    descendant_hash: u64,
    /// Order-independent digest of the one-edge expiring steps.
    expiring_hash: u32,
}

fn step_hash(step: PrefixStepID) -> u64 {
    (u64::from(step.0) ^ 0x9E37_79B9_7F4A_7C15).wrapping_mul(0x2545_F491_4F6C_DD1D)
}

/// Structural identity of a delta state: the base it extends plus its own payload. Two
/// semantically equal states built over different bases intern separately, which is bounded and
/// acceptable; content comparisons therefore never rely on state identity alone.
fn state_structural_hash(
    base: u32,
    additions_len: u32,
    additions_hash: u64,
    expiring_len: u32,
    expiring_hash: u32,
) -> u64 {
    let mut hasher = fast_hasher();
    base.hash(&mut hasher);
    additions_len.hash(&mut hasher);
    additions_hash.hash(&mut hasher);
    expiring_len.hash(&mut hasher);
    expiring_hash.hash(&mut hasher);
    hasher.finish()
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) struct PrefixTransitionKey {
    pub parent: u32,
    /// The previous sibling's rightward output state; always zero in a sibling-free automaton.
    pub previous: u32,
    pub local_facts: u32,
    pub is_document_root: bool,
    /// One answer bit per automaton positional test; always zero when the automaton has none.
    /// Positional truth is per index, not per fact cohort, so it must split the memo identity.
    pub positional_bits: u32,
}

/// The two interned states a transition starts from.
#[derive(Clone, Copy)]
struct EnteringStates {
    /// The parent's downward output state.
    parent: u32,
    /// The previous sibling's rightward output state; zero in a sibling-free automaton.
    previous: u32,
}

const UNKNOWN_ENTERING_STATES: EnteringStates = EnteringStates {
    parent: UNKNOWN_STATE,
    previous: 0,
};

#[derive(Clone, Copy)]
struct TransitionInputs {
    entering: EnteringStates,
    local_facts: u32,
    positional_bits: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct PrefixTransition {
    /// The downward output state, entering the node's children.
    state: u32,
    /// The rightward output state, entering the node's next sibling.
    right: u32,
    result: PrefixResultID,
}

const UNKNOWN_STATE: u32 = u32::MAX;

const UNKNOWN_TRANSITION: PrefixTransition = PrefixTransition {
    state: UNKNOWN_STATE,
    right: 0,
    result: PrefixResultID(0),
};

define_id! { struct PrefixStateID(); }

impl super::intern_table::InternIdentity for PrefixStateID {
    fn index(self) -> usize {
        self.0 as usize - 1
    }
}

define_id! { struct LocalFactSlot(); }

impl super::intern_table::InternIdentity for LocalFactSlot {
    fn index(self) -> usize {
        self.0 as usize
    }
}

struct LocalFactInterner {
    identities: super::intern_table::InternTable<LocalFactSlot, (u32, u32)>,
    next_identity: u32,
}

impl LocalFactInterner {
    fn new() -> Self {
        Self {
            identities: super::intern_table::InternTable::default(),
            next_identity: 1,
        }
    }

    fn intern(&mut self, facts: &StyleNodeFacts, row: u32, counters: &mut Counters) -> u32 {
        let hash = hash_local_facts(facts, row);
        if let Some(slot) = self.identities.find(hash, |_slot, &(_identity, representative)| {
            rows_have_equal_local_facts(facts, row, representative)
        }) {
            counters.bump(Counter::PrefixLocalFactIdentityHits);
            return self.identities[slot].0;
        }
        counters.bump(Counter::PrefixLocalFactIdentityMisses);
        let identity = self.next_identity;
        self.next_identity = self
            .next_identity
            .checked_add(1)
            .expect("local fact identity space exhausted");
        let slot = LocalFactSlot(u32::try_from(self.identities.len()).expect("local fact table exceeds u32 indexing"));
        self.identities.insert(hash, slot, (identity, row));
        identity
    }

    fn clear_rows(&mut self) {
        self.identities = super::intern_table::InternTable::default();
    }

    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.identities];
            cached [];
            nested [];
            skip [self.next_identity];
        }
    }
}

/// Traversal-local prefix states for one immutable selector dispatch.
pub(super) struct PrefixStates {
    states: Vec<PrefixState>,
    /// Every state's delta payload: its persisting additions followed by its expiring steps.
    delta_steps: Vec<std::mem::MaybeUninit<PrefixStepID>>,
    states_by_hash: super::intern_table::InternTable<PrefixStateID, ()>,
    match_offsets: Vec<u32>,
    match_entries: Vec<DispatchEntryID>,
    truth_offsets: Vec<u32>,
    truth_steps: Vec<PrefixStepID>,
    truth_sets_by_hash: super::intern_table::InternTable<PrefixTruthSetID, ()>,
    results: Vec<PrefixResult>,
    result_ids: super::intern_table::InternTable<PrefixResultID, PrefixResult>,
    match_sets_by_hash: super::intern_table::InternTable<PrefixMatchSetID, ()>,
    transitions: HashMap<PrefixTransitionKey, PrefixTransition>,
    local_fact_interner: LocalFactInterner,
    transition_by_row: Vec<PrefixTransition>,
    transition_by_element: Column<PrefixTransition>,
    entering_by_element: Column<EnteringStates>,
    local_facts_by_element: Column<u32>,
    /// Per-element positional truth retained only for automata that test it.
    positional_bits_by_element: Column<u32>,
    candidate_epoch: EpochColumn,
    compound_epoch: EpochColumn,
    compound_answer: Vec<bool>,
    output_epoch: EpochColumn,
    match_epoch: EpochColumn,
    /// Per step, the epoch in which the current transition's entering parent state held it as
    /// persisting; admissions already persisting there are dropped instead of stored again.
    parent_persisting_epoch: EpochColumn,
    /// The same membership marks for the entering previous-sibling state's persisting part.
    previous_persisting_epoch: EpochColumn,
    candidates: Vec<PrefixStepID>,
    compare_left: Vec<PrefixStepID>,
    compare_right: Vec<PrefixStepID>,
    output_matches: Vec<DispatchEntryID>,
    output_matched_steps: Vec<PrefixStepID>,
    new_descendant: Vec<PrefixStepID>,
    new_child: Vec<PrefixStepID>,
    new_following: Vec<PrefixStepID>,
    new_adjacent: Vec<PrefixStepID>,
    new_descendant_hash: u64,
    new_child_hash: u32,
    new_following_hash: u64,
    new_adjacent_hash: u32,
    /// Per state, the interned state holding exactly its persisting part; the state itself when
    /// it has no expiring steps, lazily interned otherwise. This is the no-admission fast path,
    /// shared by downward and rightward states.
    descendant_only: Vec<u32>,
    /// The batch row space `transition_by_row` and the local-fact representatives were built
    /// against; see StyleNodeFacts::generation.
    facts_generation: u64,
    ancestor_chain: Vec<(StyleNodeID, u32)>,
    epoch: u32,
    complete: bool,
    automaton_step_count: usize,
    automaton_statistics_recorded: bool,
    last_compacted_working_bytes: u64,
}

enum PrefixTransitionSurface<'a> {
    Retained(&'a mut PrefixStates),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct PrefixDifference {
    pub continuation_changed: bool,
    /// Whether the rightward output entering the next sibling differs; always false in a
    /// sibling-free automaton.
    pub right_changed: bool,
    pub matches_changed: bool,
    pub arrived: bool,
    /// The interned match sets on either side, so a consumer can name exactly which entries
    /// moved instead of treating the node as opaquely changed.
    pub old_matches: PrefixMatchSetID,
    pub new_matches: PrefixMatchSetID,
    pub continuation_delta: Option<PrefixStateDeltaID>,
    pub right_delta: Option<PrefixStateDeltaID>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum PrefixTransitionGap {
    MissingTransition(StyleNodeID),
    Incomplete(Incomplete),
}

pub(super) enum PrefixTransitionLookup<T> {
    Known(T),
    Missing(PrefixTransitionGap),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PrefixTransitionOrigin {
    Computed,
    Memoized,
}

pub(super) struct PrefixEvaluation<'a, 'b> {
    automaton: &'a PrefixAutomaton,
    tree: &'a StyleNodeTree,
    facts: &'a StyleNodeFacts,
    programs: &'a SelectorPrograms,
    evaluator: &'a MatchEvaluator<'b>,
    shadow_root: Option<StyleNodeID>,
    selection: Option<&'a PrefixSelection>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct PrefixStepSpan {
    start: u32,
    len: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct PrefixMatchSpan {
    start: u32,
    len: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct PrefixMatchDelta {
    additions: PrefixMatchSpan,
    removals: PrefixMatchSpan,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(super) struct PrefixStateDelta {
    persisting_additions: PrefixStepSpan,
    persisting_removals: PrefixStepSpan,
    expiring_additions: PrefixStepSpan,
    expiring_removals: PrefixStepSpan,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct PrefixStateDeltaID(NonZeroU32);

#[derive(Clone, Copy, Default)]
pub(super) struct PrefixEnteringDeltas {
    pub parent: Option<PrefixStateDeltaID>,
    pub previous: Option<PrefixStateDeltaID>,
}

#[derive(Clone, Copy)]
struct PrefixLocalOutputDeltas {
    continuation: PrefixStateDeltaID,
    right: PrefixStateDeltaID,
    matches: PrefixMatchDelta,
    outputs_changed: bool,
    result_changed: bool,
    result_delta_complete: bool,
}

#[derive(Default)]
pub(super) struct PrefixDeltaArena {
    steps: Vec<PrefixStepID>,
    deltas: Vec<PrefixStateDelta>,
    matches: Vec<DispatchEntryID>,
    scratch: [Vec<PrefixStepID>; 8],
    match_scratch: [Vec<DispatchEntryID>; 2],
    signed_scratch: Vec<(PrefixStepID, i8)>,
}

impl PrefixDeltaArena {
    fn append(&mut self, steps: &[PrefixStepID]) -> PrefixStepSpan {
        let start = u32::try_from(self.steps.len()).expect("selector prefix delta arena overflow");
        self.steps.extend_from_slice(steps);
        PrefixStepSpan {
            start,
            len: u32::try_from(steps.len()).expect("selector prefix delta arena overflow"),
        }
    }

    fn get(&self, span: PrefixStepSpan) -> &[PrefixStepID] {
        let start = span.start as usize;
        &self.steps[start..start + span.len as usize]
    }

    fn append_matches(&mut self, matches: &[DispatchEntryID]) -> PrefixMatchSpan {
        let start = u32::try_from(self.matches.len()).expect("selector prefix match delta arena overflow");
        self.matches.extend_from_slice(matches);
        PrefixMatchSpan {
            start,
            len: u32::try_from(matches.len()).expect("selector prefix match delta arena overflow"),
        }
    }

    fn get_matches(&self, span: PrefixMatchSpan) -> &[DispatchEntryID] {
        let start = span.start as usize;
        &self.matches[start..start + span.len as usize]
    }

    fn append_match_delta(&mut self) -> PrefixMatchDelta {
        let additions = std::mem::take(&mut self.match_scratch[0]);
        let removals = std::mem::take(&mut self.match_scratch[1]);
        let delta = PrefixMatchDelta {
            additions: self.append_matches(&additions),
            removals: self.append_matches(&removals),
        };
        self.match_scratch[0] = additions;
        self.match_scratch[1] = removals;
        delta
    }

    fn push_delta(&mut self, delta: PrefixStateDelta) -> PrefixStateDeltaID {
        self.deltas.push(delta);
        PrefixStateDeltaID(
            NonZeroU32::new(u32::try_from(self.deltas.len()).expect("selector prefix delta arena overflow")).unwrap(),
        )
    }

    fn delta(&self, id: PrefixStateDeltaID) -> PrefixStateDelta {
        self.deltas[id.0.get() as usize - 1]
    }

    fn sign_in_spans(&self, additions: PrefixStepSpan, removals: PrefixStepSpan, step: PrefixStepID) -> i8 {
        if self.get(additions).binary_search(&step).is_ok() {
            1
        } else if self.get(removals).binary_search(&step).is_ok() {
            -1
        } else {
            0
        }
    }

    fn persisting_sign(&self, delta: Option<PrefixStateDeltaID>, step: PrefixStepID) -> i8 {
        let Some(delta) = delta.map(|id| self.delta(id)) else {
            return 0;
        };
        self.sign_in_spans(delta.persisting_additions, delta.persisting_removals, step)
    }

    fn state_sign(&self, delta: Option<PrefixStateDeltaID>, step: PrefixStepID) -> i8 {
        let Some(delta) = delta.map(|id| self.delta(id)) else {
            return 0;
        };
        let persisting = self.sign_in_spans(delta.persisting_additions, delta.persisting_removals, step);
        let expiring = self.sign_in_spans(delta.expiring_additions, delta.expiring_removals, step);
        persisting + expiring
    }

    /// Whether an exact signed delta changes the active state viewed through `selection`.
    ///
    /// A step can occur in both delta partitions while moving between them, so test its combined
    /// state sign instead of treating every non-empty span as a semantic change.
    fn changes_selected_state(&self, id: PrefixStateDeltaID, selection: &PrefixSelection) -> bool {
        let delta = self.delta(id);
        [
            delta.persisting_additions,
            delta.persisting_removals,
            delta.expiring_additions,
            delta.expiring_removals,
        ]
        .into_iter()
        .flat_map(|span| self.get(span))
        .any(|&step| selection.contains_step(step) && self.state_sign(Some(id), step) != 0)
    }

    fn persisting_only(&mut self, id: Option<PrefixStateDeltaID>) -> PrefixStateDeltaID {
        let delta = id.map(|id| self.delta(id)).unwrap_or_default();
        self.push_delta(PrefixStateDelta {
            persisting_additions: delta.persisting_additions,
            persisting_removals: delta.persisting_removals,
            ..PrefixStateDelta::default()
        })
    }

    fn append_scratch_delta(&mut self, offset: usize) -> PrefixStateDeltaID {
        let persisting_additions = std::mem::take(&mut self.scratch[offset]);
        let persisting_removals = std::mem::take(&mut self.scratch[offset + 1]);
        let expiring_additions = std::mem::take(&mut self.scratch[offset + 2]);
        let expiring_removals = std::mem::take(&mut self.scratch[offset + 3]);
        let delta = PrefixStateDelta {
            persisting_additions: self.append(&persisting_additions),
            persisting_removals: self.append(&persisting_removals),
            expiring_additions: self.append(&expiring_additions),
            expiring_removals: self.append(&expiring_removals),
        };
        self.scratch[offset] = persisting_additions;
        self.scratch[offset + 1] = persisting_removals;
        self.scratch[offset + 2] = expiring_additions;
        self.scratch[offset + 3] = expiring_removals;
        self.push_delta(delta)
    }

    fn combine(&mut self, base: Option<PrefixStateDeltaID>, local: PrefixStateDeltaID) -> PrefixStateDeltaID {
        for scratch in &mut self.scratch[..4] {
            scratch.clear();
        }
        let base = base.map(|id| self.delta(id));
        let local = self.delta(local);
        for (index, (base_additions, base_removals, local_additions, local_removals)) in [
            (
                base.map(|delta| delta.persisting_additions),
                base.map(|delta| delta.persisting_removals),
                local.persisting_additions,
                local.persisting_removals,
            ),
            (None, None, local.expiring_additions, local.expiring_removals),
        ]
        .into_iter()
        .enumerate()
        {
            self.signed_scratch.clear();
            for span in base_additions.into_iter().chain([local_additions]) {
                let start = span.start as usize;
                for index in start..start + span.len as usize {
                    self.signed_scratch.push((self.steps[index], 1));
                }
            }
            for span in base_removals.into_iter().chain([local_removals]) {
                let start = span.start as usize;
                for index in start..start + span.len as usize {
                    self.signed_scratch.push((self.steps[index], -1));
                }
            }
            self.signed_scratch.sort_unstable_by_key(|&(step, _)| step);
            let mut cursor = 0;
            while cursor < self.signed_scratch.len() {
                let step = self.signed_scratch[cursor].0;
                let mut sign = 0_i32;
                while cursor < self.signed_scratch.len() && self.signed_scratch[cursor].0 == step {
                    sign += i32::from(self.signed_scratch[cursor].1);
                    cursor += 1;
                }
                if sign > 0 {
                    self.scratch[index * 2].push(step);
                } else if sign < 0 {
                    self.scratch[index * 2 + 1].push(step);
                }
            }
        }
        self.append_scratch_delta(0)
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.steps, self.deltas, self.matches, self.signed_scratch];
            cached [];
            nested [
                self.scratch.iter().map(Vec::capacity).sum::<usize>() * size_of::<PrefixStepID>(),
                self.match_scratch.iter().map(Vec::capacity).sum::<usize>() * size_of::<DispatchEntryID>(),
            ];
            skip [];
        }
    }
}

impl<'a, 'b> PrefixEvaluation<'a, 'b> {
    fn positional_test_matches(
        &self,
        node: StyleNodeID,
        index: usize,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        let (program, test) = self.automaton.positional_tests()[index];
        match test {
            PrefixStructuralTest::Nth(nth) => {
                self.evaluator
                    .matches_nth(self.programs.get(program), nth, node, counters)
            }
            PrefixStructuralTest::Empty => {
                let Some(row) = self.facts.row_of(node) else {
                    return Err(Incomplete::MissingFacts(node));
                };
                Ok(self.tree.first_element_child(node).is_none() && !self.facts.has_text_content_of(row))
            }
        }
    }

    pub(super) fn step_matches_direct(
        &self,
        node: StyleNodeID,
        step: PrefixStepID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        let row = self.facts.row_of(node).ok_or(Incomplete::MissingFacts(node))?;
        let compound = &self.automaton.compounds[self.automaton.steps[step.0 as usize].compound.0 as usize];
        match &compound.predicate {
            PrefixPredicate::Features {
                feature_start,
                feature_len,
                required_positional_bits,
            } => {
                let mut remaining = *required_positional_bits;
                while remaining != 0 {
                    let index = remaining.trailing_zeros() as usize;
                    if !self.positional_test_matches(node, index, counters)? {
                        return Ok(false);
                    }
                    remaining &= remaining - 1;
                }
                Ok(self
                    .automaton
                    .features_for(*feature_start, *feature_len)
                    .iter()
                    .all(|&feature| matches_feature(self.facts, row, feature)))
            }
            PrefixPredicate::Program { program, local } => {
                self.evaluator
                    .matches_prefix_local(*program, self.programs.get(*program), *local, node, counters)
            }
        }
    }

    pub(super) fn step_matches(
        &self,
        node: StyleNodeID,
        positional_bits: u32,
        step: PrefixStepID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        let row = self.facts.row_of(node).ok_or(Incomplete::MissingFacts(node))?;
        let compound = &self.automaton.compounds[self.automaton.steps[step.0 as usize].compound.0 as usize];
        match &compound.predicate {
            PrefixPredicate::Features {
                feature_start,
                feature_len,
                required_positional_bits,
            } => Ok(
                (positional_bits & required_positional_bits) == *required_positional_bits
                    && self
                        .automaton
                        .features_for(*feature_start, *feature_len)
                        .iter()
                        .all(|&feature| matches_feature(self.facts, row, feature)),
            ),
            PrefixPredicate::Program { program, local } => {
                self.evaluator
                    .matches_prefix_local(*program, self.programs.get(*program), *local, node, counters)
            }
        }
    }

    /// The per-node answers of the automaton's positional tests, one bit per test. Zero for
    /// automata without positional steps, so fact-cohort sharing is unchanged there.
    pub(super) fn positional_bits(&self, node: StyleNodeID, counters: &mut Counters) -> Result<u32, Incomplete> {
        let tests = self.automaton.positional_tests();
        let mut bits = 0_u32;
        for index in 0..tests.len() {
            if self.positional_test_matches(node, index, counters)? {
                bits |= 1 << index;
            }
        }
        Ok(bits)
    }

    pub(super) fn new(
        automaton: &'a PrefixAutomaton,
        tree: &'a StyleNodeTree,
        facts: &'a StyleNodeFacts,
        programs: &'a SelectorPrograms,
        evaluator: &'a MatchEvaluator<'b>,
        shadow_root: Option<StyleNodeID>,
        selection: Option<&'a PrefixSelection>,
    ) -> Self {
        Self {
            automaton,
            tree,
            facts,
            programs,
            evaluator,
            shadow_root,
            selection,
        }
    }
}

impl PrefixStates {
    #[must_use]
    pub(super) fn new(row_count: usize) -> Self {
        Self {
            states: vec![PrefixState::default()],
            delta_steps: Vec::new(),
            states_by_hash: super::intern_table::InternTable::default(),
            match_offsets: vec![0, 0],
            match_entries: Vec::new(),
            truth_offsets: vec![0, 0],
            truth_steps: Vec::new(),
            truth_sets_by_hash: super::intern_table::InternTable::default(),
            results: vec![PrefixResult {
                matches: PrefixMatchSetID(0),
                truth: PrefixTruthSetID(0),
            }],
            result_ids: super::intern_table::InternTable::default(),
            match_sets_by_hash: super::intern_table::InternTable::default(),
            transitions: HashMap::default(),
            local_fact_interner: LocalFactInterner::new(),
            transition_by_row: vec![UNKNOWN_TRANSITION; row_count],
            transition_by_element: Column::new(|| UNKNOWN_TRANSITION),
            entering_by_element: Column::new(|| UNKNOWN_ENTERING_STATES),
            local_facts_by_element: Column::default(),
            positional_bits_by_element: Column::default(),
            candidate_epoch: EpochColumn::default(),
            compound_epoch: EpochColumn::default(),
            compound_answer: Vec::new(),
            output_epoch: EpochColumn::default(),
            match_epoch: EpochColumn::default(),
            parent_persisting_epoch: EpochColumn::default(),
            previous_persisting_epoch: EpochColumn::default(),
            candidates: Vec::new(),
            compare_left: Vec::new(),
            compare_right: Vec::new(),
            output_matches: Vec::new(),
            output_matched_steps: Vec::new(),
            new_descendant: Vec::new(),
            new_child: Vec::new(),
            new_following: Vec::new(),
            new_adjacent: Vec::new(),
            new_descendant_hash: 0,
            new_child_hash: 0,
            new_following_hash: 0,
            new_adjacent_hash: 0,
            descendant_only: vec![0],
            facts_generation: 0,
            ancestor_chain: Vec::new(),
            epoch: 0,
            complete: false,
            automaton_step_count: 0,
            automaton_statistics_recorded: false,
            last_compacted_working_bytes: 0,
        }
    }

    pub(super) fn match_set_for(
        &mut self,
        evaluation: &PrefixEvaluation<'_, '_>,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> PrefixTransitionLookup<PrefixMatchSetID> {
        match self.transition_of(node) {
            PrefixTransitionLookup::Known(transition) => {
                counters.bump(Counter::PrefixTransitionCacheMatchHits);
                return PrefixTransitionLookup::Known(self.matches_of(transition));
            }
            PrefixTransitionLookup::Missing(_) => {}
        }
        counters.bump(Counter::PrefixTransitionCacheMatchMisses);
        match self.transition_for(evaluation, node, counters) {
            PrefixTransitionLookup::Known(transition) => PrefixTransitionLookup::Known(self.matches_of(transition)),
            PrefixTransitionLookup::Missing(gap) => PrefixTransitionLookup::Missing(gap),
        }
    }

    pub(super) fn matches_in(&self, matches: PrefixMatchSetID) -> &[DispatchEntryID] {
        let index = matches.0 as usize;
        &self.match_entries[self.match_offsets[index] as usize..self.match_offsets[index + 1] as usize]
    }

    /// Read the exact terminal matches already retained for one element without extending the
    /// prefix relation. A missing transition is not an empty answer.
    pub(super) fn retained_matches_for(&self, node: StyleNodeID) -> Option<&[DispatchEntryID]> {
        match self.transition_of(node) {
            PrefixTransitionLookup::Known(transition) => Some(self.matches_in(self.matches_of(transition))),
            PrefixTransitionLookup::Missing(_) => None,
        }
    }

    fn matches_of(&self, transition: PrefixTransition) -> PrefixMatchSetID {
        let result = self.results[transition.result.0 as usize];
        debug_assert!((result.truth.0 as usize) + 1 < self.truth_offsets.len());
        result.matches
    }

    fn truth_in(&self, truth: PrefixTruthSetID) -> &[PrefixStepID] {
        let index = truth.0 as usize;
        &self.truth_steps[self.truth_offsets[index] as usize..self.truth_offsets[index + 1] as usize]
    }

    fn additions_in(&self, state: u32) -> &[PrefixStepID] {
        let state = &self.states[state as usize];
        let start = state.payload_start as usize;
        // SAFETY: Every referenced state range is initialized. Sparse rebases can leave
        // unreferenced holes in the arena, but never point a state at one.
        unsafe {
            std::slice::from_raw_parts(
                self.delta_steps.as_ptr().add(start).cast::<PrefixStepID>(),
                state.additions_len as usize,
            )
        }
    }

    fn expiring_in(&self, state: u32) -> &[PrefixStepID] {
        let state = &self.states[state as usize];
        let start = state.payload_start as usize + state.additions_len as usize;
        // SAFETY: See `additions_in`; this is the initialized suffix of the same payload.
        unsafe {
            std::slice::from_raw_parts(
                self.delta_steps.as_ptr().add(start).cast::<PrefixStepID>(),
                state.expiring_len as usize,
            )
        }
    }

    /// Collect one state's persisting steps into `into` by walking its base chain. The levels'
    /// additions are mutually disjoint by construction, so the result is the exact persisting
    /// set, unsorted.
    fn collect_persisting(&self, state: u32, into: &mut Vec<PrefixStepID>) {
        let mut current = state;
        loop {
            into.extend_from_slice(self.additions_in(current));
            let base = self.states[current as usize].base;
            if base == current {
                break;
            }
            current = base;
        }
    }

    fn collect_active(&self, state: u32, into: &mut Vec<PrefixStepID>) {
        into.extend_from_slice(self.expiring_in(state));
        self.collect_persisting(state, into);
    }

    /// Whether two states hold the same active steps, viewed through an optional selection.
    ///
    /// Structural interning means distinct identities can still be content-equal, so identity is
    /// only a fast positive. The persisting hash is a fast negative when no selection filters
    /// the view; the full check compares both persisting sets through dense epoch marks, with the
    /// expiring parts compared directly.
    fn selected_states_equal(&mut self, left: u32, right: u32, selection: Option<&PrefixSelection>) -> bool {
        if left == right {
            return true;
        }
        let left_contents = &self.states[left as usize];
        let right_contents = &self.states[right as usize];
        if selection.is_none()
            && (left_contents.descendant_len != right_contents.descendant_len
                || left_contents.descendant_hash != right_contents.descendant_hash)
        {
            return false;
        }
        advance_epoch(
            &mut self.epoch,
            2,
            &mut [
                &mut self.candidate_epoch,
                &mut self.compound_epoch,
                &mut self.output_epoch,
                &mut self.match_epoch,
                &mut self.parent_persisting_epoch,
                &mut self.previous_persisting_epoch,
            ],
        );
        let left_epoch = self.epoch - 1;
        let right_epoch = self.epoch;
        let mut compare_epoch = std::mem::take(&mut self.candidate_epoch);
        compare_epoch.ensure_len(self.automaton_step_count);
        let mut left_count = 0;
        let mut current = left;
        loop {
            for &step in self.additions_in(current) {
                if selection.is_some_and(|selection| !selection.contains_step(step)) {
                    continue;
                }
                let mark = &mut compare_epoch[step.0 as usize];
                if *mark != left_epoch {
                    *mark = left_epoch;
                    left_count += 1;
                }
            }
            let base = self.states[current as usize].base;
            if base == current {
                break;
            }
            current = base;
        }
        let mut right_count = 0;
        let mut equal = true;
        current = right;
        'right: loop {
            for &step in self.additions_in(current) {
                if selection.is_some_and(|selection| !selection.contains_step(step)) {
                    continue;
                }
                let mark = &mut compare_epoch[step.0 as usize];
                if *mark == right_epoch {
                    continue;
                }
                if *mark != left_epoch {
                    equal = false;
                    break 'right;
                }
                *mark = right_epoch;
                right_count += 1;
            }
            let base = self.states[current as usize].base;
            if base == current {
                break;
            }
            current = base;
        }
        equal &= left_count == right_count;
        if equal {
            let left_expiring = self.expiring_in(left).iter().copied();
            let right_expiring = self.expiring_in(right).iter().copied();
            equal = match selection {
                Some(selection) => left_expiring
                    .filter(|&step| selection.contains_step(step))
                    .eq(right_expiring.filter(|&step| selection.contains_step(step))),
                None => left_expiring.eq(right_expiring),
            };
        }
        self.candidate_epoch = compare_epoch;
        self.compare_left.clear();
        self.compare_left.reserve(left_count);
        self.compare_right.clear();
        self.compare_right.reserve(right_count);
        equal
    }

    fn transition_of(&self, node: StyleNodeID) -> PrefixTransitionLookup<PrefixTransition> {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return PrefixTransitionLookup::Missing(PrefixTransitionGap::MissingTransition(node));
        };
        match self
            .transition_by_element
            .get(index)
            .copied()
            .filter(|transition| transition.state != UNKNOWN_STATE)
        {
            Some(transition) => PrefixTransitionLookup::Known(transition),
            None => PrefixTransitionLookup::Missing(PrefixTransitionGap::MissingTransition(node)),
        }
    }

    #[must_use]
    pub(super) fn has_transition(&self, node: StyleNodeID) -> bool {
        matches!(self.transition_of(node), PrefixTransitionLookup::Known(_))
    }

    fn set_transition(&mut self, node: StyleNodeID, transition: PrefixTransition) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        self.transition_by_element.insert(index, transition);
    }

    fn set_entering(&mut self, node: StyleNodeID, entering: EnteringStates) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        self.entering_by_element.insert(index, entering);
    }

    fn entering_of(&self, node: StyleNodeID) -> Option<EnteringStates> {
        let index = node.element_index()? as usize;
        self.entering_by_element
            .get(index)
            .copied()
            .filter(|entering| entering.parent != UNKNOWN_STATE)
    }

    fn local_facts_of(&self, node: StyleNodeID) -> Option<u32> {
        let index = node.element_index()? as usize;
        self.local_facts_by_element
            .get(index)
            .copied()
            .filter(|&identity| identity != 0)
    }

    fn set_local_facts(&mut self, node: StyleNodeID, identity: u32) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        self.local_facts_by_element.insert(index, identity);
    }

    fn set_positional_bits(&mut self, node: StyleNodeID, bits: u32) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        self.positional_bits_by_element.insert(index, bits);
    }

    fn positional_bits_of(&self, node: StyleNodeID) -> u32 {
        node.element_index()
            .and_then(|index| self.positional_bits_by_element.get(index as usize))
            .copied()
            .unwrap_or(0)
    }

    pub(super) fn forget_transition(&mut self, node: StyleNodeID) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        let Some(transition) = self.transition_by_element.get_mut(index) else {
            return;
        };
        *transition = UNKNOWN_TRANSITION;
        if let Some(entering) = self.entering_by_element.get_mut(index) {
            *entering = UNKNOWN_ENTERING_STATES;
        }
        if let Some(local_facts) = self.local_facts_by_element.get_mut(index) {
            *local_facts = 0;
        }
        if let Some(positional_bits) = self.positional_bits_by_element.get_mut(index) {
            *positional_bits = 0;
        }
    }

    fn persisting_state_contains_step(&self, state: u32, step: PrefixStepID) -> bool {
        let step_index = step.0;
        let mut current = state;
        loop {
            if self
                .additions_in(current)
                .binary_search_by_key(&step_index, |candidate| candidate.0)
                .is_ok()
            {
                return true;
            }
            let base = self.states[current as usize].base;
            if base == current {
                return false;
            }
            current = base;
        }
    }

    fn state_contains_step(&self, state: u32, step: PrefixStepID) -> bool {
        let step_index = step.0;
        self.expiring_in(state)
            .binary_search_by_key(&step_index, |candidate| candidate.0)
            .is_ok()
            || self.persisting_state_contains_step(state, step)
    }

    pub(super) fn producer_is_active(
        &self,
        automaton: &PrefixAutomaton,
        node: StyleNodeID,
        producer: PrefixProducer,
    ) -> bool {
        let Some(entering) = self.entering_of(node) else {
            return false;
        };
        automaton.predecessor_of(producer.step).is_none()
            || self.state_contains_step(entering.parent, producer.step)
            || self.state_contains_step(entering.previous, producer.step)
    }

    fn state_membership_across_delta(
        &self,
        old_state: u32,
        new_state: u32,
        delta: Option<PrefixStateDeltaID>,
        arena: &PrefixDeltaArena,
        step: PrefixStepID,
    ) -> (bool, bool) {
        match arena.state_sign(delta, step) {
            1 => (false, true),
            -1 => (true, false),
            0 => {
                let old_contains = self.state_contains_step(old_state, step);
                let new_contains = if delta.is_some() || old_state == new_state {
                    old_contains
                } else {
                    self.state_contains_step(new_state, step)
                };
                (old_contains, new_contains)
            }
            _ => unreachable!("prefix state delta is signed"),
        }
    }

    fn persisting_membership_across_delta(
        &self,
        old_state: u32,
        new_state: u32,
        delta: Option<PrefixStateDeltaID>,
        arena: &PrefixDeltaArena,
        step: PrefixStepID,
    ) -> (bool, bool) {
        match arena.persisting_sign(delta, step) {
            1 => (false, true),
            -1 => (true, false),
            0 => {
                let old_contains = self.persisting_state_contains_step(old_state, step);
                let new_contains = if delta.is_some() || old_state == new_state {
                    old_contains
                } else {
                    self.persisting_state_contains_step(new_state, step)
                };
                (old_contains, new_contains)
            }
            _ => unreachable!("prefix persisting delta is signed"),
        }
    }

    fn collect_delta_steps(&self, delta: PrefixStateDeltaID, arena: &PrefixDeltaArena, into: &mut Vec<PrefixStepID>) {
        let delta = arena.delta(delta);
        for span in [
            delta.persisting_additions,
            delta.persisting_removals,
            delta.expiring_additions,
            delta.expiring_removals,
        ] {
            into.extend_from_slice(arena.get(span));
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn emit_local_output_deltas(
        &self,
        evaluation: &PrefixEvaluation<'_, '_>,
        old_evaluation: &PrefixEvaluation<'_, '_>,
        node: StyleNodeID,
        old: PrefixTransition,
        old_entering: EnteringStates,
        entering: EnteringStates,
        entering_deltas: PrefixEnteringDeltas,
        positional_bits: u32,
        affected: &[PrefixStepID],
        local_producers: Option<&[PrefixProducer]>,
        use_admitted_new_truth: bool,
        arena: &mut PrefixDeltaArena,
        counters: &mut Counters,
    ) -> Result<PrefixLocalOutputDeltas, Incomplete> {
        for scratch in &mut arena.scratch {
            scratch.clear();
        }
        for scratch in &mut arena.match_scratch {
            scratch.clear();
        }
        let old_truth = self.results[old.result.0 as usize].truth;
        let old_positional_bits = self.positional_bits_of(node);
        let mut outputs_changed = false;
        let mut result_changed = false;
        let mut result_delta_complete = true;
        for &step in affected {
            let producer = local_producers.and_then(|producers| {
                producers
                    .binary_search_by_key(&step, |producer| producer.step)
                    .ok()
                    .map(|index| producers[index])
            });
            let is_root = evaluation.automaton.predecessor_of(step).is_none();
            let (old_parent, new_parent) = self.state_membership_across_delta(
                old_entering.parent,
                entering.parent,
                entering_deltas.parent,
                arena,
                step,
            );
            let (old_previous, new_previous) = self.state_membership_across_delta(
                old_entering.previous,
                entering.previous,
                entering_deltas.previous,
                arena,
                step,
            );
            let was_active = is_root || old_parent || old_previous;
            let is_active = is_root || new_parent || new_previous;
            if !was_active && !is_active {
                continue;
            }
            let compound =
                &evaluation.automaton.compounds[evaluation.automaton.steps[step.0 as usize].compound.0 as usize];
            let (old_predicate_matches, new_predicate_matches) = match producer {
                Some(producer) => {
                    let old_matches = match &compound.predicate {
                        PrefixPredicate::Program { .. } => self.truth_in(old_truth).binary_search(&step).is_ok(),
                        PrefixPredicate::Features { .. } => {
                            old_evaluation.step_matches_direct(node, producer.step, counters)?
                        }
                    };
                    let new_matches = if use_admitted_new_truth {
                        let compound_index = evaluation.automaton.steps[step.0 as usize].compound.0 as usize;
                        self.compound_epoch[compound_index] == self.epoch && self.compound_answer[compound_index]
                    } else {
                        evaluation.step_matches_direct(node, producer.step, counters)?
                    };
                    (old_matches, new_matches)
                }
                None => match &compound.predicate {
                    PrefixPredicate::Program { .. } => (
                        self.truth_in(old_truth).binary_search(&step).is_ok(),
                        evaluation.step_matches(node, positional_bits, step, counters)?,
                    ),
                    PrefixPredicate::Features { .. } => (
                        old_evaluation.step_matches(node, old_positional_bits, step, counters)?,
                        evaluation.step_matches(node, positional_bits, step, counters)?,
                    ),
                },
            };
            let old_matches = was_active && old_predicate_matches;
            let new_matches = is_active && new_predicate_matches;
            let local_changed = old_matches != new_matches;
            if local_changed && matches!(compound.predicate, PrefixPredicate::Program { .. }) {
                outputs_changed = true;
                result_changed = true;
                result_delta_complete = false;
            }
            for output in evaluation
                .automaton
                .outputs_for(&evaluation.automaton.steps[step.0 as usize])
            {
                if local_changed {
                    outputs_changed = true;
                    match output.kind {
                        PrefixOutputKind::UniqueTerminal => {
                            let terminal = DispatchEntryID::from_index(output.target as usize);
                            if evaluation
                                .selection
                                .is_none_or(|selection| selection.contains_terminal(terminal))
                            {
                                result_changed = true;
                                arena.match_scratch[usize::from(!new_matches)].push(terminal);
                            }
                        }
                        PrefixOutputKind::SharedTerminal => {
                            let terminal = DispatchEntryID::from_index(output.target as usize);
                            if evaluation
                                .selection
                                .is_none_or(|selection| selection.contains_terminal(terminal))
                            {
                                result_changed = true;
                                result_delta_complete = false;
                            }
                        }
                        _ => {}
                    }
                }
                let target = PrefixStepID(output.target);
                let correction = match output.kind {
                    PrefixOutputKind::Descendant => {
                        let (inherited_old, inherited_new) = self.persisting_membership_across_delta(
                            old_entering.parent,
                            entering.parent,
                            entering_deltas.parent,
                            arena,
                            target,
                        );
                        i8::from(inherited_new || new_matches)
                            - i8::from(inherited_old || old_matches)
                            - (i8::from(inherited_new) - i8::from(inherited_old))
                    }
                    PrefixOutputKind::FollowingSibling => {
                        let (inherited_old, inherited_new) = self.persisting_membership_across_delta(
                            old_entering.previous,
                            entering.previous,
                            entering_deltas.previous,
                            arena,
                            target,
                        );
                        i8::from(inherited_new || new_matches)
                            - i8::from(inherited_old || old_matches)
                            - (i8::from(inherited_new) - i8::from(inherited_old))
                    }
                    PrefixOutputKind::Child | PrefixOutputKind::NextSibling => {
                        i8::from(new_matches) - i8::from(old_matches)
                    }
                    PrefixOutputKind::UniqueTerminal | PrefixOutputKind::SharedTerminal => continue,
                };
                if correction != 0 {
                    outputs_changed = true;
                }
                let index = match (output.kind, correction) {
                    (PrefixOutputKind::Descendant, 1) => 0,
                    (PrefixOutputKind::Descendant, -1) => 1,
                    (PrefixOutputKind::Child, 1) => 2,
                    (PrefixOutputKind::Child, -1) => 3,
                    (PrefixOutputKind::FollowingSibling, 1) => 4,
                    (PrefixOutputKind::FollowingSibling, -1) => 5,
                    (PrefixOutputKind::NextSibling, 1) => 6,
                    (PrefixOutputKind::NextSibling, -1) => 7,
                    (_, 0) => continue,
                    (_, _) => unreachable!("prefix output correction is a signed set delta"),
                };
                arena.scratch[index].push(target);
            }
        }
        for scratch in &mut arena.scratch {
            scratch.sort_unstable();
            scratch.dedup();
        }
        for scratch in &mut arena.match_scratch {
            scratch.sort_unstable();
            scratch.dedup();
        }
        Ok(PrefixLocalOutputDeltas {
            continuation: arena.append_scratch_delta(0),
            right: arena.append_scratch_delta(4),
            matches: arena.append_match_delta(),
            outputs_changed,
            result_changed,
            result_delta_complete,
        })
    }

    #[allow(clippy::too_many_arguments)]
    fn rebase_payload_with_local_delta(
        &mut self,
        source_state: u32,
        old_base_state: u32,
        new_base_state: u32,
        delta: PrefixStateDeltaID,
        arena: &PrefixDeltaArena,
        downward: bool,
        counters: &mut Counters,
    ) -> Option<u32> {
        let mut additions = match downward {
            true => std::mem::take(&mut self.new_descendant),
            false => std::mem::take(&mut self.new_following),
        };
        let mut expiring = match downward {
            true => std::mem::take(&mut self.new_child),
            false => std::mem::take(&mut self.new_adjacent),
        };
        additions.clear();
        expiring.clear();
        if source_state != old_base_state {
            let source = &self.states[source_state as usize];
            if source.base != old_base_state {
                match downward {
                    true => {
                        self.new_descendant = additions;
                        self.new_child = expiring;
                    }
                    false => {
                        self.new_following = additions;
                        self.new_adjacent = expiring;
                    }
                }
                return None;
            }
            additions.extend_from_slice(self.additions_in(source_state));
            expiring.extend_from_slice(self.expiring_in(source_state));
        }
        let delta = arena.delta(delta);
        additions
            .retain(|&step| arena.sign_in_spans(delta.persisting_additions, delta.persisting_removals, step) != -1);
        additions.extend_from_slice(arena.get(delta.persisting_additions));
        additions.retain(|&step| !self.persisting_state_contains_step(new_base_state, step));
        additions.sort_unstable();
        additions.dedup();
        expiring.retain(|&step| arena.sign_in_spans(delta.expiring_additions, delta.expiring_removals, step) != -1);
        expiring.extend_from_slice(arena.get(delta.expiring_additions));
        expiring.sort_unstable();
        expiring.dedup();
        let additions_hash = additions
            .iter()
            .fold(0_u64, |hash, &step| hash.wrapping_add(step_hash(step)));
        let expiring_hash = expiring
            .iter()
            .fold(0_u32, |hash, &step| hash.wrapping_add(step_hash(step) as u32));
        let state = self.intern_extended_state(
            new_base_state,
            &additions,
            additions_hash,
            &expiring,
            expiring_hash,
            counters,
        );
        match downward {
            true => {
                self.new_descendant = additions;
                self.new_child = expiring;
            }
            false => {
                self.new_following = additions;
                self.new_adjacent = expiring;
            }
        }
        Some(state)
    }

    fn apply_local_match_delta(
        &mut self,
        old_result: PrefixResultID,
        delta: PrefixMatchDelta,
        arena: &PrefixDeltaArena,
        counters: &mut Counters,
    ) -> PrefixResultID {
        if delta.additions.len == 0 && delta.removals.len == 0 {
            return old_result;
        }
        let old_result_value = self.results[old_result.0 as usize];
        let mut matches = std::mem::take(&mut self.output_matches);
        matches.clear();
        matches.extend_from_slice(self.matches_in(old_result_value.matches));
        let removals = arena.get_matches(delta.removals);
        matches.retain(|entry| removals.binary_search(entry).is_err());
        matches.extend_from_slice(arena.get_matches(delta.additions));
        matches.sort_unstable();
        matches.dedup();
        self.output_matches = matches;
        let matches = self.intern_output_matches(counters);
        self.intern_result(matches, old_result_value.truth)
    }

    #[allow(clippy::too_many_arguments)]
    fn try_sparse_retained_local_transition(
        &mut self,
        evaluation: &PrefixEvaluation<'_, '_>,
        node: StyleNodeID,
        row: u32,
        old: PrefixTransition,
        entering: EnteringStates,
        entering_deltas: PrefixEnteringDeltas,
        local_facts: u32,
        positional_bits: u32,
        local_output_deltas: PrefixLocalOutputDeltas,
        delta_arena: &PrefixDeltaArena,
        counters: &mut Counters,
    ) -> Option<PrefixTransition> {
        let old_entering = self.entering_of(node)?;
        if entering_deltas.parent.is_none() && entering_deltas.previous.is_none() {
            return None;
        }
        if self.local_facts_of(node) != Some(local_facts) || self.positional_bits_of(node) != positional_bits {
            return None;
        }
        if local_output_deltas.result_changed && !local_output_deltas.result_delta_complete {
            return None;
        }
        let (state, right) = if local_output_deltas.outputs_changed {
            (
                self.rebase_payload_with_local_delta(
                    old.state,
                    old_entering.parent,
                    entering.parent,
                    local_output_deltas.continuation,
                    delta_arena,
                    true,
                    counters,
                )?,
                self.rebase_payload_with_local_delta(
                    old.right,
                    old_entering.previous,
                    entering.previous,
                    local_output_deltas.right,
                    delta_arena,
                    false,
                    counters,
                )?,
            )
        } else {
            (
                self.rebase_unchanged_local_payload(old.state, old_entering.parent, entering.parent, counters)?,
                self.rebase_unchanged_local_payload(old.right, old_entering.previous, entering.previous, counters)?,
            )
        };
        let result = match local_output_deltas.result_changed {
            true => self.apply_local_match_delta(old.result, local_output_deltas.matches, delta_arena, counters),
            false => old.result,
        };
        let transition = PrefixTransition { state, right, result };
        self.transitions.insert(
            PrefixTransitionKey {
                parent: entering.parent,
                previous: entering.previous,
                local_facts,
                is_document_root: evaluation.tree.parent(node).is_none(),
                positional_bits,
            },
            transition,
        );
        if !evaluation.automaton.positional_tests().is_empty() {
            self.set_positional_bits(node, positional_bits);
        }
        self.set_entering(node, entering);
        self.transition_by_row[row as usize] = transition;
        self.set_transition(node, transition);
        Some(transition)
    }

    /// Compare one node's transition against the retained one and update the cache.
    ///
    /// `selection` narrows difference detection to the rules the flush routed; a topology flush
    /// passes `None` because a tree change can move matches no route named.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn compare_and_update(
        &mut self,
        evaluation: &PrefixEvaluation<'_, '_>,
        old_evaluation: &PrefixEvaluation<'_, '_>,
        selection: Option<&PrefixSelection>,
        node: StyleNodeID,
        local_facts_changed: bool,
        local_affected_candidates: Option<&[PrefixProducer]>,
        entering_deltas: PrefixEnteringDeltas,
        delta_arena: &mut PrefixDeltaArena,
        counters: &mut Counters,
    ) -> PrefixTransitionLookup<PrefixDifference> {
        self.prepare_rows(evaluation.facts.generation(), evaluation.facts.row_count());
        let Some(row) = evaluation.facts.row_of(node) else {
            return PrefixTransitionLookup::Missing(PrefixTransitionGap::Incomplete(Incomplete::MissingFacts(node)));
        };
        let old = match self.transition_of(node) {
            PrefixTransitionLookup::Known(transition) => Some(transition),
            PrefixTransitionLookup::Missing(_) => None,
        };
        let parent_state = match evaluation.tree.parent(node) {
            Some(parent) => match self.transition_for(evaluation, parent, counters) {
                PrefixTransitionLookup::Known(transition) => transition.state,
                PrefixTransitionLookup::Missing(gap) => return PrefixTransitionLookup::Missing(gap),
            },
            None => 0,
        };
        let previous_state = if evaluation.automaton.has_sibling_steps() {
            match evaluation.tree.previous_element_sibling(node) {
                Some(previous) => match self.transition_for(evaluation, previous, counters) {
                    PrefixTransitionLookup::Known(transition) => transition.right,
                    PrefixTransitionLookup::Missing(gap) => return PrefixTransitionLookup::Missing(gap),
                },
                None => 0,
            }
        } else {
            0
        };
        let local_facts = if local_facts_changed || old.is_none() {
            let identity = self.local_fact_interner.intern(evaluation.facts, row, counters);
            self.set_local_facts(node, identity);
            identity
        } else {
            let Some(local_facts) = self.local_facts_of(node) else {
                return PrefixTransitionLookup::Missing(PrefixTransitionGap::MissingTransition(node));
            };
            local_facts
        };
        let positional_bits = match evaluation.positional_bits(node, counters) {
            Ok(bits) => bits,
            Err(incomplete) => return PrefixTransitionLookup::Missing(PrefixTransitionGap::Incomplete(incomplete)),
        };
        let entering = EnteringStates {
            parent: parent_state,
            previous: previous_state,
        };
        let old_entering = self.entering_of(node);
        // Interner give-back can strand a node with a remapped transition but no surviving
        // entering states: the entering names the parent's old state, which dies with a forgotten
        // parent transition. Local output deltas replay from the old entering, so without one the
        // difference has to come from comparing the interned state chains instead.
        let can_derive_delta = selection.is_some()
            && (entering_deltas.parent.is_some()
                || entering_deltas.previous.is_some()
                || local_affected_candidates.is_some())
            && (!local_facts_changed || local_affected_candidates.is_some())
            && old_entering.is_some();
        let mut affected_candidates = std::mem::take(&mut self.candidates);
        affected_candidates.clear();
        for delta in [entering_deltas.parent, entering_deltas.previous].into_iter().flatten() {
            self.collect_delta_steps(delta, delta_arena, &mut affected_candidates);
        }
        for delta in [entering_deltas.parent, entering_deltas.previous].into_iter().flatten() {
            let delta = delta_arena.delta(delta);
            for span in [delta.persisting_additions, delta.persisting_removals] {
                for &step in delta_arena.get(span) {
                    if let Some(predecessor) = evaluation.automaton.predecessor_of(step) {
                        affected_candidates.push(predecessor);
                    }
                }
            }
        }
        if local_facts_changed && let Some(local_affected_candidates) = local_affected_candidates {
            let mut active_candidates = std::mem::take(&mut self.compare_left);
            active_candidates.clear();
            for active in [
                old_entering.map(|entering| entering.parent),
                old_entering.map(|entering| entering.previous),
                Some(entering.parent),
                Some(entering.previous),
            ]
            .into_iter()
            .flatten()
            {
                self.collect_active(active, &mut active_candidates);
            }
            let mut append_roots = |facts: &StyleNodeFacts| {
                let Some(row) = facts.row_of(node) else {
                    return;
                };
                facts.for_each_dispatch_probe(row, evaluation.tree.parent(node).is_none(), |key, _| {
                    if let Some(bucket) = evaluation.automaton.bucket(key) {
                        active_candidates.extend_from_slice(&bucket.root_steps);
                    }
                });
            };
            append_roots(old_evaluation.facts);
            append_roots(evaluation.facts);
            active_candidates.retain(|step| {
                local_affected_candidates
                    .binary_search_by_key(step, |producer| producer.step)
                    .is_ok()
            });
            affected_candidates.extend_from_slice(&active_candidates);
            self.compare_left = active_candidates;
        }
        affected_candidates.sort_unstable();
        affected_candidates.dedup();
        let is_document_root = evaluation.tree.parent(node).is_none();
        affected_candidates.retain(|&step| {
            local_affected_candidates
                .is_some_and(|producers| producers.binary_search_by_key(&step, |producer| producer.step).is_ok())
                || evaluation.facts.carries_dispatch_key(
                    row,
                    evaluation.automaton.compounds[evaluation.automaton.steps[step.0 as usize].compound.0 as usize]
                        .dispatch_key,
                    is_document_root,
                )
        });
        let local_output_deltas = if can_derive_delta && !local_facts_changed {
            old.zip(old_entering).map(|(old, old_entering)| {
                self.emit_local_output_deltas(
                    evaluation,
                    old_evaluation,
                    node,
                    old,
                    old_entering,
                    entering,
                    entering_deltas,
                    positional_bits,
                    &affected_candidates,
                    local_affected_candidates,
                    false,
                    delta_arena,
                    counters,
                )
            })
        } else {
            None
        };
        let mut local_output_deltas = match local_output_deltas.transpose() {
            Ok(deltas) => deltas,
            Err(incomplete) => {
                self.candidates = affected_candidates;
                return PrefixTransitionLookup::Missing(PrefixTransitionGap::Incomplete(incomplete));
            }
        };
        let sparse = if !local_facts_changed && selection.is_some() {
            match (old, local_output_deltas) {
                (Some(old), Some(local_output_deltas)) => self.try_sparse_retained_local_transition(
                    evaluation,
                    node,
                    row,
                    old,
                    entering,
                    entering_deltas,
                    local_facts,
                    positional_bits,
                    local_output_deltas,
                    delta_arena,
                    counters,
                ),
                _ => None,
            }
        } else {
            None
        };
        let used_sparse_transition = sparse.is_some();
        let (new, use_admitted_new_truth) = if let Some(transition) = sparse {
            (transition, false)
        } else {
            let mut surface = PrefixTransitionSurface::Retained(self);
            let (transition, origin) = match surface.transition_from_inputs(
                evaluation,
                node,
                row,
                TransitionInputs {
                    entering: EnteringStates {
                        parent: entering.parent,
                        previous: entering.previous,
                    },
                    local_facts,
                    positional_bits,
                },
                counters,
            ) {
                PrefixTransitionLookup::Known(transition) => transition,
                PrefixTransitionLookup::Missing(gap) => return PrefixTransitionLookup::Missing(gap),
            };
            surface.remember_transition(node, row, transition);
            (transition, origin == PrefixTransitionOrigin::Computed)
        };

        let Some(old) = old else {
            if evaluation.tree.children(node).next().is_some() {
                self.complete = false;
            }
            return PrefixTransitionLookup::Known(PrefixDifference {
                continuation_changed: true,
                right_changed: true,
                matches_changed: true,
                arrived: true,
                old_matches: self.matches_of(new),
                new_matches: self.matches_of(new),
                continuation_delta: None,
                right_delta: None,
            });
        };
        if local_output_deltas.is_none()
            && can_derive_delta
            && let Some(old_entering) = old_entering
        {
            local_output_deltas = match self.emit_local_output_deltas(
                evaluation,
                old_evaluation,
                node,
                old,
                old_entering,
                entering,
                entering_deltas,
                positional_bits,
                &affected_candidates,
                local_affected_candidates,
                use_admitted_new_truth,
                delta_arena,
                counters,
            ) {
                Ok(deltas) => Some(deltas),
                Err(incomplete) => {
                    self.candidates = affected_candidates;
                    return PrefixTransitionLookup::Missing(PrefixTransitionGap::Incomplete(incomplete));
                }
            };
        }
        let sparse_has_local_output_delta = used_sparse_transition && local_output_deltas.unwrap().outputs_changed;
        // An exact local delta is also an equality proof. Avoid walking both interned state
        // chains after doing the more precise work needed for downstream propagation already.
        let (continuation_changed, continuation_delta) = if old.state == new.state {
            (false, None)
        } else if can_derive_delta {
            let delta = if used_sparse_transition && !sparse_has_local_output_delta {
                delta_arena.persisting_only(entering_deltas.parent)
            } else {
                delta_arena.combine(entering_deltas.parent, local_output_deltas.unwrap().continuation)
            };
            let changed = delta_arena.changes_selected_state(delta, selection.unwrap());
            (changed, changed.then_some(delta))
        } else {
            (!self.selected_states_equal(old.state, new.state, selection), None)
        };
        let (right_changed, right_delta) = if old.right == new.right {
            (false, None)
        } else if can_derive_delta {
            let delta = if used_sparse_transition && !sparse_has_local_output_delta {
                delta_arena.persisting_only(entering_deltas.previous)
            } else {
                delta_arena.combine(entering_deltas.previous, local_output_deltas.unwrap().right)
            };
            let changed = delta_arena.changes_selected_state(delta, selection.unwrap());
            (changed, changed.then_some(delta))
        } else {
            (!self.selected_states_equal(old.right, new.right, selection), None)
        };
        let old_matches = self.matches_of(old);
        let new_matches = self.matches_of(new);
        self.candidates = affected_candidates;
        PrefixTransitionLookup::Known(PrefixDifference {
            continuation_changed,
            right_changed,
            matches_changed: match selection {
                Some(selection) => {
                    !selected_matches_equal(self.matches_in(old_matches), self.matches_in(new_matches), selection)
                }
                // Match sets are content-interned, so identity is content equality.
                None => old_matches != new_matches,
            },
            arrived: false,
            old_matches,
            new_matches,
            continuation_delta,
            right_delta,
        })
    }

    pub(super) fn complete_nodes_with_budget(
        &mut self,
        evaluation: &PrefixEvaluation<'_, '_>,
        nodes: impl Iterator<Item = StyleNodeID>,
        completion_budget: usize,
        counters: &mut Counters,
    ) -> bool {
        let mut completions = 0;
        for node in nodes {
            if matches!(self.transition_of(node), PrefixTransitionLookup::Known(_)) {
                continue;
            }
            if completions == completion_budget {
                self.complete = false;
                return false;
            }
            match self.transition_for(evaluation, node, counters) {
                PrefixTransitionLookup::Known(_) => {}
                PrefixTransitionLookup::Missing(_) => {
                    self.complete = false;
                    return false;
                }
            }
            completions += 1;
        }
        self.complete = true;
        true
    }

    /// Drop every row-indexed answer the moment the batch's row space moves.
    ///
    /// Per-row transitions and local-fact representatives hold raw row indices. A batch rebuild
    /// renumbers rows, so holding them across generations reads the wrong element's facts, or
    /// past the end of the batch. Appending keeps the generation and the held rows.
    fn prepare_rows(&mut self, generation: u64, row_count: usize) {
        if self.facts_generation != generation || generation == 0 {
            self.facts_generation = generation;
            self.transition_by_row.clear();
            self.local_fact_interner.clear_rows();
        }
        if self.transition_by_row.len() != row_count {
            self.transition_by_row.clear();
            self.transition_by_row.resize(row_count, UNKNOWN_TRANSITION);
        }
    }

    fn rebuild_interners(&mut self) {
        if self.states.len() > 1 && self.states_by_hash.is_empty() {
            for state in 1..self.states.len() {
                let contents = &self.states[state];
                let base_hash = self.states[contents.base as usize].descendant_hash;
                let hash = state_structural_hash(
                    contents.base,
                    contents.additions_len,
                    contents.descendant_hash.wrapping_sub(base_hash),
                    contents.expiring_len,
                    contents.expiring_hash,
                );
                self.states_by_hash.insert_identity(hash, PrefixStateID(state as u32));
            }
        }
        if self.match_offsets.len() > 2 && self.match_sets_by_hash.is_empty() {
            for matches in 1..self.match_offsets.len() - 1 {
                let identity = PrefixMatchSetID(matches as u32);
                let mut hasher = fast_hasher();
                self.matches_in(identity).hash(&mut hasher);
                self.match_sets_by_hash.insert_identity(hasher.finish(), identity);
            }
        }
        if self.truth_offsets.len() > 2 && self.truth_sets_by_hash.is_empty() {
            for truth in 1..self.truth_offsets.len() - 1 {
                let identity = PrefixTruthSetID(truth as u32);
                let mut hasher = fast_hasher();
                self.truth_in(identity).hash(&mut hasher);
                self.truth_sets_by_hash.insert_identity(hasher.finish(), identity);
            }
        }
    }

    fn transition_for(
        &mut self,
        evaluation: &PrefixEvaluation<'_, '_>,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> PrefixTransitionLookup<PrefixTransition> {
        transition_for(&mut PrefixTransitionSurface::Retained(self), evaluation, node, counters)
    }

    #[allow(clippy::too_many_arguments)]
    fn transition(
        &mut self,
        evaluation: &PrefixEvaluation<'_, '_>,
        entering: EnteringStates,
        row: u32,
        node: StyleNodeID,
        is_document_root: bool,
        positional_bits: u32,
        counters: &mut Counters,
    ) -> PrefixTransitionLookup<PrefixTransition> {
        let automaton = evaluation.automaton;
        if !self.automaton_statistics_recorded {
            self.automaton_step_count = automaton.steps.len();
            self.automaton_statistics_recorded = true;
        }
        self.begin_transition(automaton);
        self.enter_states(entering);

        evaluation
            .facts
            .for_each_dispatch_probe(row, is_document_root, |key, _| {
                self.offer_key(automaton, entering, key, evaluation.selection, counters);
            });

        for candidate_index in 0..self.candidates.len() {
            let step_id = self.candidates[candidate_index];
            let step = &automaton.steps[step_id.0 as usize];
            let compound_index = step.compound.0 as usize;
            let matches = if self.compound_epoch[compound_index] == self.epoch {
                self.compound_answer[compound_index]
            } else {
                let compound = &automaton.compounds[compound_index];
                let matches = match &compound.predicate {
                    PrefixPredicate::Features {
                        feature_start,
                        feature_len,
                        required_positional_bits,
                    } => {
                        (positional_bits & required_positional_bits) == *required_positional_bits
                            && automaton
                                .features_for(*feature_start, *feature_len)
                                .iter()
                                .all(|&feature| matches_feature(evaluation.facts, row, feature))
                    }
                    PrefixPredicate::Program { program, local } => match evaluation.evaluator.matches_prefix_local(
                        *program,
                        evaluation.programs.get(*program),
                        *local,
                        node,
                        counters,
                    ) {
                        Ok(matches) => matches,
                        Err(incomplete) => {
                            return PrefixTransitionLookup::Missing(PrefixTransitionGap::Incomplete(incomplete));
                        }
                    },
                };
                self.compound_epoch[compound_index] = self.epoch;
                self.compound_answer[compound_index] = matches;
                counters.bump(Counter::PrefixCompoundsEvaluated);
                matches
            };
            if !matches {
                continue;
            }
            if matches!(
                automaton.compounds[compound_index].predicate,
                PrefixPredicate::Program { .. }
            ) {
                self.output_matched_steps.push(step_id);
            }
            for output in automaton.outputs_for(step) {
                match output.kind {
                    PrefixOutputKind::UniqueTerminal | PrefixOutputKind::SharedTerminal => {
                        let terminal = DispatchEntryID::from_index(output.target as usize);
                        if evaluation
                            .selection
                            .is_none_or(|selection| selection.contains_terminal(terminal))
                        {
                            self.admit_match(terminal);
                        }
                    }
                    kind => {
                        let successor = PrefixStepID(output.target);
                        if evaluation
                            .selection
                            .is_some_and(|selection| !selection.contains_step(successor))
                        {
                            continue;
                        }
                        let axis = match kind {
                            PrefixOutputKind::Child => SelectorPrefixAxis::Child,
                            PrefixOutputKind::Descendant => SelectorPrefixAxis::Descendant,
                            PrefixOutputKind::NextSibling => SelectorPrefixAxis::NextSibling,
                            PrefixOutputKind::FollowingSibling => SelectorPrefixAxis::FollowingSibling,
                            PrefixOutputKind::UniqueTerminal | PrefixOutputKind::SharedTerminal => unreachable!(),
                        };
                        self.admit_successor(successor, axis);
                    }
                }
            }
        }

        if !self.output_matches.is_sorted() {
            self.output_matches.sort_unstable();
        }
        if !self.output_matched_steps.is_sorted() {
            self.output_matched_steps.sort_unstable();
        }
        let state = self.intern_transition_state(automaton, entering.parent, counters);
        let right = self.intern_right_transition_state(automaton, entering.previous, counters);
        let matches = self.intern_output_matches(counters);
        let truth = self.intern_output_truth();
        let result = self.intern_result(matches, truth);
        PrefixTransitionLookup::Known(PrefixTransition { state, right, result })
    }

    fn begin_transition(&mut self, automaton: &PrefixAutomaton) {
        let step_count = automaton.steps.len();
        let compound_count = automaton.compounds.len();
        // NB: Each scratch column carries its own length gate. Retention trims every column to
        //     empty, but comparison and sparse-delta paths regrow candidate_epoch on their own,
        //     so a shared gate on candidate_epoch alone would leave the sibling columns empty
        //     and the persisting-output walk indexing into them out of bounds.
        self.candidate_epoch.ensure_len(step_count);
        self.output_epoch.ensure_len(step_count);
        self.parent_persisting_epoch.ensure_len(step_count);
        self.previous_persisting_epoch.ensure_len(step_count);
        self.compound_epoch.ensure_len(compound_count);
        if self.compound_answer.len() < compound_count {
            self.compound_answer.resize(compound_count, false);
        }
        advance_epoch(
            &mut self.epoch,
            1,
            &mut [
                &mut self.candidate_epoch,
                &mut self.compound_epoch,
                &mut self.output_epoch,
                &mut self.match_epoch,
                &mut self.parent_persisting_epoch,
                &mut self.previous_persisting_epoch,
            ],
        );
        self.candidates.clear();
        self.output_matches.clear();
        self.output_matched_steps.clear();
        self.new_descendant.clear();
        self.new_child.clear();
        self.new_following.clear();
        self.new_adjacent.clear();
        self.new_descendant_hash = 0;
        self.new_child_hash = 0;
        self.new_following_hash = 0;
        self.new_adjacent_hash = 0;
    }

    /// Mark both entering states' persisting membership so admissions dedup in O(1).
    fn enter_states(&mut self, entering: EnteringStates) {
        let mut marks = std::mem::take(&mut self.parent_persisting_epoch);
        self.enter_state(entering.parent, &mut marks);
        self.parent_persisting_epoch = marks;
        let mut marks = std::mem::take(&mut self.previous_persisting_epoch);
        self.enter_state(entering.previous, &mut marks);
        self.previous_persisting_epoch = marks;
    }

    fn enter_state(&self, state: u32, persisting_marks: &mut [u32]) {
        let mut current = state;
        loop {
            for &step in self.additions_in(current) {
                persisting_marks[step.0 as usize] = self.epoch;
            }
            let base = self.states[current as usize].base;
            if base == current {
                break;
            }
            current = base;
        }
    }

    /// Offer every root step dispatching under one of the row's keys. Buckets hold only root
    /// steps, which are enabled unconditionally; continuation steps are candidates through the
    /// parent state's active set instead.
    fn offer_key(
        &mut self,
        automaton: &PrefixAutomaton,
        entering: EnteringStates,
        key: DispatchKey,
        selection: Option<&PrefixSelection>,
        _counters: &mut Counters,
    ) {
        let Some(bucket) = automaton.bucket(key) else {
            return;
        };
        for &step in &bucket.root_steps {
            let index = step.0 as usize;
            if selection.is_some_and(|selection| !selection.contains_step(step)) {
                continue;
            }
            if self.candidate_epoch[index] != self.epoch {
                self.candidate_epoch[index] = self.epoch;
                self.candidates.push(step);
            }
        }
        if entering.parent != 0 {
            self.offer_state_range(entering.parent, bucket.first_step, bucket.end_step, selection);
        }
        if entering.previous != 0 && entering.previous != entering.parent {
            self.offer_state_range(entering.previous, bucket.first_step, bucket.end_step, selection);
        }
    }

    fn offer_state_range(&mut self, state: u32, first_step: u32, end_step: u32, selection: Option<&PrefixSelection>) {
        let contents = &self.states[state as usize];
        self.offer_step_range(
            contents.payload_start as usize + contents.additions_len as usize,
            contents.expiring_len,
            (first_step, end_step),
            selection,
        );
        let mut current = state;
        loop {
            let contents = &self.states[current as usize];
            let payload_start = contents.payload_start;
            let additions_len = contents.additions_len;
            let base = contents.base;
            self.offer_step_range(payload_start as usize, additions_len, (first_step, end_step), selection);
            if base == current {
                break;
            }
            current = base;
        }
    }

    fn offer_step_range(
        &mut self,
        payload_start: usize,
        len: u32,
        dispatch_range: (u32, u32),
        selection: Option<&PrefixSelection>,
    ) {
        // SAFETY: Callers pass a range from an interned state's initialized payload.
        let steps = unsafe {
            std::slice::from_raw_parts(
                self.delta_steps.as_ptr().add(payload_start).cast::<PrefixStepID>(),
                len as usize,
            )
        };
        let start = steps.partition_point(|step| step.0 < dispatch_range.0);
        let end = steps[start..].partition_point(|step| step.0 < dispatch_range.1) + start;
        for &step in &steps[start..end] {
            if selection.is_some_and(|selection| !selection.contains_step(step)) {
                continue;
            }
            let candidate = &mut self.candidate_epoch[step.0 as usize];
            if *candidate != self.epoch {
                *candidate = self.epoch;
                self.candidates.push(step);
            }
        }
    }

    fn admit_successor(&mut self, step: PrefixStepID, axis: SelectorPrefixAxis) {
        let seen = &mut self.output_epoch[step.0 as usize];
        if *seen == self.epoch {
            return;
        }
        *seen = self.epoch;
        let hash = step_hash(step);
        match axis {
            SelectorPrefixAxis::Child => {
                self.new_child.push(step);
                self.new_child_hash = self.new_child_hash.wrapping_add(hash as u32);
            }
            SelectorPrefixAxis::Descendant => {
                self.new_descendant.push(step);
                self.new_descendant_hash = self.new_descendant_hash.wrapping_add(hash);
            }
            SelectorPrefixAxis::NextSibling => {
                self.new_adjacent.push(step);
                self.new_adjacent_hash = self.new_adjacent_hash.wrapping_add(hash as u32);
            }
            SelectorPrefixAxis::FollowingSibling => {
                self.new_following.push(step);
                self.new_following_hash = self.new_following_hash.wrapping_add(hash);
            }
            SelectorPrefixAxis::Root => unreachable!("root steps are dispatched, never admitted"),
        }
    }

    fn admit_match(&mut self, entry: DispatchEntryID) {
        let index = entry.index();
        if self.match_epoch.mark(index, self.epoch) {
            self.output_matches.push(entry);
        }
    }

    /// Intern the transition's downward output state from the parent state and the new
    /// down-axis admissions.
    fn intern_transition_state(
        &mut self,
        automaton: &PrefixAutomaton,
        parent_state: u32,
        counters: &mut Counters,
    ) -> u32 {
        let mut additions = std::mem::take(&mut self.new_descendant);
        let mut additions_hash = self.new_descendant_hash;
        additions.retain(|step| {
            if self.parent_persisting_epoch[step.0 as usize] != self.epoch {
                true
            } else {
                additions_hash = additions_hash.wrapping_sub(step_hash(*step));
                false
            }
        });
        automaton.sort_steps_by_dispatch_order(&mut additions);
        let mut expiring = std::mem::take(&mut self.new_child);
        automaton.sort_steps_by_dispatch_order(&mut expiring);
        let state = self.intern_extended_state(
            parent_state,
            &additions,
            additions_hash,
            &expiring,
            self.new_child_hash,
            counters,
        );
        self.new_descendant = additions;
        self.new_child = expiring;
        state
    }

    /// Intern the transition's rightward output state from the previous sibling's rightward
    /// state and the new sibling-axis admissions.
    fn intern_right_transition_state(
        &mut self,
        automaton: &PrefixAutomaton,
        previous_state: u32,
        counters: &mut Counters,
    ) -> u32 {
        let mut additions = std::mem::take(&mut self.new_following);
        let mut additions_hash = self.new_following_hash;
        additions.retain(|step| {
            if self.previous_persisting_epoch[step.0 as usize] != self.epoch {
                true
            } else {
                additions_hash = additions_hash.wrapping_sub(step_hash(*step));
                false
            }
        });
        automaton.sort_steps_by_dispatch_order(&mut additions);
        let mut expiring = std::mem::take(&mut self.new_adjacent);
        automaton.sort_steps_by_dispatch_order(&mut expiring);
        let state = self.intern_extended_state(
            previous_state,
            &additions,
            additions_hash,
            &expiring,
            self.new_adjacent_hash,
            counters,
        );
        self.new_following = additions;
        self.new_adjacent = expiring;
        state
    }

    /// Intern the state extending `base_state` with the given sorted persisting additions and
    /// sorted expiring steps.
    ///
    /// The additions were deduplicated against the base's persisting set through the entering
    /// marks, so the state stores exactly its delta and extends the order-independent persisting
    /// hash by exactly the genuinely new steps. A transition that admitted nothing keeps the
    /// base's persisting part by identity, which is the base itself unless it carried expiring
    /// steps.
    fn intern_extended_state(
        &mut self,
        base_state: u32,
        additions: &[PrefixStepID],
        additions_hash: u64,
        expiring: &[PrefixStepID],
        expiring_hash: u32,
        counters: &mut Counters,
    ) -> u32 {
        if additions.is_empty() && expiring.is_empty() {
            if self.states[base_state as usize].expiring_len == 0 {
                return base_state;
            }
            return self.descendant_only_state(base_state, counters);
        }
        let additions_len = u32::try_from(additions.len()).expect("selector prefix state payload overflow");
        let expiring_len = u32::try_from(expiring.len()).expect("selector prefix state payload overflow");
        let hash = state_structural_hash(base_state, additions_len, additions_hash, expiring_len, expiring_hash);
        if let Some(candidate) = self.states_by_hash.find(hash, |candidate, ()| {
            self.states[candidate.0 as usize].base == base_state
                && self.additions_in(candidate.0) == additions
                && self.expiring_in(candidate.0) == expiring
        }) {
            return candidate.0;
        }

        let base = &self.states[base_state as usize];
        let descendant_len = base.descendant_len + additions_len;
        let descendant_hash = base.descendant_hash.wrapping_add(additions_hash);
        let state = u32::try_from(self.states.len()).expect("selector prefix state space exhausted");
        let payload_start = u32::try_from(self.delta_steps.len()).expect("selector prefix state payload overflow");
        self.append_delta_steps(additions);
        self.append_delta_steps(expiring);
        self.states.push(PrefixState {
            base: base_state,
            payload_start,
            additions_len,
            expiring_len,
            descendant_len,
            descendant_hash,
            expiring_hash,
        });
        self.descendant_only.push(match expiring.is_empty() {
            true => state,
            false => UNKNOWN_STATE,
        });
        self.states_by_hash.insert_identity(hash, PrefixStateID(state));
        state
    }

    fn append_delta_steps(&mut self, steps: &[PrefixStepID]) {
        self.delta_steps
            .extend(steps.iter().map(|&step| std::mem::MaybeUninit::new(step)));
    }

    fn skip_delta_steps(&mut self, additions_len: usize, expiring_len: usize) {
        for len in [additions_len, expiring_len] {
            self.delta_steps.reserve(len);
            let new_len = self
                .delta_steps
                .len()
                .checked_add(len)
                .expect("selector prefix state payload overflow");
            // SAFETY: `MaybeUninit<PrefixStepID>` permits uninitialized elements. No state
            // references this skipped range; a sparse rebase shares its source payload.
            unsafe { self.delta_steps.set_len(new_len) };
        }
    }

    fn rebase_unchanged_local_payload(
        &mut self,
        source_state: u32,
        old_base_state: u32,
        new_base_state: u32,
        counters: &mut Counters,
    ) -> Option<u32> {
        if old_base_state == new_base_state {
            return Some(source_state);
        }
        if source_state == old_base_state {
            return Some(match self.states[new_base_state as usize].expiring_len == 0 {
                true => new_base_state,
                false => self.descendant_only_state(new_base_state, counters),
            });
        }
        let source = &self.states[source_state as usize];
        if source.base != old_base_state {
            return None;
        }
        let additions_len = source.additions_len;
        let expiring_len = source.expiring_len;
        let payload_start = source.payload_start;
        let expiring_hash = source.expiring_hash;
        let additions_hash = source
            .descendant_hash
            .wrapping_sub(self.states[old_base_state as usize].descendant_hash);
        let new_base = &self.states[new_base_state as usize];
        let descendant_len = new_base
            .descendant_len
            .checked_add(additions_len)
            .expect("selector prefix state payload overflow");
        let descendant_hash = new_base.descendant_hash.wrapping_add(additions_hash);
        let hash = state_structural_hash(
            new_base_state,
            additions_len,
            additions_hash,
            expiring_len,
            expiring_hash,
        );
        if let Some(candidate) = self.states_by_hash.find(hash, |candidate, ()| {
            self.states[candidate.0 as usize].base == new_base_state
                && self.additions_in(candidate.0) == self.additions_in(source_state)
                && self.expiring_in(candidate.0) == self.expiring_in(source_state)
        }) {
            return Some(candidate.0);
        }
        let state = u32::try_from(self.states.len()).expect("selector prefix state space exhausted");
        self.skip_delta_steps(additions_len as usize, expiring_len as usize);
        self.states.push(PrefixState {
            base: new_base_state,
            payload_start,
            additions_len,
            expiring_len,
            descendant_len,
            descendant_hash,
            expiring_hash,
        });
        self.descendant_only.push(match expiring_len == 0 {
            true => state,
            false => UNKNOWN_STATE,
        });
        self.states_by_hash.insert_identity(hash, PrefixStateID(state));
        Some(state)
    }

    /// The interned state holding exactly `state`'s persisting part, memoized per state.
    fn descendant_only_state(&mut self, state: u32, counters: &mut Counters) -> u32 {
        let known = self.descendant_only[state as usize];
        if known != UNKNOWN_STATE {
            return known;
        }
        let mut additions = std::mem::take(&mut self.compare_left);
        additions.clear();
        additions.extend_from_slice(self.additions_in(state));
        let base = self.states[state as usize].base;
        let additions_hash = self.states[state as usize]
            .descendant_hash
            .wrapping_sub(self.states[base as usize].descendant_hash);
        let interned = self.intern_extended_state(base, &additions, additions_hash, &[], 0, counters);
        additions.clear();
        self.compare_left = additions;
        self.descendant_only[state as usize] = interned;
        interned
    }

    fn intern_output_matches(&mut self, _counters: &mut Counters) -> PrefixMatchSetID {
        let mut hasher = fast_hasher();
        self.output_matches.hash(&mut hasher);
        let hash = hasher.finish();
        if let Some(candidate) = self
            .match_sets_by_hash
            .find(hash, |candidate, ()| self.matches_in(candidate) == self.output_matches)
        {
            return candidate;
        }

        let matches = PrefixMatchSetID(
            u32::try_from(self.match_offsets.len() - 1).expect("selector prefix match-set space exhausted"),
        );
        self.match_entries.extend_from_slice(&self.output_matches);
        self.match_offsets
            .push(u32::try_from(self.match_entries.len()).expect("selector prefix match-set payload overflow"));
        self.match_sets_by_hash.insert_identity(hash, matches);
        matches
    }

    fn intern_output_truth(&mut self) -> PrefixTruthSetID {
        let mut hasher = fast_hasher();
        self.output_matched_steps.hash(&mut hasher);
        let hash = hasher.finish();
        if let Some(candidate) = self.truth_sets_by_hash.find(hash, |candidate, ()| {
            self.truth_in(candidate) == self.output_matched_steps
        }) {
            return candidate;
        }

        let truth = PrefixTruthSetID(
            u32::try_from(self.truth_offsets.len() - 1).expect("selector prefix truth-set space exhausted"),
        );
        self.truth_steps.extend_from_slice(&self.output_matched_steps);
        self.truth_offsets
            .push(u32::try_from(self.truth_steps.len()).expect("selector prefix truth-set payload overflow"));
        self.truth_sets_by_hash.insert_identity(hash, truth);
        truth
    }

    fn intern_result(&mut self, matches: PrefixMatchSetID, truth: PrefixTruthSetID) -> PrefixResultID {
        let hash = super::intern_table::content_hash((matches, truth));
        if let Some(result) = self.result_ids.find(hash, |_result, candidate| {
            candidate.matches == matches && candidate.truth == truth
        }) {
            return result;
        }
        let result = PrefixResultID(u32::try_from(self.results.len()).expect("prefix result space exhausted"));
        let payload = PrefixResult { matches, truth };
        self.results.push(payload);
        self.result_ids.insert(hash, result, payload);
        result
    }

    fn compact_interned_states(&mut self) {
        let mut live_states = vec![false; self.states.len()];
        live_states[0] = true;
        for transition in self
            .transition_by_element
            .iter()
            .filter(|transition| transition.state != UNKNOWN_STATE)
        {
            for mut state in [transition.state, transition.right] {
                loop {
                    if live_states[state as usize] {
                        break;
                    }
                    live_states[state as usize] = true;
                    let base = self.states[state as usize].base;
                    if base == state {
                        break;
                    }
                    state = base;
                }
            }
        }

        let old_transitions = std::mem::take(&mut self.transitions);

        let mut state_remap = vec![UNKNOWN_STATE; self.states.len()];
        let mut states = Vec::with_capacity(live_states.iter().filter(|&&live| live).count());
        let mut delta_steps = Vec::new();
        for (old_index, live) in live_states.iter().copied().enumerate() {
            if !live {
                continue;
            }
            let old = &self.states[old_index];
            let new_index = u32::try_from(states.len()).expect("selector prefix state space exhausted");
            state_remap[old_index] = new_index;
            let payload_start = u32::try_from(delta_steps.len()).expect("selector prefix state payload overflow");
            delta_steps.extend(
                self.additions_in(old_index as u32)
                    .iter()
                    .map(|&step| std::mem::MaybeUninit::new(step)),
            );
            delta_steps.extend(
                self.expiring_in(old_index as u32)
                    .iter()
                    .map(|&step| std::mem::MaybeUninit::new(step)),
            );
            states.push(PrefixState {
                base: state_remap[old.base as usize],
                payload_start,
                additions_len: old.additions_len,
                expiring_len: old.expiring_len,
                descendant_len: old.descendant_len,
                descendant_hash: old.descendant_hash,
                expiring_hash: old.expiring_hash,
            });
        }

        for transition in self
            .transition_by_element
            .iter_mut()
            .filter(|transition| transition.state != UNKNOWN_STATE)
        {
            transition.state = state_remap[transition.state as usize];
            transition.right = state_remap[transition.right as usize];
        }
        let mut descendant_only = vec![UNKNOWN_STATE; states.len()];
        for (old_index, &new_index) in state_remap.iter().enumerate() {
            if new_index == UNKNOWN_STATE {
                continue;
            }
            let old_descendant = self.descendant_only[old_index];
            if old_descendant != UNKNOWN_STATE {
                descendant_only[new_index as usize] = state_remap[old_descendant as usize];
            }
        }

        self.states = states;
        self.delta_steps = delta_steps;
        self.descendant_only = descendant_only;
        self.states_by_hash = super::intern_table::InternTable::default();
        self.transitions = old_transitions
            .into_iter()
            .filter_map(|(key, transition)| {
                let parent = *state_remap.get(key.parent as usize)?;
                let previous = *state_remap.get(key.previous as usize)?;
                let state = *state_remap.get(transition.state as usize)?;
                let right = *state_remap.get(transition.right as usize)?;
                if [parent, previous, state, right].contains(&UNKNOWN_STATE) {
                    return None;
                }
                Some((
                    PrefixTransitionKey {
                        parent,
                        previous,
                        ..key
                    },
                    PrefixTransition {
                        state,
                        right,
                        result: transition.result,
                    },
                ))
            })
            .collect();
        for entering in self.entering_by_element.iter_mut() {
            if entering.parent == UNKNOWN_STATE {
                continue;
            }
            let Some(&parent) = state_remap.get(entering.parent as usize) else {
                *entering = UNKNOWN_ENTERING_STATES;
                continue;
            };
            let Some(&previous) = state_remap.get(entering.previous as usize) else {
                *entering = UNKNOWN_ENTERING_STATES;
                continue;
            };
            if parent == UNKNOWN_STATE || previous == UNKNOWN_STATE {
                *entering = UNKNOWN_ENTERING_STATES;
                continue;
            }
            entering.parent = parent;
            entering.previous = previous;
        }
    }

    fn interned_working_bytes(&self) -> u64 {
        (self.states.len() * size_of::<PrefixState>() + self.delta_steps.len() * size_of::<PrefixStepID>()) as u64
    }

    /// Shed unreachable interned identities and allocation slack before the working form is
    /// offered for residency. Only per-element transitions cross this boundary; traversal memos
    /// and completed-answer keys are batch-local, so every remapped identity stays internal.
    fn trim_for_retention(&mut self) {
        let working_bytes = self.interned_working_bytes();
        if self.last_compacted_working_bytes == 0
            || working_bytes > self.last_compacted_working_bytes.saturating_mul(3) / 2
        {
            self.compact_interned_states();
            self.last_compacted_working_bytes = self.interned_working_bytes();
        }
        // Retained state becomes scratch again before it can be mutated. Preserve the allocation
        // slack of its working vectors across that cycle: trimming them here makes the next flush
        // immediately allocate the same capacity again. Batch-local workspaces below still give
        // their complete allocations back because none of their contents cross this boundary.
        self.transition_by_row = Vec::new();
        self.candidate_epoch = EpochColumn::default();
        self.compound_epoch = EpochColumn::default();
        self.compound_answer = Vec::new();
        self.output_epoch = EpochColumn::default();
        self.match_epoch = EpochColumn::default();
        self.parent_persisting_epoch = EpochColumn::default();
        self.previous_persisting_epoch = EpochColumn::default();
        self.candidates = Vec::new();
        self.compare_left = Vec::new();
        self.compare_right = Vec::new();
        self.output_matches = Vec::new();
        self.output_matched_steps = Vec::new();
        self.new_descendant = Vec::new();
        self.new_child = Vec::new();
        self.new_following = Vec::new();
        self.new_adjacent = Vec::new();
        self.ancestor_chain = Vec::new();
    }

    fn top_level_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.states,
                self.delta_steps,
                self.states_by_hash,
                self.match_offsets,
                self.match_entries,
                self.truth_offsets,
                self.truth_steps,
                self.truth_sets_by_hash,
                self.results,
                self.result_ids,
                self.match_sets_by_hash,
                self.transitions,
                self.transition_by_row,
                self.transition_by_element,
                self.entering_by_element,
                self.local_facts_by_element,
                self.positional_bits_by_element,
                self.candidate_epoch,
                self.compound_epoch,
                self.output_epoch,
                self.match_epoch,
                self.parent_persisting_epoch,
                self.previous_persisting_epoch,
                self.compound_answer,
                self.candidates,
                self.compare_left,
                self.compare_right,
                self.new_descendant,
                self.new_child,
                self.new_following,
                self.new_adjacent,
                self.descendant_only,
                self.output_matches,
                self.output_matched_steps,
                self.ancestor_chain,
            ];
            cached [];
            nested [];
            skip [
                self.local_fact_interner,
                self.new_descendant_hash,
                self.new_child_hash,
                self.new_following_hash,
                self.new_adjacent_hash,
                self.facts_generation,
                self.epoch,
                self.complete,
                self.automaton_step_count,
                self.automaton_statistics_recorded,
                self.last_compacted_working_bytes,
            ];
        }
    }

    #[must_use]
    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [self.local_fact_interner.capacity_bytes()];
            nested [self.top_level_capacity_bytes()];
            skip [];
        }
    }
}

impl PrefixTransitionSurface<'_> {
    fn ancestor_chain_is_empty(&self) -> bool {
        match self {
            Self::Retained(states) => states.ancestor_chain.is_empty(),
        }
    }

    fn clear_ancestor_chain(&mut self) {
        match self {
            Self::Retained(states) => states.ancestor_chain.clear(),
        }
    }

    fn push_ancestor(&mut self, node: StyleNodeID, row: u32) {
        match self {
            Self::Retained(states) => states.ancestor_chain.push((node, row)),
        }
    }

    fn pop_ancestor(&mut self) -> Option<(StyleNodeID, u32)> {
        match self {
            Self::Retained(states) => states.ancestor_chain.pop(),
        }
    }

    fn known_transition(&self, facts: &StyleNodeFacts, node: StyleNodeID) -> Option<PrefixTransition> {
        let row = facts.row_of(node)? as usize;
        let known = match self {
            Self::Retained(states) => states.transition_by_row[row],
        };
        if known.state != UNKNOWN_STATE {
            return Some(known);
        }
        match self {
            Self::Retained(states) => match states.transition_of(node) {
                PrefixTransitionLookup::Known(known) => Some(known),
                PrefixTransitionLookup::Missing(_) => None,
            },
        }
    }

    fn local_fact_identity(
        &mut self,
        facts: &StyleNodeFacts,
        node: StyleNodeID,
        row: u32,
        counters: &mut Counters,
    ) -> u32 {
        match self {
            Self::Retained(states) => match states.local_facts_of(node) {
                Some(identity) => identity,
                None => {
                    let identity = states.local_fact_interner.intern(facts, row, counters);
                    states.set_local_facts(node, identity);
                    identity
                }
            },
        }
    }

    fn memoized_transition(&self, key: &PrefixTransitionKey) -> Option<PrefixTransition> {
        match self {
            Self::Retained(states) => states.transitions.get(key).copied(),
        }
    }

    fn insert_transition(&mut self, key: PrefixTransitionKey, transition: PrefixTransition) {
        match self {
            Self::Retained(states) => {
                states.transitions.insert(key, transition);
            }
        }
    }

    fn remember_transition(&mut self, node: StyleNodeID, row: u32, transition: PrefixTransition) {
        match self {
            Self::Retained(states) => {
                states.transition_by_row[row as usize] = transition;
                // Retained coverage is ancestor-closed: a missing parent therefore proves that no
                // descendant below it can hold a transition that this update would leave stale.
                states.set_transition(node, transition);
            }
        }
    }

    fn remember_positional_bits(&mut self, node: StyleNodeID, bits: u32) {
        match self {
            Self::Retained(states) => states.set_positional_bits(node, bits),
        }
    }

    fn remember_entering(&mut self, node: StyleNodeID, entering: EnteringStates) {
        match self {
            Self::Retained(states) => states.set_entering(node, entering),
        }
    }

    fn transition_from_inputs(
        &mut self,
        evaluation: &PrefixEvaluation<'_, '_>,
        node: StyleNodeID,
        row: u32,
        inputs: TransitionInputs,
        counters: &mut Counters,
    ) -> PrefixTransitionLookup<(PrefixTransition, PrefixTransitionOrigin)> {
        let key = PrefixTransitionKey {
            parent: inputs.entering.parent,
            previous: inputs.entering.previous,
            local_facts: inputs.local_facts,
            is_document_root: evaluation.tree.parent(node).is_none(),
            positional_bits: inputs.positional_bits,
        };
        if let Some(transition) = self.memoized_transition(&key) {
            counters.bump(Counter::PrefixTransitionMemoHits);
            if !evaluation.automaton.positional_tests().is_empty() {
                self.remember_positional_bits(node, inputs.positional_bits);
            }
            self.remember_entering(node, inputs.entering);
            return PrefixTransitionLookup::Known((transition, PrefixTransitionOrigin::Memoized));
        }
        counters.bump(Counter::PrefixTransitionMemoMisses);
        let transition = match self {
            Self::Retained(states) => states.transition(
                evaluation,
                inputs.entering,
                row,
                node,
                key.is_document_root,
                key.positional_bits,
                counters,
            ),
        };
        let transition = match transition {
            PrefixTransitionLookup::Known(transition) => transition,
            PrefixTransitionLookup::Missing(gap) => return PrefixTransitionLookup::Missing(gap),
        };
        self.insert_transition(key, transition);
        if !evaluation.automaton.positional_tests().is_empty() {
            self.remember_positional_bits(node, inputs.positional_bits);
        }
        self.remember_entering(node, inputs.entering);
        PrefixTransitionLookup::Known((transition, PrefixTransitionOrigin::Computed))
    }
}

/// Walk the dependencies of one transition in the retained cache. Sibling-free automata use the
/// same walk with the previous-sibling state pinned to zero.
fn transition_for(
    surface: &mut PrefixTransitionSurface<'_>,
    evaluation: &PrefixEvaluation<'_, '_>,
    node: StyleNodeID,
    counters: &mut Counters,
) -> PrefixTransitionLookup<PrefixTransition> {
    let has_sibling_steps = evaluation.automaton.has_sibling_steps();
    surface.clear_ancestor_chain();
    let mut current = node;
    loop {
        let Some(row) = evaluation.facts.row_of(current) else {
            return PrefixTransitionLookup::Missing(PrefixTransitionGap::Incomplete(Incomplete::MissingFacts(current)));
        };
        if let Some(known) = surface.known_transition(evaluation.facts, current) {
            if surface.ancestor_chain_is_empty() {
                return PrefixTransitionLookup::Known(known);
            }
            break;
        }
        surface.push_ancestor(current, row);
        if has_sibling_steps && let Some(previous) = evaluation.tree.previous_element_sibling(current) {
            current = previous;
            continue;
        }
        let Some(parent) = evaluation.tree.parent(current) else {
            break;
        };
        if Some(parent) == evaluation.shadow_root {
            break;
        }
        current = parent;
    }

    let mut result = UNKNOWN_TRANSITION;
    while let Some((node, row)) = surface.pop_ancestor() {
        let parent_state = match evaluation.tree.parent(node) {
            Some(parent) if Some(parent) != evaluation.shadow_root => {
                match surface.known_transition(evaluation.facts, parent) {
                    Some(transition) => transition.state,
                    None => return PrefixTransitionLookup::Missing(PrefixTransitionGap::MissingTransition(parent)),
                }
            }
            Some(_) | None => 0,
        };
        let previous_state = if has_sibling_steps {
            match evaluation.tree.previous_element_sibling(node) {
                Some(previous) => match surface.known_transition(evaluation.facts, previous) {
                    Some(transition) => transition.right,
                    None => {
                        return PrefixTransitionLookup::Missing(PrefixTransitionGap::MissingTransition(previous));
                    }
                },
                None => 0,
            }
        } else {
            0
        };
        let local_facts = surface.local_fact_identity(evaluation.facts, node, row, counters);
        let positional_bits = match evaluation.positional_bits(node, counters) {
            Ok(bits) => bits,
            Err(incomplete) => return PrefixTransitionLookup::Missing(PrefixTransitionGap::Incomplete(incomplete)),
        };
        result = match surface.transition_from_inputs(
            evaluation,
            node,
            row,
            TransitionInputs {
                entering: EnteringStates {
                    parent: parent_state,
                    previous: previous_state,
                },
                local_facts,
                positional_bits,
            },
            counters,
        ) {
            PrefixTransitionLookup::Known((transition, _)) => transition,
            PrefixTransitionLookup::Missing(gap) => return PrefixTransitionLookup::Missing(gap),
        };
        surface.remember_transition(node, row, result);
    }
    PrefixTransitionLookup::Known(result)
}

fn selected_matches_equal(left: &[DispatchEntryID], right: &[DispatchEntryID], selection: &PrefixSelection) -> bool {
    left.iter()
        .copied()
        .filter(|&entry| selection.contains_terminal(entry))
        .eq(right
            .iter()
            .copied()
            .filter(|&entry| selection.contains_terminal(entry)))
}

/// Prefix states shared by every element ask in one synchronous matching traversal.
///
/// The delta representation keeps the working form small enough to retain outright, so there is
/// no compact retained encoding: retention is a residency grant for the working bytes, and a
/// refused grant drops nothing but the grant itself.
pub(super) struct PrefixStateCache {
    by_program: Column<Option<Box<PrefixStates>>>,
    scratch_memory: MemoryLease,
    residency: MemoryLease,
    lifecycle: PrefixStateCacheLifecycle,
}

#[derive(Clone, Copy)]
enum PrefixStateCacheCoverage {
    Full,
    Sparse,
}

#[derive(Clone, Copy)]
enum PrefixStateCacheLifecycle {
    Scratch(PrefixStateCacheCoverage),
    Retained(PrefixStateCacheCoverage),
    CurrentScratch(PrefixStateCacheCoverage),
    CurrentRetained(PrefixStateCacheCoverage),
}

impl PrefixStateCacheLifecycle {
    fn coverage(self) -> PrefixStateCacheCoverage {
        match self {
            Self::Scratch(coverage)
            | Self::Retained(coverage)
            | Self::CurrentScratch(coverage)
            | Self::CurrentRetained(coverage) => coverage,
        }
    }

    fn is_retained(self) -> bool {
        matches!(self, Self::Retained(_) | Self::CurrentRetained(_))
    }

    fn is_current(self) -> bool {
        matches!(self, Self::CurrentScratch(_) | Self::CurrentRetained(_))
    }
}

pub(super) type PrefixStatesGuard<'a> = CapacityGuard<'a, PrefixStates>;

impl Default for PrefixStateCache {
    fn default() -> Self {
        Self {
            by_program: Column::default(),
            scratch_memory: MemoryLease::new(MemoryCategory::BatchScratch),
            residency: MemoryLease::new(MemoryCategory::PrefixTransitionCache),
            lifecycle: PrefixStateCacheLifecycle::Scratch(PrefixStateCacheCoverage::Full),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum PrefixStateCacheGap {
    MissingProgram(ScopeProgramID),
}

impl PrefixStateCache {
    pub(super) fn lookup(&self, program: ScopeProgramID) -> Lookup<&PrefixStates, PrefixStateCacheGap> {
        match self.by_program.get(program.0 as usize).and_then(Option::as_deref) {
            Some(states) => Lookup::Known(states),
            None => Lookup::Missing(PrefixStateCacheGap::MissingProgram(program)),
        }
    }

    pub(super) fn get_or_insert(
        &mut self,
        program: ScopeProgramID,
        generation: u64,
        row_count: usize,
        memory: &mut MemoryController,
    ) -> PrefixStatesGuard<'_> {
        // A retained cache's committed bytes live in the residency lease, so every accounting
        // move here has to follow the same lease choice as `lookup_mut`. Charging scratch while
        // retained underflows the scratch lease as soon as `prepare_rows` shrinks an entry. The
        // residency lease is acceleration-tier: capacity that already exists is recorded as
        // committed, and the over-limit answer comes from the caller's settle.
        let retained = self.lifecycle.is_retained();
        let lease = if retained {
            &mut self.residency
        } else {
            &mut self.scratch_memory
        };
        let charge = |lease: &mut MemoryLease, memory: &mut MemoryController, bytes: u64| {
            if retained {
                lease.grow_committed(bytes);
            } else {
                lease.grow_required(memory, bytes);
            }
        };
        let index = program.0 as usize;
        charge(lease, memory, self.by_program.ensure(index));
        if self.by_program[index].is_none() {
            let states = PrefixStates::new(row_count);
            let bytes = size_of::<PrefixStates>() as u64 + states.capacity_bytes();
            self.by_program[index] = Some(Box::new(states));
            charge(lease, memory, bytes);
        } else {
            let states = self.by_program[index].as_mut().expect("program entry just checked");
            let before = states.capacity_bytes();
            states.prepare_rows(generation, row_count);
            let after = states.capacity_bytes();
            if after > before {
                charge(lease, memory, after - before);
            } else {
                lease.shrink_committed(before - after);
            }
        }
        let states = self.by_program[index].as_mut().expect("program entry just inserted");
        CapacityGuard::new(states, lease, PrefixStates::capacity_bytes)
    }

    pub(super) fn lookup_mut(&mut self, program: ScopeProgramID) -> Lookup<PrefixStatesGuard<'_>, PrefixStateCacheGap> {
        let memory = if self.lifecycle.is_retained() {
            &mut self.residency
        } else {
            &mut self.scratch_memory
        };
        match self.by_program.get_mut(program.0 as usize).and_then(Option::as_mut) {
            Some(states) => Lookup::Known(CapacityGuard::new(states, memory, PrefixStates::capacity_bytes)),
            None => Lookup::Missing(PrefixStateCacheGap::MissingProgram(program)),
        }
    }

    pub(super) fn settle_memory(&mut self, memory: &mut MemoryController) -> bool {
        if self.lifecycle.is_retained() {
            self.residency.settle_committed(memory)
        } else {
            self.scratch_memory.settle_committed(memory)
        }
    }

    pub(super) fn release(&mut self) {
        if self.lifecycle.is_retained() {
            self.residency.release();
        } else {
            self.scratch_memory.release();
        }
        self.by_program = Column::default();
        self.lifecycle = PrefixStateCacheLifecycle::Scratch(self.lifecycle.coverage());
    }

    pub(super) fn retain(&mut self, memory: &mut MemoryController, _counters: &mut Counters) -> bool {
        if self.lifecycle.is_retained() {
            return true;
        }
        for states in self.by_program.iter_mut().flatten() {
            let before = states.capacity_bytes();
            states.trim_for_retention();
            let after = states.capacity_bytes();
            self.scratch_memory.shrink_committed(before - after);
        }
        let working_bytes = self.scratch_memory.bytes();
        if !self.residency.grow(memory, working_bytes).is_granted() {
            return false;
        }
        self.scratch_memory.release();
        self.lifecycle = match self.lifecycle {
            PrefixStateCacheLifecycle::Scratch(coverage) => PrefixStateCacheLifecycle::Retained(coverage),
            PrefixStateCacheLifecycle::CurrentScratch(coverage) => PrefixStateCacheLifecycle::CurrentRetained(coverage),
            PrefixStateCacheLifecycle::Retained(_) | PrefixStateCacheLifecycle::CurrentRetained(_) => unreachable!(),
        };
        true
    }

    /// Forget one element's transition. This is the whole maintenance a pure tree flush needs,
    /// so it stays valid on a retained cache.
    pub(super) fn forget_transition(&mut self, program: ScopeProgramID, node: StyleNodeID) {
        if let Some(states) = self.by_program.get_mut(program.0 as usize).and_then(Option::as_mut) {
            states.forget_transition(node);
        }
    }

    /// Whether the resident states were last given back by a publication completion batch,
    /// which warms spines but covers only the nodes that were asked: a walk over such a cache
    /// must keep every pending route, since subsumption speaks for a route's whole candidate
    /// space. A walk that refreshes the cache clears this.
    #[must_use]
    pub(super) fn is_sparse(&self) -> bool {
        matches!(self.lifecycle.coverage(), PrefixStateCacheCoverage::Sparse)
    }

    fn set_coverage(&mut self, coverage: PrefixStateCacheCoverage) {
        self.lifecycle = match self.lifecycle {
            PrefixStateCacheLifecycle::Scratch(_) => PrefixStateCacheLifecycle::Scratch(coverage),
            PrefixStateCacheLifecycle::Retained(_) => PrefixStateCacheLifecycle::Retained(coverage),
            PrefixStateCacheLifecycle::CurrentScratch(_) => PrefixStateCacheLifecycle::CurrentScratch(coverage),
            PrefixStateCacheLifecycle::CurrentRetained(_) => PrefixStateCacheLifecycle::CurrentRetained(coverage),
        };
    }

    pub(super) fn mark_sparse(&mut self) {
        self.set_coverage(PrefixStateCacheCoverage::Sparse);
    }

    pub(super) fn mark_full(&mut self) {
        self.set_coverage(PrefixStateCacheCoverage::Full);
    }

    #[must_use]
    pub(super) fn is_current(&self) -> bool {
        self.lifecycle.is_current()
    }

    #[must_use]
    pub(super) fn is_retained(&self) -> bool {
        self.lifecycle.is_retained()
    }

    pub(super) fn mark_current(&mut self) {
        self.lifecycle = match self.lifecycle {
            PrefixStateCacheLifecycle::Scratch(coverage) => PrefixStateCacheLifecycle::CurrentScratch(coverage),
            PrefixStateCacheLifecycle::Retained(coverage) => PrefixStateCacheLifecycle::CurrentRetained(coverage),
            current
            @ (PrefixStateCacheLifecycle::CurrentScratch(_) | PrefixStateCacheLifecycle::CurrentRetained(_)) => current,
        };
    }

    pub(super) fn mark_previous(&mut self) {
        self.lifecycle = match self.lifecycle {
            PrefixStateCacheLifecycle::CurrentScratch(coverage) => PrefixStateCacheLifecycle::Scratch(coverage),
            PrefixStateCacheLifecycle::CurrentRetained(coverage) => PrefixStateCacheLifecycle::Retained(coverage),
            previous @ (PrefixStateCacheLifecycle::Scratch(_) | PrefixStateCacheLifecycle::Retained(_)) => previous,
        };
    }

    pub(super) fn make_scratch(&mut self, memory: &mut MemoryController) {
        if !self.lifecycle.is_retained() {
            return;
        }
        self.scratch_memory.resize_required_to(memory, self.residency.bytes());
        self.residency.release();
        self.lifecycle = match self.lifecycle {
            PrefixStateCacheLifecycle::Retained(coverage) => PrefixStateCacheLifecycle::Scratch(coverage),
            PrefixStateCacheLifecycle::CurrentRetained(coverage) => PrefixStateCacheLifecycle::CurrentScratch(coverage),
            PrefixStateCacheLifecycle::Scratch(_) | PrefixStateCacheLifecycle::CurrentScratch(_) => unreachable!(),
        };
    }

    pub(super) fn prepare_to_mutate(&mut self, memory: &mut MemoryController) {
        debug_assert!(!self.lifecycle.is_retained());
        for states in self.by_program.iter_mut().flatten() {
            let before = states.capacity_bytes();
            states.rebuild_interners();
            let after = states.capacity_bytes();
            if after > before {
                self.scratch_memory.grow_required(memory, after - before);
            }
        }
    }

    pub(super) fn remove(&mut self, program: ScopeProgramID) {
        let index = program.0 as usize;
        if self.by_program.get(index).is_none_or(Option::is_none) {
            return;
        }
        let released = size_of::<PrefixStates>() as u64
            + self.by_program[index]
                .as_ref()
                .expect("program entry is live")
                .capacity_bytes();
        self.by_program[index] = None;
        let held = if self.lifecycle.is_retained() {
            self.residency.bytes()
        } else {
            self.scratch_memory.bytes()
        };
        let released = released.min(held);
        if self.lifecycle.is_retained() {
            self.residency.shrink_to(held - released);
        } else {
            self.scratch_memory.shrink_to(held - released);
        }
    }
}

fn hash_local_facts(facts: &StyleNodeFacts, row: u32) -> u64 {
    let mut hasher = fast_hasher();
    facts.tag_of(row).hash(&mut hasher);
    facts.folded_tag_of(row).hash(&mut hasher);
    facts.id_of(row).hash(&mut hasher);
    facts.states_of(row).hash(&mut hasher);
    facts.directionality_of(row).hash(&mut hasher);
    facts.language_of(row).hash(&mut hasher);
    facts.language_tag_of(row).hash(&mut hasher);
    facts.namespace_of(row).hash(&mut hasher);
    facts.heading_level_of(row).hash(&mut hasher);
    facts.custom_states_of(row).hash(&mut hasher);
    facts.parts_of(row).hash(&mut hasher);
    facts.classes_of(row).hash(&mut hasher);
    for &attribute in facts.attributes_of(row) {
        attribute.name.hash(&mut hasher);
        attribute.local.hash(&mut hasher);
        attribute.folded_name.hash(&mut hasher);
        attribute.folded_local.hash(&mut hasher);
        attribute.value.hash(&mut hasher);
        facts.text_of(attribute).hash(&mut hasher);
    }
    hasher.finish()
}

fn rows_have_equal_local_facts(facts: &StyleNodeFacts, left: u32, right: u32) -> bool {
    if facts.tag_of(left) != facts.tag_of(right)
        || facts.folded_tag_of(left) != facts.folded_tag_of(right)
        || facts.id_of(left) != facts.id_of(right)
        || facts.states_of(left) != facts.states_of(right)
        || facts.directionality_of(left) != facts.directionality_of(right)
        || facts.language_of(left) != facts.language_of(right)
        || facts.language_tag_of(left) != facts.language_tag_of(right)
        || facts.namespace_of(left) != facts.namespace_of(right)
        || facts.heading_level_of(left) != facts.heading_level_of(right)
        || facts.custom_states_of(left) != facts.custom_states_of(right)
        || facts.parts_of(left) != facts.parts_of(right)
        || facts.classes_of(left) != facts.classes_of(right)
    {
        return false;
    }
    let left_attributes = facts.attributes_of(left);
    let right_attributes = facts.attributes_of(right);
    left_attributes.len() == right_attributes.len()
        && left_attributes.iter().zip(right_attributes).all(|(&left, &right)| {
            left.name == right.name
                && left.local == right.local
                && left.folded_name == right.folded_name
                && left.folded_local == right.folded_local
                && left.value == right.value
                && facts.text_of(left) == facts.text_of(right)
        })
}

fn matches_feature(facts: &StyleNodeFacts, row: u32, feature: FeatureTest) -> bool {
    match feature {
        FeatureTest::AnyElement => true,
        FeatureTest::Namespace(NamespaceTest::None) => facts.namespace_of(row).is_none(),
        FeatureTest::Namespace(NamespaceTest::Named(namespace)) => facts.namespace_of(row) == namespace,
        FeatureTest::TagName(tag) => tag.matches(facts.tag_of(row), facts.namespace_of(row)),
        FeatureTest::Id(id) => facts.id_of(row) == id,
        FeatureTest::Class(class) => facts.classes_of(row).contains(&class),
        FeatureTest::Attribute(test) => {
            let folds = !test.fold_in_namespace.is_none() && facts.namespace_of(row) == test.fold_in_namespace;
            facts.attributes_of(row).iter().any(|attribute| {
                let (written, folded) = match test.any_namespace {
                    true => (attribute.local, attribute.folded_local),
                    false => (attribute.name, attribute.folded_name),
                };
                (written == test.name || (folds && folded == test.folded))
                    && match test.operator {
                        AttributeOperator::Presence => true,
                        AttributeOperator::Exact => attribute.value == test.value_atom,
                        _ => unreachable!("only atom-answerable features are canonicalized"),
                    }
            })
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::index::StyleAtomID;
    use super::*;

    #[test]
    fn begin_transition_sizes_every_scratch_column_independently() {
        let mut automaton = PrefixAutomaton::default();
        for _ in 0..8_u32 {
            automaton.steps.push(PrefixStep {
                compound: PrefixCompoundID(0),
                output_start: 0,
                output_len: 0,
            });
        }
        let mut states = PrefixStates::new(0);
        // Retention trims every column to empty; comparison and sparse-delta paths regrow
        // candidate_epoch alone. begin_transition must size the siblings regardless.
        states.candidate_epoch.resize(automaton.steps.len(), 0);
        states.begin_transition(&automaton);
        assert_eq!(states.output_epoch.len(), automaton.steps.len());
        assert_eq!(states.parent_persisting_epoch.len(), automaton.steps.len());
        assert_eq!(states.previous_persisting_epoch.len(), automaton.steps.len());
    }

    #[test]
    fn prefix_state_digests_use_existing_alignment_space() {
        assert_eq!(size_of::<PrefixState>(), 32);
    }

    #[test]
    fn a_forgotten_prefix_transition_is_a_typed_missing_node() {
        let mut states = PrefixStates::new(0);
        let node = StyleNodeID::element(1);
        assert!(matches!(
            states.transition_of(node),
            PrefixTransitionLookup::Missing(PrefixTransitionGap::MissingTransition(gap)) if gap == node
        ));

        states.set_transition(
            node,
            PrefixTransition {
                state: 0,
                right: 0,
                result: PrefixResultID::default(),
            },
        );
        states.set_entering(node, EnteringStates { parent: 0, previous: 0 });
        states.set_positional_bits(node, 0b11);
        assert!(matches!(states.transition_of(node), PrefixTransitionLookup::Known(_)));
        states.forget_transition(node);
        assert_eq!(
            states.entering_by_element[node.element_index().unwrap() as usize].parent,
            UNKNOWN_STATE
        );
        assert_eq!(
            states.positional_bits_by_element[node.element_index().unwrap() as usize],
            0
        );
        assert!(matches!(
            states.transition_of(node),
            PrefixTransitionLookup::Missing(PrefixTransitionGap::MissingTransition(gap)) if gap == node
        ));
    }

    #[test]
    fn sparse_rebase_shares_unchanged_local_payload() {
        let mut states = PrefixStates::new(0);
        let mut counters = Counters::new();
        let old_base_step = PrefixStepID(1);
        let local_step = PrefixStepID(2);
        let expiring_step = PrefixStepID(3);
        let new_base_step = PrefixStepID(4);
        let old_base =
            states.intern_extended_state(0, &[old_base_step], step_hash(old_base_step), &[], 0, &mut counters);
        let source = states.intern_extended_state(
            old_base,
            &[local_step],
            step_hash(local_step),
            &[expiring_step],
            step_hash(expiring_step) as u32,
            &mut counters,
        );
        let new_base =
            states.intern_extended_state(0, &[new_base_step], step_hash(new_base_step), &[], 0, &mut counters);
        let source_payload = states.states[source as usize].payload_start;

        let rebased = states
            .rebase_unchanged_local_payload(source, old_base, new_base, &mut counters)
            .expect("a one-level local payload can be rebased");

        assert_eq!(states.states[rebased as usize].base, new_base);
        assert_eq!(states.states[rebased as usize].payload_start, source_payload);
        assert_eq!(states.additions_in(rebased), [local_step]);
        assert_eq!(states.expiring_in(rebased), [expiring_step]);
    }

    #[test]
    fn prefix_state_equality_compares_structural_sets_without_sorting() {
        let first = PrefixStepID(1);
        let second = PrefixStepID(2);
        let third = PrefixStepID(3);
        let mut states = PrefixStates::new(0);
        states.automaton_step_count = 4;
        states.candidate_epoch.resize(4, 0);
        states.delta_steps = [first, second, first, second, third]
            .into_iter()
            .map(std::mem::MaybeUninit::new)
            .collect();
        states.states.extend([
            PrefixState {
                base: 0,
                payload_start: 0,
                additions_len: 2,
                expiring_len: 0,
                descendant_len: 2,
                descendant_hash: step_hash(first).wrapping_add(step_hash(second)),
                expiring_hash: 0,
            },
            PrefixState {
                base: 0,
                payload_start: 2,
                additions_len: 1,
                expiring_len: 0,
                descendant_len: 1,
                descendant_hash: step_hash(first),
                expiring_hash: 0,
            },
            PrefixState {
                base: 2,
                payload_start: 3,
                additions_len: 1,
                expiring_len: 0,
                descendant_len: 2,
                descendant_hash: step_hash(first).wrapping_add(step_hash(second)),
                expiring_hash: 0,
            },
            PrefixState {
                base: 2,
                payload_start: 4,
                additions_len: 1,
                expiring_len: 0,
                descendant_len: 2,
                descendant_hash: step_hash(first).wrapping_add(step_hash(third)),
                expiring_hash: 0,
            },
        ]);

        assert!(states.selected_states_equal(1, 3, None));
        assert!(!states.selected_states_equal(3, 4, None));
        let selection = PrefixSelection {
            steps: vec![false, true, false, false].into_boxed_slice(),
            terminals: Box::new([]),
        };
        assert!(states.selected_states_equal(3, 4, Some(&selection)));
    }

    #[test]
    fn prefix_state_deltas_compare_the_selected_active_set() {
        let cancelled = PrefixStepID(1);
        let changed = PrefixStepID(2);
        let mut arena = PrefixDeltaArena::default();
        arena.scratch[0].extend([cancelled, changed]);
        arena.scratch[3].push(cancelled);
        let delta = arena.append_scratch_delta(0);
        let cancelled_selection = PrefixSelection {
            steps: vec![false, true, false].into_boxed_slice(),
            terminals: Box::new([]),
        };
        let changed_selection = PrefixSelection {
            steps: vec![false, true, true].into_boxed_slice(),
            terminals: Box::new([]),
        };

        assert!(!arena.changes_selected_state(delta, &cancelled_selection));
        assert!(arena.changes_selected_state(delta, &changed_selection));
    }

    #[test]
    fn sparse_rebase_applies_signed_local_output_edits() {
        let mut states = PrefixStates::new(0);
        let mut counters = Counters::new();
        let old_base_step = PrefixStepID(0);
        let retained_step = PrefixStepID(1);
        let removed_step = PrefixStepID(2);
        let new_base_step = PrefixStepID(3);
        let removed_expiring_step = PrefixStepID(4);
        let added_step = PrefixStepID(5);
        let added_expiring_step = PrefixStepID(6);
        let old_base =
            states.intern_extended_state(0, &[old_base_step], step_hash(old_base_step), &[], 0, &mut counters);
        let source = states.intern_extended_state(
            old_base,
            &[retained_step, removed_step],
            step_hash(retained_step).wrapping_add(step_hash(removed_step)),
            &[removed_expiring_step],
            step_hash(removed_expiring_step) as u32,
            &mut counters,
        );
        let new_base =
            states.intern_extended_state(0, &[new_base_step], step_hash(new_base_step), &[], 0, &mut counters);
        let mut arena = PrefixDeltaArena::default();
        arena.scratch[0].extend([new_base_step, added_step]);
        arena.scratch[1].push(removed_step);
        arena.scratch[2].push(added_expiring_step);
        arena.scratch[3].push(removed_expiring_step);
        let delta = arena.append_scratch_delta(0);

        let rebased = states
            .rebase_payload_with_local_delta(source, old_base, new_base, delta, &arena, true, &mut counters)
            .expect("a one-level local payload can apply signed edits");

        assert_eq!(states.states[rebased as usize].base, new_base);
        assert_eq!(states.additions_in(rebased), [retained_step, added_step]);
        assert_eq!(states.expiring_in(rebased), [added_expiring_step]);
    }

    #[test]
    fn finished_automaton_classifies_terminal_producers() {
        let mut automaton = PrefixAutomaton::default();
        automaton.compounds.push(PrefixCompound {
            predicate: PrefixPredicate::Features {
                feature_start: 0,
                feature_len: 0,
                required_positional_bits: 0,
            },
            dispatch_key: DispatchKey::Universal,
        });
        automaton
            .buckets
            .insert(DispatchKey::Universal, PrefixDispatchBucket::default());
        for _ in 0..2 {
            automaton.steps.push(PrefixStep {
                compound: PrefixCompoundID(0),
                output_start: 0,
                output_len: 0,
            });
            automaton.step_predecessors.push(u32::MAX);
            automaton.step_output_builders.push(PrefixStepOutputBuilder::default());
        }
        let unique = DispatchEntryID::from_index(0);
        let shared = DispatchEntryID::from_index(1);
        automaton.step_output_builders[0]
            .terminals
            .extend([unique, unique, shared]);
        automaton.step_output_builders[1].terminals.push(shared);

        automaton.finish();

        let first_outputs = automaton.outputs_for(&automaton.steps[0]);
        assert!(matches!(first_outputs[0].kind, PrefixOutputKind::UniqueTerminal));
        assert!(matches!(first_outputs[1].kind, PrefixOutputKind::UniqueTerminal));
        assert!(matches!(first_outputs[2].kind, PrefixOutputKind::SharedTerminal));
        let second_outputs = automaton.outputs_for(&automaton.steps[1]);
        assert!(matches!(second_outputs[0].kind, PrefixOutputKind::SharedTerminal));
    }

    #[test]
    fn finished_automaton_remaps_step_references_into_dispatch_order() {
        let mut automaton = PrefixAutomaton::default();
        automaton.compounds.extend([
            PrefixCompound {
                predicate: PrefixPredicate::Features {
                    feature_start: 0,
                    feature_len: 0,
                    required_positional_bits: 0,
                },
                dispatch_key: DispatchKey::Universal,
            },
            PrefixCompound {
                predicate: PrefixPredicate::Features {
                    feature_start: 0,
                    feature_len: 0,
                    required_positional_bits: 0,
                },
                dispatch_key: DispatchKey::Id(StyleAtomID(1)),
            },
        ]);
        automaton.buckets.insert(
            DispatchKey::Universal,
            PrefixDispatchBucket {
                root_steps: vec![PrefixStepID(0)],
                ..PrefixDispatchBucket::default()
            },
        );
        automaton
            .buckets
            .insert(DispatchKey::Id(StyleAtomID(1)), PrefixDispatchBucket::default());
        automaton.steps.extend([
            PrefixStep {
                compound: PrefixCompoundID(0),
                output_start: 0,
                output_len: 0,
            },
            PrefixStep {
                compound: PrefixCompoundID(1),
                output_start: 0,
                output_len: 0,
            },
        ]);
        automaton.step_predecessors.extend([u32::MAX, 0]);
        automaton
            .step_output_builders
            .resize(2, PrefixStepOutputBuilder::default());
        automaton.step_output_builders[0].child_successors.push(PrefixStepID(1));
        automaton.entry_paths.push(PrefixEntryPaths {
            key: (SelectorProgramID(0), 0),
            paths: vec![PrefixEntryPath {
                terminal: DispatchEntryID::from_index(0),
                steps: vec![PrefixStepID(0), PrefixStepID(1)].into_boxed_slice(),
            }],
        });

        automaton.finish();

        assert_eq!(
            automaton.compounds[automaton.steps[0].compound.0 as usize].dispatch_key,
            DispatchKey::Id(StyleAtomID(1))
        );
        assert_eq!(automaton.predecessor_of(PrefixStepID(0)), Some(PrefixStepID(1)));
        let outputs = automaton.outputs_for(&automaton.steps[1]);
        assert!(matches!(outputs[0].kind, PrefixOutputKind::Child));
        assert_eq!(outputs[0].target, 0);
        assert_eq!(
            automaton.bucket(DispatchKey::Universal).unwrap().root_steps,
            [PrefixStepID(1)]
        );
        assert_eq!(
            automaton.entry_paths[0].paths[0].steps.as_ref(),
            [PrefixStepID(1), PrefixStepID(0)]
        );
    }

    #[test]
    fn sparse_match_delta_applies_additions_and_removals() {
        let mut states = PrefixStates::new(0);
        let mut counters = Counters::new();
        let removed = DispatchEntryID::from_index(0);
        let retained = DispatchEntryID::from_index(1);
        let added = DispatchEntryID::from_index(2);
        states.output_matches.extend([removed, retained]);
        let matches = states.intern_output_matches(&mut counters);
        let old_result = states.intern_result(matches, PrefixTruthSetID::default());
        let mut arena = PrefixDeltaArena::default();
        arena.match_scratch[0].push(added);
        arena.match_scratch[1].push(removed);
        let delta = arena.append_match_delta();

        let result = states.apply_local_match_delta(old_result, delta, &arena, &mut counters);

        assert_eq!(
            states.matches_in(states.results[result.0 as usize].matches),
            [retained, added]
        );
        assert_eq!(states.results[result.0 as usize].truth, PrefixTruthSetID::default());
    }

    #[test]
    fn retention_compacts_prefix_states_from_element_roots() {
        let mut states = PrefixStates::new(0);
        states.delta_steps = vec![
            std::mem::MaybeUninit::new(PrefixStepID(1)),
            std::mem::MaybeUninit::new(PrefixStepID(2)),
            std::mem::MaybeUninit::new(PrefixStepID(3)),
        ];
        states.states.extend([
            PrefixState {
                base: 0,
                payload_start: 0,
                additions_len: 1,
                expiring_len: 0,
                descendant_len: 1,
                descendant_hash: step_hash(PrefixStepID(1)),
                expiring_hash: 0,
            },
            PrefixState {
                base: 0,
                payload_start: 1,
                additions_len: 1,
                expiring_len: 0,
                descendant_len: 1,
                descendant_hash: step_hash(PrefixStepID(2)),
                expiring_hash: 0,
            },
            PrefixState {
                base: 2,
                payload_start: 2,
                additions_len: 1,
                expiring_len: 0,
                descendant_len: 2,
                descendant_hash: step_hash(PrefixStepID(2)).wrapping_add(step_hash(PrefixStepID(3))),
                expiring_hash: 0,
            },
        ]);
        states.descendant_only = vec![0, 1, 2, 3];
        let node = StyleNodeID::element(1);
        states.set_transition(
            node,
            PrefixTransition {
                state: 3,
                right: 0,
                result: PrefixResultID::default(),
            },
        );
        states.set_entering(node, EnteringStates { parent: 3, previous: 0 });
        let PrefixTransitionLookup::Known(transition) = states.transition_of(node) else {
            panic!("the retained transition must be known");
        };
        states.transitions.insert(
            PrefixTransitionKey {
                parent: 3,
                previous: 0,
                local_facts: 1,
                is_document_root: false,
                positional_bits: 0,
            },
            transition,
        );

        states.compact_interned_states();

        assert_eq!(states.states.len(), 3);
        assert_eq!(states.additions_in(1), [PrefixStepID(2)]);
        assert_eq!(states.additions_in(2), [PrefixStepID(3)]);
        let PrefixTransitionLookup::Known(transition) = states.transition_of(node) else {
            panic!("the compacted transition must be known");
        };
        assert_eq!(transition.state, 2);
        assert_eq!(
            states.entering_by_element[node.element_index().unwrap() as usize].parent,
            2
        );
        assert_eq!(states.states[transition.state as usize].base, 1);
        assert_eq!(states.transitions.len(), 1);
        let (key, memoized) = states.transitions.iter().next().unwrap();
        assert_eq!(key.parent, 2);
        assert_eq!(memoized.state, 2);
    }

    #[test]
    fn retention_preserves_working_vector_capacity() {
        let mut states = PrefixStates::new(0);
        states.last_compacted_working_bytes = states.interned_working_bytes();
        states.match_offsets.reserve(32);
        states.match_entries.reserve(32);
        states.truth_offsets.reserve(32);
        states.truth_steps.reserve(32);
        states.results.reserve(32);
        states.transition_by_element.reserve(32);
        let capacities = [
            states.match_offsets.capacity(),
            states.match_entries.capacity(),
            states.truth_offsets.capacity(),
            states.truth_steps.capacity(),
            states.results.capacity(),
            states.transition_by_element.capacity(),
        ];

        states.trim_for_retention();

        assert_eq!(
            capacities,
            [
                states.match_offsets.capacity(),
                states.match_entries.capacity(),
                states.truth_offsets.capacity(),
                states.truth_steps.capacity(),
                states.results.capacity(),
                states.transition_by_element.capacity(),
            ]
        );
    }

    #[test]
    fn retained_cache_entry_maintenance_charges_the_residency_lease() {
        use super::super::index::StateSet;
        use super::super::memory::DeviceClass;

        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut counters = Counters::new();
        let mut cache = PrefixStateCache::default();
        let mut facts = StyleNodeFacts::new();
        facts.push_row(
            super::super::tree::StyleNodeID::element(1),
            StyleAtomID(1),
            StyleAtomID::NONE,
            StateSet(0),
            &[],
            &[],
        );
        {
            let mut states = cache.get_or_insert(ScopeProgramID(0), facts.generation(), 1, &mut memory);
            states.local_fact_interner.intern(&facts, 0, &mut counters);
        }
        cache.settle_memory(&mut memory);
        assert!(cache.retain(&mut memory, &mut counters));
        assert!(memory.bytes_in_category(MemoryCategory::PrefixTransitionCache) > 0);

        // A generation change clears the entry's local-fact interner, shrinking its capacity. On a
        // retained cache that shrink and the guard's follow-up accounting must move the residency
        // lease; the scratch lease holds zero bytes here and would underflow.
        drop(cache.get_or_insert(ScopeProgramID(0), facts.generation() + 1, 1, &mut memory));
        cache.settle_memory(&mut memory);
        assert_eq!(memory.bytes_in_category(MemoryCategory::BatchScratch), 0);
    }
}
