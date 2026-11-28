/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Cursors as the backend iterates them, https://w3c.github.io/IndexedDB/#cursor-construct

use std::rc::Rc;

use libipc_rust::wire::{Decode, Encode};

use crate::indexed_db::Name;

use crate::indexed_db::backend::store::ObjectStore;
use crate::indexed_db::backend::{Error, ErrorKind};
use crate::indexed_db::key::Key;
use crate::indexed_db::key_range::{KeyRange, OrderedKey};

/// The object store or the index that a request or a cursor reads records from.
#[derive(Clone, Debug, PartialEq, Encode, Decode)]
pub enum Source {
    ObjectStore(Name),
    Index { store: Name, index: Name },
}

impl Source {
    pub fn store_name(&self) -> &Name {
        match self {
            Source::ObjectStore(store) | Source::Index { store, .. } => store,
        }
    }
}

// https://w3c.github.io/IndexedDB/#enumdef-idbcursordirection
#[derive(Clone, Copy, Debug, PartialEq, Eq, Encode, Decode)]
#[repr(u8)]
pub enum Direction {
    Next,
    NextUnique,
    Prev,
    PrevUnique,
}

impl Direction {
    fn is_reverse(self) -> bool {
        matches!(self, Direction::Prev | Direction::PrevUnique)
    }
}

/// A record that a cursor or a request found: the record's key (the index key, for an index), the key of the object
/// store record, and the object store record's value unless only keys were asked for.
#[derive(Clone, Encode, Decode)]
pub struct FoundRecord {
    pub key: Key,
    pub primary_key: Key,
    pub value: Option<Vec<u8>>,
}

/// The records of a source from a starting key on, in one direction: each record's key, and the key of the object
/// store record it is or references.
pub(crate) fn records_from<'a>(
    store: &'a ObjectStore,
    source: &Source,
    range: &KeyRange,
    reverse: bool,
) -> Result<Box<dyn Iterator<Item = (&'a OrderedKey, &'a OrderedKey)> + 'a>, Error> {
    index_records_from(store, source, range, reverse, None)
}

/// The records of a source as records_from() has them, leaving out, within the index key of `skip`, the records before
/// its object store key (after it, in reverse).
fn index_records_from<'a>(
    store: &'a ObjectStore,
    source: &Source,
    range: &KeyRange,
    reverse: bool,
    skip: Option<(OrderedKey, OrderedKey)>,
) -> Result<Box<dyn Iterator<Item = (&'a OrderedKey, &'a OrderedKey)> + 'a>, Error> {
    match source {
        Source::ObjectStore(_) => {
            let records = store.records_in_range(range).map(|(key, _)| (key, key));
            Ok(if reverse {
                Box::new(records.rev())
            } else {
                Box::new(records)
            })
        }
        Source::Index { index, .. } => {
            let index = store
                .index(index)
                .ok_or_else(|| Error::new(ErrorKind::NotFound, "No index with this name"))?;
            Ok(index.records_in_range_from(range, reverse, skip))
        }
    }
}

// https://w3c.github.io/IndexedDB/#cursor-construct
pub struct Cursor {
    pub source: Source,
    pub range: KeyRange,
    pub direction: Direction,
    pub key_only: bool,

    // A cursor has a position within its range.
    position: Option<Rc<Key>>,

    // When iterating indexes the cursor also has an object store position, which indicates the position of the
    // referenced record in the index's object store.
    object_store_position: Option<Rc<Key>>,
}

/// The tighter of two lower bounds, or of two upper bounds when `upper` is true; the bounds are (key, open).
fn tighter(a: Option<(Rc<Key>, bool)>, b: Option<(Rc<Key>, bool)>, upper: bool) -> Option<(Rc<Key>, bool)> {
    match (a, b) {
        (None, bound) | (bound, None) => bound,
        (Some(a), Some(b)) => {
            let comparison = Key::compare_two_keys(&a.0, &b.0);
            let a_is_tighter = if upper { comparison < 0 } else { comparison > 0 };
            if comparison == 0 {
                Some((a.0, a.1 || b.1))
            } else if a_is_tighter {
                Some(a)
            } else {
                Some(b)
            }
        }
    }
}

