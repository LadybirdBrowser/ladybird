/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Sequentializing parallel moves.

use super::Location;
use super::Move;

/// Orders a set of moves that happen in parallel (every source is read
/// before any destination is written) into a sequence with the same effect.
/// Destinations must be distinct and must not be constants. Cycles are broken
/// through `temp`, which must not appear in `moves`.
pub fn resolve_parallel_moves(moves: &[Move], temp: Location) -> Vec<Move> {
    let mut pending = moves.iter().copied().filter(|m| m.from != m.to).collect::<Vec<_>>();
    debug_assert!(pending.iter().all(|m| !matches!(m.to, Location::Constant(_))));
    debug_assert!(pending.iter().all(|m| m.from != temp && m.to != temp));
    let mut result = Vec::with_capacity(pending.len());
    while !pending.is_empty() {
        let ready = pending
            .iter()
            .position(|candidate| pending.iter().all(|other| other.from != candidate.to));
        if let Some(index) = ready {
            result.push(pending.remove(index));
            continue;
        }
        // Every destination is still needed as a source, so the moves form
        // cycles. Save one destination's value and read it from the temp.
        let saved = pending[0].to;
        result.push(Move { from: saved, to: temp });
        for pending_move in &mut pending {
            if pending_move.from == saved {
                pending_move.from = temp;
            }
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    fn simulate(initial: &[(Location, u32)], moves: &[Move]) -> HashMap<Location, u32> {
        let mut state = initial.iter().copied().collect::<HashMap<_, _>>();
        for m in moves {
            let value = match m.from {
                Location::Constant(bits) => u32::try_from(bits).unwrap(),
                from => state[&from],
            };
            state.insert(m.to, value);
        }
        state
    }

    fn check(moves: &[Move]) {
        let temp = Location::Stack(99);
        let locations = moves
            .iter()
            .flat_map(|m| [m.from, m.to])
            .filter(|location| !matches!(location, Location::Constant(_)))
            .collect::<Vec<_>>();
        let initial = locations
            .iter()
            .enumerate()
            .map(|(index, location)| (*location, u32::try_from(index).unwrap() + 1000))
            .collect::<HashMap<_, _>>()
            .into_iter()
            .collect::<Vec<_>>();
        let before = initial.iter().copied().collect::<HashMap<_, _>>();
        let after = simulate(&initial, &resolve_parallel_moves(moves, temp));
        for m in moves {
            let expected = match m.from {
                Location::Constant(bits) => u32::try_from(bits).unwrap(),
                from => before[&from],
            };
            assert_eq!(after[&m.to], expected, "{m:?}");
        }
    }

    #[test]
    fn resolves_chains_cycles_and_fan_out() {
        use Location::*;
        let m = |from, to| Move { from, to };
        // A chain must be emitted back to front.
        check(&[m(Register(1), Register(2)), m(Register(2), Register(3))]);
        // A swap needs the temp.
        check(&[m(Stack(0), Stack(1)), m(Stack(1), Stack(0))]);
        // A three element rotation with a branch off the cycle.
        check(&[
            m(Register(0), Register(1)),
            m(Register(1), Stack(2)),
            m(Stack(2), Register(0)),
            m(Register(1), Stack(5)),
            m(Constant(7), Register(4)),
        ]);
        // Two independent cycles.
        check(&[
            m(Stack(0), Stack(1)),
            m(Stack(1), Stack(0)),
            m(Register(3), Register(4)),
            m(Register(4), Register(3)),
        ]);
        // Self moves disappear.
        assert!(resolve_parallel_moves(&[m(Register(1), Register(1))], Stack(9)).is_empty());
    }
}
