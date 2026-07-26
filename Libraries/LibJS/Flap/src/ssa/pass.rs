/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SSA optimization pass orchestration and analysis caching.

use super::Function;
use super::analysis::{
    ControlFlowGraph, DominatorTree, GuardExits, InstructionLayout, NaturalLoop, find_natural_loops,
};
use super::effects::EffectDependencies;
use super::print::function_to_string;
use super::report::{OptimizationRemark, OptimizationRemarkKind, PassRunReport};
use std::time::Instant;

#[derive(Debug, Clone, Copy)]
pub(crate) struct FunctionAnalyses<'a> {
    pub(crate) cfg: &'a ControlFlowGraph,
    pub(crate) dominators: &'a DominatorTree,
    pub(crate) instruction_layout: &'a InstructionLayout,
    pub(crate) effects: &'a EffectDependencies,
    pub(crate) guards: &'a GuardExits,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct PlacementAnalyses<'a> {
    pub(crate) dominators: &'a DominatorTree,
    pub(crate) instruction_layout: &'a InstructionLayout,
    pub(crate) loops: &'a [NaturalLoop],
    pub(crate) guards: &'a GuardExits,
}

#[derive(Debug, Default)]
struct PassInstrumentation {
    attempted_transformations: u64,
    successful_transformations: u64,
    remarks: Vec<OptimizationRemark>,
    collect_remarks: bool,
}

#[derive(Debug, Default)]
pub(crate) struct AnalysisManager {
    cfg: Option<ControlFlowGraph>,
    dominators: Option<DominatorTree>,
    instruction_layout: Option<InstructionLayout>,
    loops: Option<Vec<NaturalLoop>>,
    effects: Option<EffectDependencies>,
    guards: Option<GuardExits>,
    instrumentation: Option<PassInstrumentation>,
}

impl AnalysisManager {
    pub(crate) fn cfg<'a>(&'a mut self, function: &Function) -> &'a ControlFlowGraph {
        self.cfg.get_or_insert_with(|| ControlFlowGraph::compute(function))
    }

    pub(crate) fn dominators<'a>(&'a mut self, function: &Function) -> &'a DominatorTree {
        if self.dominators.is_none() {
            self.cfg(function);
            let dominators = DominatorTree::compute(function, self.cfg.as_ref().unwrap());
            self.dominators = Some(dominators);
        }
        self.dominators.as_ref().unwrap()
    }

    pub(crate) fn instruction_layout<'a>(&'a mut self, function: &Function) -> &'a InstructionLayout {
        self.instruction_layout.get_or_insert_with(|| {
            InstructionLayout::compute(function).expect("valid SSA has a valid instruction layout")
        })
    }

    pub(crate) fn loops<'a>(&'a mut self, function: &Function) -> &'a [NaturalLoop] {
        if self.loops.is_none() {
            self.cfg(function);
            self.dominators(function);
            let loops = find_natural_loops(function, self.cfg.as_ref().unwrap(), self.dominators.as_ref().unwrap());
            self.loops = Some(loops);
        }
        self.loops.as_deref().unwrap()
    }

    pub(crate) fn effects<'a>(&'a mut self, function: &Function) -> &'a EffectDependencies {
        if self.effects.is_none() {
            self.cfg(function);
            let effects = EffectDependencies::compute(function, self.cfg.as_ref().unwrap());
            self.effects = Some(effects);
        }
        self.effects.as_ref().unwrap()
    }

    pub(crate) fn guards<'a>(&'a mut self, function: &Function) -> &'a GuardExits {
        if self.guards.is_none() {
            self.cfg(function);
            let guards = GuardExits::compute(function, self.cfg.as_ref().unwrap());
            self.guards = Some(guards);
        }
        self.guards.as_ref().unwrap()
    }

    /// The analyses a pass needs to decide where an instruction belongs.
    ///
    /// Placement says nothing about effects, and computing the effect
    /// dependencies of a whole large function is the most expensive analysis
    /// there is, so asking for the bundle that includes them costs a pass that
    /// never reads them dearly.
    pub(crate) fn placement<'a>(&'a mut self, function: &Function) -> PlacementAnalyses<'a> {
        self.dominators(function);
        self.instruction_layout(function);
        self.loops(function);
        self.guards(function);
        PlacementAnalyses {
            dominators: self.dominators.as_ref().unwrap(),
            instruction_layout: self.instruction_layout.as_ref().unwrap(),
            loops: self.loops.as_deref().unwrap(),
            guards: self.guards.as_ref().unwrap(),
        }
    }

    pub(crate) fn get<'a>(&'a mut self, function: &Function) -> FunctionAnalyses<'a> {
        self.cfg(function);
        self.dominators(function);
        self.instruction_layout(function);
        self.effects(function);
        self.guards(function);
        FunctionAnalyses {
            cfg: self.cfg.as_ref().unwrap(),
            dominators: self.dominators.as_ref().unwrap(),
            instruction_layout: self.instruction_layout.as_ref().unwrap(),
            effects: self.effects.as_ref().unwrap(),
            guards: self.guards.as_ref().unwrap(),
        }
    }

    pub(crate) fn invalidate(&mut self) {
        self.cfg = None;
        self.dominators = None;
        self.instruction_layout = None;
        self.loops = None;
        self.effects = None;
        self.guards = None;
    }

    pub(crate) fn report(
        &mut self,
        kind: OptimizationRemarkKind,
        transformation: impl Into<String>,
        count: u64,
        message: impl Into<String>,
    ) {
        let Some(instrumentation) = &mut self.instrumentation else {
            return;
        };
        if count == 0 {
            return;
        }
        instrumentation.attempted_transformations += count;
        if kind == OptimizationRemarkKind::Applied {
            instrumentation.successful_transformations += count;
        }
        if instrumentation.collect_remarks {
            instrumentation.remarks.push(OptimizationRemark {
                kind,
                transformation: transformation.into(),
                message: message.into(),
            });
        }
    }

    fn begin_pass(&mut self, collect_remarks: bool) {
        self.instrumentation = Some(PassInstrumentation {
            collect_remarks,
            ..PassInstrumentation::default()
        });
    }

    fn finish_pass(&mut self, changed: bool) -> PassInstrumentation {
        let mut instrumentation = self.instrumentation.take().expect("pass instrumentation was enabled");
        if instrumentation.attempted_transformations == 0 {
            instrumentation.attempted_transformations = 1;
            instrumentation.successful_transformations = u64::from(changed);
        }
        assert_eq!(
            instrumentation.successful_transformations != 0,
            changed,
            "pass instrumentation disagrees with its result"
        );
        instrumentation
    }
}

