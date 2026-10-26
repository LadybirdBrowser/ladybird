/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The records of a sorted list of records whose keys a key range selects

use crate::indexed_db::idb_key_range::IDBKeyRange;
use crate::indexed_db::internal::key::Key;

/// A record of an object store's or an index's list of records, which is sorted on its keys.
pub(crate) trait KeyedRecord {
    fn key(&self) -> &Key;
}

pub(crate) struct RecordRange {
    pub(crate) start: usize,
    pub(crate) end: usize,
}

fn first_record_index_with_key_at_or_after<R: KeyedRecord>(records: &[R], key: &Key, open: bool) -> usize {
    let mut low = 0;
    let mut high = records.len();

    while low < high {
        let middle = low + (high - low) / 2;
        let comparison = Key::compare_two_keys(records[middle].key(), key);
        if comparison < 0 || (comparison == 0 && open) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    low
}

fn first_record_index_with_key_after<R: KeyedRecord>(records: &[R], key: &Key, open: bool, start: usize) -> usize {
    let mut low = start;
    let mut high = records.len();

    while low < high {
        let middle = low + (high - low) / 2;
        let comparison = Key::compare_two_keys(records[middle].key(), key);
        if comparison < 0 || (comparison == 0 && !open) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    low
}

pub(crate) fn record_range_for_key_range<R: KeyedRecord>(records: &[R], range: &IDBKeyRange) -> RecordRange {
    let mut start = 0;
    let mut end = records.len();

    if let Some(lower) = range.lower_key() {
        start = first_record_index_with_key_at_or_after(records, lower, range.is_lower_open());
    }

    if let Some(upper) = range.upper_key() {
        end = first_record_index_with_key_after(records, upper, range.is_upper_open(), start);
    }

    RecordRange { start, end }
}
