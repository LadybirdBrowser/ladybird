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

mod branch_fusion;
mod check_elimination;
mod dce;
pub mod dominators;
pub mod edit;
mod escape_analysis;
mod facts;
mod frame_initialization;
mod gvn;
mod licm;
mod representation;
mod simplify;
pub mod verify;

#[cfg(test)]
mod tests;

use crate::bytecode::FrameLayout;
use crate::ir::Graph;

/// One transformation of the graph.
struct Pass {
    name: &'static str,
    run: fn(&mut Graph),
}

/// The passes, in the order they run.
const PASSES: &[Pass] = &[
    Pass {
        name: "trivial phi removal",
        run: edit::remove_trivial_phis,
    },
    Pass {
        name: "simplification",
        run: simplify::run,
    },
    Pass {
        name: "representation selection",
        run: representation::run,
    },
    Pass {
        name: "simplification",
        run: simplify::run,
    },
    Pass {
        name: "check elimination",
        run: check_elimination::run,
    },
    Pass {
        name: "global value numbering",
        run: gvn::run,
    },
    Pass {
        name: "loop invariant code motion",
        run: licm::run,
    },
    Pass {
        name: "dead code elimination",
        run: dce::run,
    },
    Pass {
        name: "escape analysis",
        run: escape_analysis::run,
    },
    Pass {
        name: "dead code elimination",
        run: dce::run,
    },
    Pass {
        name: "branch fusion",
        run: branch_fusion::run,
    },
];

/// Runs every pass over `graph`, whose compiled function has frame layout
/// `layout` (for dumps of invalid graphs). Checks the graph's invariants
/// before and after every pass in debug builds and tests, or if `verify`.
pub fn optimize(graph: &mut Graph, layout: &FrameLayout, verify: bool) {
    run_passes(graph, layout, verify, &mut |_, _| {});
}

/// `optimize()`, returning the graph as text after building it and after
/// every pass, each headed by "=== after <pass>" (for tests of passes).
pub fn optimize_with_dumps(graph: &mut Graph, layout: &FrameLayout, verify: bool) -> String {
    let mut text = String::new();
    run_passes(graph, layout, verify, &mut |graph, after| {
        text.push_str(&format!("=== after {after}\n"));
        text.push_str(&crate::ir::dump(graph, layout));
    });
    text
}

fn run_passes(graph: &mut Graph, layout: &FrameLayout, verify: bool, observe: &mut dyn FnMut(&Graph, &str)) {
    let verify = verify || cfg!(debug_assertions) || cfg!(test);
    // NB: Graph building builds the loops around OSR entries even without
    //     a forward entry, which no entry reaches when every path out of the
    //     inner loop ends in an exit.
    edit::remove_unreachable_blocks(graph);
    check(graph, layout, verify, "graph building");
    observe(graph, "graph building");
    for pass in PASSES {
        (pass.run)(graph);
        check(graph, layout, verify, pass.name);
        observe(graph, pass.name);
    }
    // NB: Nodes that took different refinements of one value compute the
    //     same once the refinements are gone, and nothing moves anymore.
    edit::remove_refinements(graph);
    gvn::run(graph);
    check(graph, layout, verify, "refinement removal");
    observe(graph, "refinement removal");
    frame_initialization::run(graph, layout);
    check(graph, layout, verify, "frame initialization");
    edit::remove_unused_frame_states(graph);
    // NB: Only the final block order puts the cold code last; passes see
    //     every block after its predecessors (except over back edges).
    //     On-stack replacement entries run once per entry, so they are cold
    //     code too, which keeps the blocks they lead to from starting with
    //     their register state.
    for (_, block) in graph.osr_entries.clone() {
        graph.blocks[block.index()].is_cold = true;
    }
    edit::move_cold_blocks_last(graph);
    check(graph, layout, verify, "cold block layout");
    observe(graph, "frame initialization");
}

fn check(graph: &Graph, layout: &FrameLayout, verify: bool, after: &str) {
    if !verify {
        return;
    }
    if let Err(errors) = verify::verify(graph).and(verify::verify_frame_writes(graph, layout)) {
        panic!(
            "invalid graph after {after}:\n{errors}\n{}",
            crate::ir::dump(graph, layout)
        );
    }
}
