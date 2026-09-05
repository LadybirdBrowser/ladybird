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
    SortedMerge { left, right, compare }
}

impl<'a, Left, Right, Compare> Iterator for SortedMerge<'a, Left, Right, Compare>
where
    Compare: Fn(&Left, &Right) -> Ordering,
{
    type Item = SortedMergeEntry<'a, Left, Right>;

    fn next(&mut self) -> Option<Self::Item> {
        match (self.left.split_first(), self.right.split_first()) {
            (Some((left, left_rest)), Some((right, right_rest))) => match (self.compare)(left, right) {
                Ordering::Equal => {
                    self.left = left_rest;
                    self.right = right_rest;
                    Some(SortedMergeEntry::Both(left, right))
                }
                Ordering::Less => {
                    self.left = left_rest;
                    Some(SortedMergeEntry::Left(left))
                }
                Ordering::Greater => {
                    self.right = right_rest;
                    Some(SortedMergeEntry::Right(right))
                }
            },
            (Some((left, left_rest)), None) => {
                self.left = left_rest;
                Some(SortedMergeEntry::Left(left))
            }
            (None, Some((right, right_rest))) => {
                self.right = right_rest;
                Some(SortedMergeEntry::Right(right))
            }
            (None, None) => None,
        }
    }
}
