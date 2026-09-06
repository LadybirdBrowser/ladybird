/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Order-maintenance tokens for cascade source order.
//!
//! Source order is a comparison, never a stored rank. Tokens are stable handles into a flat label
//! table, while a second vector keeps the tokens in order. Insertion normally chooses the midpoint
//! of the neighbouring labels. When a gap is exhausted, all live labels are spread evenly again.
//!
//! Numeric relabeling changes no semantic version. Nothing downstream uses a label as identity; it
//! only reads the order labels imply. Relabeling is therefore invisible to selector truth, cascade
//! winners, and computed values.

use super::capacity::capacity_bytes;

define_id! {
    /// A stable handle on one position in an order. The numeric label behind it may be rewritten at
    /// any time; the handle does not change.
    pub struct OrderToken();
}

impl OrderToken {
    #[must_use]
    pub fn raw(self) -> u32 {
        self.0
    }
}

/// One totally ordered axis: stylesheet order within a tree context, nested rule order within a
/// sheet, or layer order within an origin.
#[derive(Default)]
pub struct OrderMaintenance {
    labels: Vec<u64>,
    free_entries: Vec<u32>,
    order: Vec<OrderToken>,
}

impl OrderMaintenance {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.order.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.order.is_empty()
    }

    /// The comparable rank of a position.
    #[must_use]
    pub fn rank(&self, token: OrderToken) -> u64 {
        self.labels[token.0 as usize]
    }

    /// Insert at the end of the order.
    pub fn insert_last(&mut self) -> OrderToken {
        self.insert_at(self.order.len())
    }

    /// Insert at the start of the order.
    #[cfg(test)]
    pub fn insert_first(&mut self) -> OrderToken {
        self.insert_at(0)
    }

    #[cfg(test)]
    pub fn insert_after(&mut self, after: OrderToken) -> OrderToken {
        let position = self.locate(after);
        self.insert_at(position + 1)
    }

    pub fn insert_before(&mut self, before: OrderToken) -> OrderToken {
        let position = self.locate(before);
        self.insert_at(position)
    }

    pub fn remove(&mut self, token: OrderToken) {
        let position = self.locate(token);
        self.order.remove(position);
        self.free_entries.push(token.0);
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.labels, self.free_entries, self.order];
            cached [];
            nested [];
            skip [];
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = OrderToken> + '_ {
        self.order.iter().copied()
    }

    fn insert_at(&mut self, position: usize) -> OrderToken {
        let (mut lower, mut upper) = self.bounds(position);
        if upper - lower < 2 {
            self.relabel();
            (lower, upper) = self.bounds(position);
        }
        assert!(
            upper - lower >= 2,
            "a relabeled order always leaves room for one insertion"
        );

        let token = self.allocate_entry(lower + (upper - lower) / 2);
        self.order.insert(position, token);
        token
    }

    fn bounds(&self, position: usize) -> (u64, u64) {
        let lower = position
            .checked_sub(1)
            .map_or(0, |index| self.labels[self.order[index].0 as usize]);
        let upper = self
            .order
            .get(position)
            .map_or(u64::MAX, |token| self.labels[token.0 as usize]);
        (lower, upper)
    }

    fn relabel(&mut self) {
        let length = u64::try_from(self.order.len()).expect("order length exceeds label space");
        let step = u64::MAX / (length + 1);
        assert!(step >= 2, "order label space exhausted");
        for (index, token) in self.order.iter().enumerate() {
            self.labels[token.0 as usize] = step * (index as u64 + 1);
        }
    }

    fn allocate_entry(&mut self, label: u64) -> OrderToken {
        match self.free_entries.pop() {
            Some(index) => {
                self.labels[index as usize] = label;
                OrderToken(index)
            }
            None => {
                self.labels.push(label);
                OrderToken(u32::try_from(self.labels.len() - 1).expect("order token space exhausted"))
            }
        }
    }

    fn locate(&self, token: OrderToken) -> usize {
        self.order
            .iter()
            .position(|candidate| *candidate == token)
            .expect("an order token must be live")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn is_sorted(order: &OrderMaintenance, tokens: &[OrderToken]) -> bool {
        tokens.windows(2).all(|pair| order.rank(pair[0]) < order.rank(pair[1]))
    }

    #[test]
    fn appending_keeps_positions_ordered() {
        let mut order = OrderMaintenance::new();
        let tokens: Vec<OrderToken> = (0..500).map(|_| order.insert_last()).collect();
        assert_eq!(order.len(), 500);
        assert!(is_sorted(&order, &tokens));
        assert_eq!(order.iter().collect::<Vec<_>>(), tokens);
    }

    #[test]
    fn prepending_keeps_positions_ordered() {
        let mut order = OrderMaintenance::new();
        let mut tokens: Vec<OrderToken> = Vec::new();
        for _ in 0..500 {
            tokens.insert(0, order.insert_first());
        }
        assert!(is_sorted(&order, &tokens));
        assert_eq!(order.iter().collect::<Vec<_>>(), tokens);
    }

    #[test]
    fn repeatedly_inserting_at_the_same_point_keeps_positions_ordered() {
        let mut order = OrderMaintenance::new();
        let first = order.insert_last();
        let last = order.insert_last();

        let mut middles = Vec::new();
        for _ in 0..2000 {
            middles.push(order.insert_after(first));
        }
        middles.reverse();

        let mut expected = vec![first];
        expected.extend(middles);
        expected.push(last);
        assert_eq!(order.iter().collect::<Vec<_>>(), expected);
        assert!(is_sorted(&order, &expected));
    }

    #[test]
    fn interleaved_insertion_and_removal_keeps_the_order_consistent() {
        let mut order = OrderMaintenance::new();
        let mut tokens: Vec<OrderToken> = (0..200).map(|_| order.insert_last()).collect();

        for index in (0..tokens.len()).step_by(3).rev() {
            order.remove(tokens[index]);
            tokens.remove(index);
        }
        assert_eq!(order.len(), tokens.len());
        assert!(is_sorted(&order, &tokens));

        for index in (0..tokens.len()).step_by(2) {
            let inserted = order.insert_before(tokens[index]);
            assert!(order.rank(inserted) < order.rank(tokens[index]));
        }
        assert_eq!(order.iter().count(), order.len());
    }

    #[test]
    fn removing_every_position_reuses_the_entry_arena() {
        let mut order = OrderMaintenance::new();
        let tokens: Vec<OrderToken> = (0..300).map(|_| order.insert_last()).collect();
        for token in tokens {
            order.remove(token);
        }
        assert!(order.is_empty());
        assert_eq!(order.iter().count(), 0);

        let bytes = order.capacity_bytes();
        let reinserted: Vec<OrderToken> = (0..300).map(|_| order.insert_last()).collect();
        assert!(is_sorted(&order, &reinserted));
        assert_eq!(order.capacity_bytes(), bytes);
    }

    #[test]
    fn a_rank_orders_the_same_way_a_comparison_does() {
        let mut order = OrderMaintenance::new();
        let first = order.insert_last();
        let second = order.insert_last();
        for _ in 0..300 {
            order.insert_after(first);
        }
        assert!(order.rank(first) < order.rank(second));
        let all: Vec<OrderToken> = order.iter().collect();
        assert!(all.windows(2).all(|pair| order.rank(pair[0]) < order.rank(pair[1])));
    }
}
