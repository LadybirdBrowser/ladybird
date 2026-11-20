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

use ak::Zone;
use core::fmt;

use crate::CompileFailure;
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
    /// A pass that keeps its data in the work zone.
    Once(fn(&mut Graph, &Zone)),
    /// A pass that keeps what lasts for the whole pass in the work zone, and the data of its steps in the step zones
    /// (see `Scratch::steps`).
    Steps(fn(&mut Graph, &Zone, &mut [Zone; 2])),
}

/// How many bytes of memory each step zone keeps after a pass, so that the steps of most functions take no memory from
/// the system again.
const STEP_MEMORY_KEPT: usize = 4 << 20;

/// Runs the pass `run`. It keeps its own data in `work` and `steps`, which hold none of it when this returns.
fn run_pass(run: Run, graph: &mut Graph, work: &mut Zone, steps: &mut [Zone; 2]) -> Result<(), CompileFailure> {
    fits(graph)?;
    match run {
        Run::Once(run) => work.scope(|work| run(graph, work)),
        Run::Steps(run) => {
            work.scope(|work| run(graph, work, steps));
            // NB: A pass that moves its data between the step zones leaves data in both. The memory of large steps
            //     goes back to the system, so that it does not stay on top of what the stages after this one take
            //     in the work zone.
            for zone in steps {
                zone.reset();
                zone.trim(STEP_MEMORY_KEPT);
            }
        }
    }
    Ok(())
}

/// Fails the compile if the passes cannot compute the dominators of `graph`, which has too many blocks.
// NB: The passes add blocks, so the count is checked again before each of them.
fn fits(graph: &Graph) -> Result<(), CompileFailure> {
    if graph.blocks.len() > Dominators::MAX_BLOCKS {
        return Err(CompileFailure::TooLarge);
    }
    Ok(())
}

/// The passes, in the order they run.
const PASSES: &[Pass] = &[
    Pass {
        name: "loop rotation",
        run: Run::Steps(loop_rotation::run),
    },
    Pass {
        name: "trivial phi removal",
        run: Run::Once(edit::remove_trivial_phis),
    },
    Pass {
        name: "truncation",
        run: Run::Once(truncation::run),
    },
    Pass {
        name: "simplification",
        run: Run::Once(simplify::run),
    },
    Pass {
        name: "representation selection",
        run: Run::Once(representation::run),
    },
    Pass {
        name: "simplification",
        run: Run::Once(simplify::run),
    },
    Pass {
        name: "branch splitting",
        run: Run::Steps(branch_splitting::run),
    },
    Pass {
        name: "global value numbering",
        run: Run::Once(gvn::run),
    },
    Pass {
        name: "check elimination",
        run: Run::Steps(check_elimination::run),
    },
    Pass {
        name: "simplification",
        run: Run::Once(simplify::run_after_check_elimination),
    },
    Pass {
        name: "global value numbering",
        run: Run::Once(gvn::run),
    },
    Pass {
        name: "loop invariant code motion",
        run: Run::Once(licm::run),
    },
    Pass {
        name: "bounds check elimination",
        run: Run::Once(bounds_check_elimination::run),
    },
    Pass {
        name: "interrupt check elimination",
        run: Run::Once(interrupt_check_elimination::run),
    },
    Pass {
        name: "exit unboxing",
        run: Run::Once(exit_unboxing::run),
    },
    Pass {
        name: "dead code elimination",
        run: Run::Once(dce::run),
    },
    Pass {
        name: "escape analysis",
        run: Run::Steps(escape_analysis::run),
    },
    Pass {
        name: "dead code elimination",
        run: Run::Once(dce::run),
    },
    Pass {
        name: "enumerated keys",
        run: Run::Once(enumerated_keys::run),
    },
    Pass {
        name: "branch fusion",
        run: Run::Once(branch_fusion::run),
    },
    Pass {
        name: "key strings",
        run: Run::Once(key_strings::run),
    },
];

