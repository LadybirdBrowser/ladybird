/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Sequentializing parallel moves.

use ak::ZoneVec;

use super::Location;
use super::Move;

/// Orders the moves `pending`, which happen in parallel (every source is read
/// before any destination is written), into a sequence with the same effect, which it adds to `result`.
/// Destinations must be distinct and must not be constants. Cycles are broken
/// through `temp`, which must not appear in the moves. No move is left in `pending`.
pub fn resolve_parallel_moves(pending: &mut ZoneVec<'_, Move>, temp: Location, result: &mut ZoneVec<'_, Move>) {
    let mut kept = 0;
    for index in 0..pending.len() {
        let pending_move = pending[index];
        if pending_move.from != pending_move.to {
            pending[kept] = pending_move;
            kept += 1;
        }
    }
    pending.truncate(kept);
    debug_assert!(pending.iter().all(|m| !matches!(m.to, Location::Constant(_))));
    debug_assert!(pending.iter().all(|m| m.from != temp && m.to != temp));
    while !pending.is_empty() {
        let ready = pending
            .iter()
            .position(|candidate| pending.iter().all(|other| other.from != candidate.to));
        if let Some(index) = ready {
            result.push(pending[index]);
            // NB: The moves that are left stay in their order.
            pending.copy_within(index + 1.., index);
            pending.truncate(pending.len() - 1);
            continue;
        }
        // Every destination is still needed as a source, so the moves form
        // cycles. Save one destination's value and read it from the temp.
        let saved = pending[0].to;
        result.push(Move { from: saved, to: temp });
        for pending_move in pending.iter_mut() {
            if pending_move.from == saved {
                pending_move.from = temp;
            }
        }
    }
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

    /// The sequence that `moves`, which happen in parallel, are put in.
    fn resolve(moves: &[Move], temp: Location) -> Vec<Move> {
        let mut pending = ZoneVec::from_slice_in(moves, crate::test_zone());
        let mut result = ZoneVec::new_in(crate::test_zone());
        resolve_parallel_moves(&mut pending, temp, &mut result);
        assert!(pending.is_empty());
        result.to_vec()
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
        let after = simulate(&initial, &resolve(moves, temp));
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
        assert!(resolve(&[m(Register(1), Register(1))], Stack(9)).is_empty());
    }
}
