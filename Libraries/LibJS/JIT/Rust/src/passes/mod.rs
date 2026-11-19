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

mod bounds_check_elimination;
mod branch_fusion;
mod branch_splitting;
mod check_elimination;
mod dce;
pub mod dominators;
pub mod edit;
mod enumerated_keys;
mod escape_analysis;
mod exit_unboxing;
mod facts;
mod frame_initialization;
mod gvn;
mod interrupt_check_elimination;
mod key_strings;
mod licm;
mod loop_rotation;
mod representation;
mod simplify;
mod truncation;
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

/// How a pass is run.
#[derive(Clone, Copy)]
enum Run {
    Plain(fn(&mut Graph, &Zone)),
    /// The pass computes the dominators of the graph into the tables it gets. It fails if the graph is too large
    /// for them.
    WithDominators(fn(&mut Graph, &mut Dominators, &Zone) -> Result<(), ColumnFull>),
}

impl Run {
    /// Runs the pass. It keeps its own data in `work`, which holds none of it when this returns.
    fn run(self, graph: &mut Graph, dominators: &mut Dominators, work: &mut Zone) -> Result<(), ColumnFull> {
        work.scope(|work| match self {
            Run::Plain(run) => {
                run(graph, work);
                Ok(())
            }
            Run::WithDominators(run) => run(graph, dominators, work),
        })
    }
}

/// The passes, in the order they run.
const PASSES: &[Pass] = &[
    Pass {
        name: "loop rotation",
        run: Run::WithDominators(loop_rotation::run),
    },
    Pass {
        name: "trivial phi removal",
        run: Run::Plain(|graph, _| edit::remove_trivial_phis(graph)),
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
        run: Run::WithDominators(branch_splitting::run),
    },
    Pass {
        name: "global value numbering",
        run: Run::WithDominators(gvn::run),
    },
    Pass {
        name: "check elimination",
        run: Run::Plain(check_elimination::run),
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
        run: Run::Plain(escape_analysis::run),
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
) -> Result<(), ColumnFull> {
    run_passes(graph, layout, verify, dominators, work, &mut |_, _| {})
}

/// `optimize()`, returning the graph as text after building it and after
/// every pass, each headed by "=== after <pass>" (for tests of passes).
pub fn optimize_with_dumps(
    graph: &mut Graph,
    layout: &FrameLayout,
    verify: bool,
    dominators: &mut Dominators,
    work: &mut Zone,
) -> Result<String, ColumnFull> {
    let mut text = String::new();
    run_passes(graph, layout, verify, dominators, work, &mut |graph, after| {
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
    observe: &mut dyn FnMut(&Graph, &str),
) -> Result<(), ColumnFull> {
    let verify = verify || cfg!(debug_assertions) || cfg!(test);
    // NB: Graph building builds the loops around OSR entries even without
    //     a forward entry, which no entry reaches when every path out of the
    //     inner loop ends in an exit.
    edit::remove_unreachable_blocks(graph);
    check(graph, layout, verify, dominators, "graph building");
    observe(graph, "graph building");
    for pass in PASSES {
        pass.run.run(graph, dominators, work)?;
        check(graph, layout, verify, dominators, pass.name);
        observe(graph, pass.name);
    }
    // NB: Nodes that took different refinements of one value compute the
    //     same once the refinements are gone, and nothing moves anymore.
    edit::remove_refinements(graph);
    Run::WithDominators(gvn::run).run(graph, dominators, work)?;
    // NB: Checks that merged took their frame states along, which may have been the last uses of what those refer to.
    Run::Plain(dce::run).run(graph, dominators, work)?;
    check(graph, layout, verify, dominators, "refinement removal");
    observe(graph, "refinement removal");
    frame_initialization::run(graph, layout);
    check(graph, layout, verify, dominators, "frame initialization");
    edit::remove_unused_frame_states(graph);
    // NB: Only the final block order puts the cold code last; passes see
    //     every block after its predecessors (except over back edges).
    //     On-stack replacement entries run once per entry, so they are cold
    //     code too, which keeps the blocks they lead to from starting with
    //     their register state.
    let osr_entries = graph.osr_entries.to_vec();
    for (_, block) in osr_entries {
        graph.blocks[block.index()].is_cold = true;
    }
    edit::move_cold_blocks_last(graph);
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
