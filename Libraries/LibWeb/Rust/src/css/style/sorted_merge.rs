/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cmp::Ordering;

pub(super) enum SortedMergeEntry<'a, Left, Right> {
    Both(&'a Left, &'a Right),
    Left(&'a Left),
    Right(&'a Right),
}

pub(super) struct SortedMerge<'a, Left, Right, Compare> {
    left: &'a [Left],
    right: &'a [Right],
    left_index: usize,
    right_index: usize,
    compare: Compare,
}

pub(super) fn merge_sorted_by<'a, Left, Right, Compare>(
    left: &'a [Left],
    right: &'a [Right],
    compare: Compare,
) -> SortedMerge<'a, Left, Right, Compare>
where
    Compare: Fn(&Left, &Right) -> Ordering,
{
    SortedMerge {
        left,
        right,
        left_index: 0,
        right_index: 0,
        compare,
    }
}

impl<'a, Left, Right, Compare> Iterator for SortedMerge<'a, Left, Right, Compare>
where
    Compare: Fn(&Left, &Right) -> Ordering,
{
    type Item = SortedMergeEntry<'a, Left, Right>;

    fn next(&mut self) -> Option<Self::Item> {
        match (self.left.get(self.left_index), self.right.get(self.right_index)) {
            (Some(left), Some(right)) => match (self.compare)(left, right) {
                Ordering::Equal => {
                    self.left_index += 1;
                    self.right_index += 1;
                    Some(SortedMergeEntry::Both(left, right))
                }
                Ordering::Less => {
                    self.left_index += 1;
                    Some(SortedMergeEntry::Left(left))
                }
                Ordering::Greater => {
                    self.right_index += 1;
                    Some(SortedMergeEntry::Right(right))
                }
            },
            (Some(left), None) => {
                self.left_index += 1;
                Some(SortedMergeEntry::Left(left))
            }
            (None, Some(right)) => {
                self.right_index += 1;
                Some(SortedMergeEntry::Right(right))
            }
            (None, None) => None,
        }
    }
}