impl Cursor {
    pub fn new(source: Source, range: KeyRange, direction: Direction, key_only: bool) -> Self {
        Self {
            source,
            range,
            direction,
            key_only,
            position: None,
            object_store_position: None,
        }
    }

    // https://w3c.github.io/IndexedDB/#iterate-a-cursor
    pub fn iterate(
        &mut self,
        store: &ObjectStore,
        key: Option<Key>,
        primary_key: Option<Key>,
        count: u32,
    ) -> Result<Option<FoundRecord>, Error> {
        let key = key.map(Rc::new);
        let primary_key = primary_key.map(Rc::new);
        let source_is_index = matches!(self.source, Source::Index { .. });
        let reverse = self.direction.is_reverse();

        // 3. Assert: if primaryKey is given, source is an index and direction is "next" or "prev".
        // NB: The agent checked that, and that a key comes with the primary key; one that sent otherwise misbehaved.
        if primary_key.is_some()
            && (key.is_none() || !source_is_index || !matches!(self.direction, Direction::Next | Direction::Prev))
        {
            return Err(Error::new(
                ErrorKind::InvalidState,
                "A primary key is only for a key of an index cursor",
            ));
        }

        // NB: A cursor of an index that found no record kept its position but not its object store position. The
        //     agent doesn't iterate it again; one that does misbehaved.
        if source_is_index
            && matches!(self.direction, Direction::Next | Direction::Prev)
            && self.position.is_some()
            && self.object_store_position.is_none()
        {
            return Err(Error::new(
                ErrorKind::InvalidState,
                "The cursor has gone past its last record",
            ));
        }

        // 6. Let position be cursor's position.
        let mut position = self.position.clone();

        // 7. Let object store position be cursor's object store position.
        let mut object_store_position = self.object_store_position.clone();

        // 8. If count is not given, let count be 1.
        let mut count = count.max(1);

        let satisfies = |record_key: &Key,
                         referenced: &Key,
                         position: &Option<Rc<Key>>,
                         object_store_position: &Option<Rc<Key>>|
         -> bool {
            let toward = |a: &Key, b: &Key| {
                let comparison = Key::compare_two_keys(a, b);
                if reverse { -comparison } else { comparison }
            };

            // * If key is defined: The record's key is greater than or equal to key (less than or equal, for prev).
            if let Some(key) = &key
                && toward(record_key, key) < 0
            {
                return false;
            }

            // * If primaryKey is defined: The record's key is equal to key and the record's value is greater than or
            //   equal to primaryKey (less than or equal, for prev), or the record's key is greater than key (less
            //   than, for prev).
            if let Some(primary_key) = &primary_key {
                let key = key.as_ref().expect("primaryKey is given with key");
                let key_comparison = toward(record_key, key);
                if key_comparison < 0 || (key_comparison == 0 && toward(referenced, primary_key) < 0) {
                    return false;
                }
            }

            if let Some(position) = position {
                let position_comparison = toward(record_key, position);
                let unique = matches!(self.direction, Direction::NextUnique | Direction::PrevUnique);
                if !source_is_index || unique {
                    // * If position is defined and source is an object store (or the direction is unique): The
                    //   record's key is greater than position (less than, for prev).
                    if position_comparison <= 0 {
                        return false;
                    }
                } else {
                    // * If position is defined and source is an index: The record's key is equal to position and the
                    //   record's value is greater than object store position (less than, for prev), or the record's
                    //   key is greater than position (less than, for prev).
                    let object_store_position = object_store_position
                        .as_ref()
                        .expect("a cursor of an index with a position has an object store position");
                    if position_comparison < 0
                        || (position_comparison == 0 && toward(referenced, object_store_position) <= 0)
                    {
                        return false;
                    }
                }
            }

            // * The record's key is in range.
            self.range.contains(record_key)
        };

        // 9. While count is greater than 0:
        let mut found: Option<(Rc<Key>, Rc<Key>)> = None;
        while count > 0 {
            // NB: Every record before the furthest of the range's bound, key and position fails the requirements, so
            //     the search starts there.
            let start = [key.clone(), position.clone()]
                .into_iter()
                .flatten()
                .map(|key| (key, false))
                .fold(
                    if reverse {
                        self.range
                            .upper
                            .clone()
                            .map(|key| (Rc::new(key), self.range.upper_open))
                    } else {
                        self.range
                            .lower
                            .clone()
                            .map(|key| (Rc::new(key), self.range.lower_open))
                    },
                    |bound, key| tighter(bound, Some(key), reverse),
                );
            let search_range = if reverse {
                KeyRange {
                    lower: self.range.lower.clone(),
                    lower_open: self.range.lower_open,
                    upper: start.as_ref().map(|(key, _)| key.as_ref().clone()),
                    upper_open: start.as_ref().is_some_and(|(_, open)| *open),
                }
            } else {
                KeyRange {
                    lower: start.as_ref().map(|(key, _)| key.as_ref().clone()),
                    lower_open: start.as_ref().is_some_and(|(_, open)| *open),
                    upper: self.range.upper.clone(),
                    upper_open: self.range.upper_open,
                }
            };

            // NB: Within the index key that the search starts at, the records before the cursor's object store position
            //     (or before primaryKey, for the key it comes with) fail the requirements too, so the search skips them.
            let unique = matches!(self.direction, Direction::NextUnique | Direction::PrevUnique);
            let start_key = start.as_ref().map(|(key, _)| key.clone());
            let in_start_group = |group: &Option<Rc<Key>>| matches!((group, &start_key), (Some(group), Some(start)) if Key::compare_two_keys(group, start) == 0);
            let mut skip: Option<Rc<Key>> = None;
            if source_is_index && !unique && in_start_group(&position) {
                skip = object_store_position.clone();
            }
            if let Some(primary_key) = &primary_key
                && in_start_group(&key)
            {
                skip = match skip {
                    Some(from) if Key::compare_two_keys(&from, primary_key) * if reverse { -1 } else { 1 } > 0 => {
                        Some(from)
                    }
                    _ => Some(primary_key.clone()),
                };
            }
            let skip = skip
                .zip(start_key.clone())
                .map(|(from, group)| (OrderedKey(group), OrderedKey(from)));

            // 1. Switch on direction: next and nextunique find the first record that satisfies the requirements, prev
            //    the last one.
            let candidate = index_records_from(store, &self.source, &search_range, reverse, skip)?
                .find(|(record_key, referenced)| {
                    satisfies(record_key.key(), referenced.key(), &position, &object_store_position)
                })
                .map(|(record_key, referenced)| (record_key.key().clone(), referenced.key().clone()));

            // prevunique: Let temp record be the last record in records which satisfy all of the requirements. If temp
            // record is defined, let found record be the first record in records whose key is equal to temp record's
            // key.
            found = match (self.direction, candidate) {
                (Direction::PrevUnique, Some((temp_key, _))) => {
                    records_from(store, &self.source, &KeyRange::only(temp_key.as_ref().clone()), false)?
                        .next()
                        .map(|(record_key, referenced)| (record_key.key().clone(), referenced.key().clone()))
                }
                (_, candidate) => candidate,
            };

            // 2. If found record is not defined, then:
            let Some((found_key, found_referenced)) = &found else {
                // 1. Set cursor's key to undefined.
                // 2. If source is an index, set cursor's object store position to undefined.
                // NB: The agent's cursor keeps its key, and the backend's needs no position from here on.
                self.object_store_position = None;

                // 4. Return null.
                return Ok(None);
            };

            // 3. Let position be found record's key.
            position = Some(found_key.clone());

            // 4. If source is an index, let object store position be found record's value.
            if source_is_index {
                object_store_position = Some(found_referenced.clone());
            }

            // 5. Decrease count by 1.
            count -= 1;
        }
        let (found_key, found_referenced) = found.expect("a record was found");

        // 10. Set cursor's position to position.
        self.position = position;

        // 11. If source is an index, set cursor's object store position to object store position.
        if source_is_index {
            self.object_store_position = object_store_position;
        }

        // 13. If cursor's key only flag is false, then let serialized be found record's value if source is an object
        //     store, or found record's referenced value otherwise.
        let value = if self.key_only {
            None
        } else {
            store
                .records
                .get(&OrderedKey(found_referenced.clone()))
                .map(|value| value.as_ref().clone())
        };

        Ok(Some(FoundRecord {
            key: found_key.as_ref().clone(),
            primary_key: found_referenced.as_ref().clone(),
            value,
        }))
    }
}
