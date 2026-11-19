/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Optimization passes over the IR, run between graph building and register
//! allocation.
//!
//! The builder translates bytecode to SSA form and records its speculation
//! decisions as explicit checks and typed nodes; it folds nothing. The passes
//! then simplify the graph, one transformation per pass. Debug builds and
//! tests check the graph's invariants (see `verify`) after building and after
//! every pass.

// NB: A pass with `allow(clippy::disallowed_types)` still uses containers of the heap (see clippy.toml in the
//     directory of the crate). It loses the `allow` when its data is in zones.
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod bounds_check_elimination;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod branch_fusion;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod branch_splitting;
mod check_elimination;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod dce;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod dominators;
pub mod edit;
mod enumerated_keys;
mod escape_analysis;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod exit_unboxing;
mod facts;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod frame_initialization;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod gvn;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod interrupt_check_elimination;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod key_strings;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod licm;
mod loop_rotation;
mod representation;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod simplify;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
mod truncation;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod verify;

#[cfg(test)]
mod tests;

use ak::ColumnFull;
use ak::Zone;

use crate::bytecode::FrameLayout;
use crate::ir::Graph;
use dominators::Dominators;

/// One transformation of the graph.
struct Pass {
    name: &'static str,
    run: Run,
}

/// How a pass runs. A pass that repeats a step whose work data grows with the graph keeps the data of each step in
/// the step zone, which drops it when the step ends: the work zone takes nothing back before the pass ends, so it
/// would hold the data of every step. A pass whose steps hand data on to later steps moves what is still in use from
/// one step zone to the other (see `check_elimination`).
#[derive(Clone, Copy)]
enum Run {
    Plain(fn(&mut Graph, &Zone)),
    /// The pass computes the dominators of the graph into the tables it gets. It fails if the graph is too large
    /// for them.
    WithDominators(fn(&mut Graph, &mut Dominators, &Zone) -> Result<(), ColumnFull>),
    /// A pass that keeps what lasts for the whole pass in the work zone, and the data of its steps in the step zones
    /// (see `Scratch::steps`).
    Steps(fn(&mut Graph, &Zone, &mut [Zone; 2])),
    StepsWithDominators(fn(&mut Graph, &mut Dominators, &Zone, &mut [Zone; 2]) -> Result<(), ColumnFull>),
}

/// How many bytes of memory each step zone keeps after a pass, so that the steps of most functions take no memory from
/// the system again.
const STEP_MEMORY_KEPT: usize = 4 << 20;

impl Run {
    /// Runs the pass. It keeps its own data in `work` and `steps`, which hold none of it when this returns.
    fn run(
        self,
        graph: &mut Graph,
        dominators: &mut Dominators,
        work: &mut Zone,
        steps: &mut [Zone; 2],
    ) -> Result<(), ColumnFull> {
        let result = work.scope(|work| match self {
            Run::Plain(run) => {
                run(graph, work);
                Ok(())
            }
            Run::WithDominators(run) => run(graph, dominators, work),
            Run::Steps(run) => {
                run(graph, work, steps);
                Ok(())
            }
            Run::StepsWithDominators(run) => run(graph, dominators, work, steps),
        });
        if let Run::Steps(_) | Run::StepsWithDominators(_) = self {
            // NB: A pass that moves its data between the step zones leaves data in both. The memory of large steps
            //     goes back to the system, so that it does not stay on top of what the stages after this one take
            //     in the work zone.
            for zone in steps {
                zone.reset();
                zone.trim(STEP_MEMORY_KEPT);
            }
        }
        result
    }
}

/// The passes, in the order they run.
const PASSES: &[Pass] = &[
    Pass {
        name: "loop rotation",
        run: Run::StepsWithDominators(loop_rotation::run),
    },
    Pass {
        name: "trivial phi removal",
        run: Run::Plain(edit::remove_trivial_phis),
    },
    Pass {
        name: "truncation",
        run: Run::Plain(truncation::run),
    },
    Pass {
        name: "simplification",
        run: Run::Plain(simplify::run),
    },
    Pass {
        name: "representation selection",
        run: Run::Plain(representation::run),
    },
    Pass {
        name: "simplification",
        run: Run::Plain(simplify::run),
    },
    Pass {
        name: "branch splitting",
        run: Run::StepsWithDominators(branch_splitting::run),
    },
    Pass {
        name: "global value numbering",
        run: Run::WithDominators(gvn::run),
    },
    Pass {
        name: "check elimination",
        run: Run::Steps(check_elimination::run),
    },
    Pass {
        name: "simplification",
        run: Run::Plain(simplify::run_after_check_elimination),
    },
    Pass {
        name: "global value numbering",
        run: Run::WithDominators(gvn::run),
    },
    Pass {
        name: "loop invariant code motion",
        run: Run::WithDominators(licm::run),
    },
    Pass {
        name: "bounds check elimination",
        run: Run::WithDominators(bounds_check_elimination::run),
    },
    Pass {
        name: "interrupt check elimination",
        run: Run::WithDominators(interrupt_check_elimination::run),
    },
    Pass {
        name: "exit unboxing",
        run: Run::Plain(exit_unboxing::run),
    },
    Pass {
        name: "dead code elimination",
        run: Run::Plain(dce::run),
    },
    Pass {
        name: "escape analysis",
        run: Run::Steps(escape_analysis::run),
    },
    Pass {
        name: "dead code elimination",
        run: Run::Plain(dce::run),
    },
    Pass {
        name: "enumerated keys",
        run: Run::Plain(enumerated_keys::run),
    },
    Pass {
        name: "branch fusion",
        run: Run::Plain(branch_fusion::run),
    },
    Pass {
        name: "key strings",
        run: Run::Plain(key_strings::run),
    },
];