/// Runs every pass over `graph`, whose compiled function has frame layout
/// `layout` (for dumps of invalid graphs). Checks the graph's invariants
/// before and after every pass in debug builds and tests, or if `verify`.
pub fn optimize(
    graph: &mut Graph,
    layout: &FrameLayout,
    verify: bool,
    work: &mut Zone,
    steps: &mut [Zone; 2],
) -> Result<(), CompileFailure> {
    run_passes(graph, layout, verify, work, steps, &mut |_, _| {})
}

/// `optimize()`, writing the graph as text to `dumps` after building it and
/// after every pass, each headed by "=== after <pass>" (for tests of passes).
/// Text that `dumps` has no room for is left out.
pub fn optimize_with_dumps(
    graph: &mut Graph,
    layout: &FrameLayout,
    verify: bool,
    work: &mut Zone,
    steps: &mut [Zone; 2],
    dumps: &mut dyn fmt::Write,
) -> Result<(), CompileFailure> {
    run_passes(graph, layout, verify, work, steps, &mut |graph, after| {
        // NB: A write fails only when `dumps` has no room for more text.
        let _ = write!(dumps, "=== after {after}\n{}", crate::ir::dump(graph, layout));
    })
}

fn run_passes(
    graph: &mut Graph,
    layout: &FrameLayout,
    verify: bool,
    work: &mut Zone,
    steps: &mut [Zone; 2],
    observe: &mut dyn FnMut(&Graph, &str),
) -> Result<(), CompileFailure> {
    let verify = verify || cfg!(debug_assertions) || cfg!(test);
    // NB: Graph building builds the loops around OSR entries even without
    //     a forward entry, which no entry reaches when every path out of the
    //     inner loop ends in an exit.
    work.scope(|work| edit::remove_unreachable_blocks(graph, work));
    check(graph, layout, verify, work, "graph building");
    observe(graph, "graph building");
    for pass in PASSES {
        run_pass(pass.run, graph, work, steps)?;
        check(graph, layout, verify, work, pass.name);
        observe(graph, pass.name);
    }
    // NB: Nodes that took different refinements of one value compute the
    //     same once the refinements are gone, and nothing moves anymore.
    work.scope(|work| edit::remove_refinements(graph, work));
    run_pass(Run::Once(gvn::run), graph, work, steps)?;
    // NB: Checks that merged took their frame states along, which may have been the last uses of what those refer to.
    run_pass(Run::Once(dce::run), graph, work, steps)?;
    check(graph, layout, verify, work, "refinement removal");
    observe(graph, "refinement removal");
    work.scope(|work| frame_initialization::run(graph, layout, work));
    check(graph, layout, verify, work, "frame initialization");
    work.scope(|work| edit::remove_unused_frame_states(graph, work));
    // NB: Only the final block order puts the cold code last; passes see
    //     every block after its predecessors (except over back edges).
    //     On-stack replacement entries run once per entry, so they are cold
    //     code too, which keeps the blocks they lead to from starting with
    //     their register state.
    for index in 0..graph.osr_entries.len() {
        let (_, block) = graph.osr_entries[index];
        graph.blocks[block.index()].is_cold = true;
    }
    work.scope(|work| edit::move_cold_blocks_last(graph, work));
    check(graph, layout, verify, work, "cold block layout");
    observe(graph, "frame initialization");
    Ok(())
}

fn check(graph: &Graph, layout: &FrameLayout, verify: bool, work: &mut Zone, after: &str) {
    if !verify {
        return;
    }
    // NB: The errors are in the work zone, so the panic happens while it holds them.
    work.scope(|work| {
        if let Err(errors) = verify::verify_with(graph, work).and(verify::verify_frame_writes(graph, layout, work)) {
            panic!(
                "invalid graph after {after}:\n{errors}\n{}",
                crate::ir::dump(graph, layout)
            );
        }
    });
}