pub(crate) type FunctionPass = fn(&mut Function, &mut AnalysisManager) -> bool;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub(crate) struct PassManagerOptions {
    pub(crate) collect_remarks: bool,
    pub(crate) dump_changed_ir: bool,
}

pub(crate) struct PassRunner {
    analyses: AnalysisManager,
    options: Option<PassManagerOptions>,
}

impl PassRunner {
    pub(crate) fn new(options: Option<PassManagerOptions>) -> Self {
        Self {
            analyses: AnalysisManager::default(),
            options,
        }
    }

    pub(crate) fn run(
        &mut self,
        function: &mut Function,
        name: &'static str,
        pass: FunctionPass,
    ) -> (bool, Option<PassRunReport>) {
        let ir_before = self
            .options
            .is_some_and(|options| options.dump_changed_ir)
            .then(|| function_to_string(function));
        if let Some(options) = self.options {
            self.analyses.begin_pass(options.collect_remarks);
        }
        let started_at = self.options.map(|_| Instant::now());
        let changed = pass(function, &mut self.analyses);
        let elapsed = started_at.map(|started_at| started_at.elapsed());
        let instrumentation = self.options.map(|_| self.analyses.finish_pass(changed));
        if changed {
            function.recompute_machine_state_dependencies();
            self.analyses.invalidate();
        }
        function
            .validate()
            .unwrap_or_else(|error| panic!("SSA pass '{name}' produced invalid IR: {error}"));
        let report = instrumentation.map(|instrumentation| PassRunReport {
            name: name.to_string(),
            elapsed: elapsed.expect("pass timing was enabled with instrumentation"),
            changed,
            attempted_transformations: instrumentation.attempted_transformations,
            successful_transformations: instrumentation.successful_transformations,
            remarks: instrumentation.remarks,
            ir_before: changed.then_some(ir_before).flatten(),
            ir_after: (self.options.is_some_and(|options| options.dump_changed_ir) && changed)
                .then(|| function_to_string(function)),
        });
        (changed, report)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ssa::Terminator;

    fn report_change(_: &mut Function, analyses: &mut AnalysisManager) -> bool {
        analyses.report(
            OptimizationRemarkKind::Applied,
            "test-change",
            1,
            "changed the test function",
        );
        true
    }

    fn report_no_change(_: &mut Function, analyses: &mut AnalysisManager) -> bool {
        analyses.report(
            OptimizationRemarkKind::Missed,
            "test-change",
            1,
            "the test condition was false",
        );
        false
    }

    fn compute_and_invalidate_analyses(function: &mut Function, analyses: &mut AnalysisManager) -> bool {
        analyses.get(function);
        true
    }

    fn assert_analyses_were_invalidated(_: &mut Function, analyses: &mut AnalysisManager) -> bool {
        assert!(analyses.cfg.is_none());
        assert!(analyses.dominators.is_none());
        assert!(analyses.instruction_layout.is_none());
        assert!(analyses.loops.is_none());
        assert!(analyses.effects.is_none());
        assert!(analyses.guards.is_none());
        false
    }

    fn empty_function() -> Function {
        let mut function = Function::new("empty", Vec::new(), Vec::new());
        function.set_terminator(function.entry, Terminator::Return(Vec::new()));
        function
    }

    #[test]
    fn reports_pass_runs() {
        let mut function = empty_function();
        let mut runner = PassRunner::new(Some(PassManagerOptions {
            collect_remarks: true,
            dump_changed_ir: true,
        }));

        let (_, changed) = runner.run(&mut function, "report-change", report_change);
        let changed = changed.unwrap();
        assert_eq!(changed.attempted_transformations, 1);
        assert_eq!(changed.successful_transformations, 1);
        assert_eq!(changed.remarks[0].kind, OptimizationRemarkKind::Applied);
        assert!(changed.ir_before.is_some());
        assert!(changed.ir_after.is_some());

        let (_, unchanged) = runner.run(&mut function, "report-no-change", report_no_change);
        let unchanged = unchanged.unwrap();
        assert_eq!(unchanged.attempted_transformations, 1);
        assert_eq!(unchanged.successful_transformations, 0);
        assert_eq!(unchanged.remarks[0].kind, OptimizationRemarkKind::Missed);
        assert!(unchanged.ir_before.is_none());
        assert!(unchanged.ir_after.is_none());
    }

    #[test]
    fn invalidates_analyses_after_changes() {
        let mut function = empty_function();
        let mut runner = PassRunner::new(None);
        assert!(
            runner
                .run(&mut function, "compute-and-invalidate", compute_and_invalidate_analyses,)
                .0
        );
        runner.run(&mut function, "assert-invalidated", assert_analyses_were_invalidated);
    }
}