/// Runs every pass over `graph`, whose compiled function has frame layout
/// `layout` (for dumps of invalid graphs). Checks the graph's invariants
/// before and after every pass in debug builds and tests, or if `verify`.
pub fn optimize(
    graph: &mut Graph,
    layout: &FrameLayout,
    verify: bool,
    dominators: &mut Dominators,
    work: &mut Zone,
    steps: &mut [Zone; 2],
) -> Result<(), ColumnFull> {
    run_passes(graph, layout, verify, dominators, work, steps, &mut |_, _| {})
}

/// `optimize()`, returning the graph as text after building it and after
/// every pass, each headed by "=== after <pass>" (for tests of passes).
pub fn optimize_with_dumps(
    graph: &mut Graph,
    layout: &FrameLayout,
    verify: bool,
    dominators: &mut Dominators,
    work: &mut Zone,
    steps: &mut [Zone; 2],
) -> Result<String, ColumnFull> {
    let mut text = String::new();
    run_passes(graph, layout, verify, dominators, work, steps, &mut |graph, after| {
        text.push_str(&format!("=== after {after}\n"));
        text.push_str(&crate::ir::dump(graph, layout));
    })?;
    Ok(text)
}

fn run_passes(
    graph: &mut Graph,
    layout: &FrameLayout,
    verify: bool,
    dominators: &mut Dominators,
    work: &mut Zone,
    steps: &mut [Zone; 2],
    observe: &mut dyn FnMut(&Graph, &str),
) -> Result<(), ColumnFull> {
    let verify = verify || cfg!(debug_assertions) || cfg!(test);
    // NB: Graph building builds the loops around OSR entries even without
    //     a forward entry, which no entry reaches when every path out of the
    //     inner loop ends in an exit.
    work.scope(|work| edit::remove_unreachable_blocks(graph, work));
    check(graph, layout, verify, dominators, "graph building");
    observe(graph, "graph building");
    for pass in PASSES {
        pass.run.run(graph, dominators, work, steps)?;
        check(graph, layout, verify, dominators, pass.name);
        observe(graph, pass.name);
    }
    // NB: Nodes that took different refinements of one value compute the
    //     same once the refinements are gone, and nothing moves anymore.
    work.scope(|work| edit::remove_refinements(graph, work));
    Run::WithDominators(gvn::run).run(graph, dominators, work, steps)?;
    // NB: Checks that merged took their frame states along, which may have been the last uses of what those refer to.
    Run::Plain(dce::run).run(graph, dominators, work, steps)?;
    check(graph, layout, verify, dominators, "refinement removal");
    observe(graph, "refinement removal");
    frame_initialization::run(graph, layout);
    check(graph, layout, verify, dominators, "frame initialization");
    work.scope(|work| edit::remove_unused_frame_states(graph, work));
    // NB: Only the final block order puts the cold code last; passes see
    //     every block after its predecessors (except over back edges).
    //     On-stack replacement entries run once per entry, so they are cold
    //     code too, which keeps the blocks they lead to from starting with
    //     their register state.
    let osr_entries = graph.osr_entries.to_vec();
    for (_, block) in osr_entries {
        graph.blocks[block.index()].is_cold = true;
    }
    work.scope(|work| edit::move_cold_blocks_last(graph, work));
    check(graph, layout, verify, dominators, "cold block layout");
    observe(graph, "frame initialization");
    Ok(())
}

fn check(graph: &Graph, layout: &FrameLayout, verify: bool, dominators: &mut Dominators, after: &str) {
    if !verify {
        return;
    }
    if let Err(errors) = verify::verify_with(graph, dominators).and(verify::verify_frame_writes(graph, layout)) {
        panic!(
            "invalid graph after {after}:\n{errors}\n{}",
            crate::ir::dump(graph, layout)
        );
    }
}
