/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! This process's copies of the maps of storage bottles, https://storage.spec.whatwg.org/#storage-bottle

use core::ffi::c_void;
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::rc::{Rc, Weak};

use ak::{Utf16String, Utf16StringUnits};

use crate::storage_api::storage_key::StorageKey;

/// The value cannot be stored, as the bottle's quota does not leave room for it.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct QuotaExceeded;

/// https://storage.spec.whatwg.org/#storage-identifier
///
/// The storage identifiers of the registered endpoints, as StorageAPI::StorageEndpointType numbers them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum StorageEndpointType {
    Caches = 0,
    IndexedDB = 1,
    LocalStorage = 2,
    ServiceWorkerRegistrations = 3,
    SessionStorage = 4,
}

impl StorageEndpointType {
    pub(crate) const COUNT: usize = 5;
}

/// Bytes a string contributes toward a storage bottle's quota: its UTF-8 length, with a lone surrogate counted as the
/// three bytes it is written as. The process that keeps a bottle and the processes that mirror it measure the same
/// way, so a write one accepts is a write the other accepts.
pub(crate) fn storage_quota_size(string: &Utf16String) -> u64 {
    quota_size_of_units(string.as_units())
}

// NB: Counted without encoding, as every write measures its key and value, and building the UTF-8 string would cost a
//     copy of each on the path meant to be fast.
fn quota_size_of_units(units: Utf16StringUnits<'_>) -> u64 {
    match units {
        Utf16StringUnits::Ascii(units) => units.len() as u64,
        Utf16StringUnits::Latin1(units) => units.iter().map(|&unit| if unit < 0x80 { 1 } else { 2 }).sum(),
        Utf16StringUnits::Utf16(units) => {
            let mut size = 0;
            let mut index = 0;
            while index < units.len() {
                let unit = units[index];
                let is_pair = (0xd800..0xdc00).contains(&unit)
                    && units.get(index + 1).is_some_and(|next| (0xdc00..0xe000).contains(next));
                if is_pair {
                    size += 4;
                    index += 2;
                    continue;
                }
                size += match unit {
                    0..0x80 => 1,
                    0x80..0x800 => 2,
                    _ => 3,
                };
                index += 1;
            }
            size
        }
    }
}

/// What the process that owns the storage areas answers when this process connects to one: the area's id and
/// everything the bottle holds. No id means the area couldn't be resolved, and the mirror stays detached.
#[derive(Default)]
pub(crate) struct StorageAreaConnection {
    pub(crate) id: Option<u64>,
    pub(crate) keys: Vec<Utf16String>,
    pub(crate) values: Vec<Utf16String>,
    pub(crate) usage: u64,
}

/// The page and environment a bottle asks on behalf of, which the owner resolves to the area. A mirror outlives the
/// document that first asked for it, so every call names its own requester.
pub(crate) struct Requester<'a> {
    pub(crate) page_id: u64,
    pub(crate) environment_id: &'a str,
    /// The environment settings object, which only the client reads, to name the environment to the owner.
    pub(crate) environment: *mut c_void,
}

/// The process's one link to the storage areas' owner. WebContent connects it to the browser process; a process
/// without one leaves every mirror detached.
pub(crate) trait StorageAreaMirrorClient {
    fn connect_to_storage_area(
        &self,
        requester: &Requester<'_>,
        endpoint_type: StorageEndpointType,
    ) -> StorageAreaConnection;
    fn disconnect_from_storage_area(&self, id: u64);
    fn set_storage_item(&self, id: u64, key: &Utf16String, value: &Utf16String, document_url: &str);
    fn remove_storage_item(&self, id: u64, key: &Utf16String, document_url: &str);
    fn clear_storage_area(&self, id: u64, document_url: &str);
}

// The mirrors this process holds, by the key a bottle finds them with and by the area id the owner's messages name.
// Weak, so a mirror lives exactly as long as some bottle addresses it; each mirror removes itself when it dies.
#[derive(Default)]
struct MirrorRegistry {
    by_key: HashMap<String, Weak<StorageAreaMirror>>,
    by_id: HashMap<u64, Weak<StorageAreaMirror>>,
}

thread_local! {
    static CLIENT: RefCell<Option<Rc<dyn StorageAreaMirrorClient>>> = const { RefCell::new(None) };
    static REGISTRY: RefCell<MirrorRegistry> = RefCell::new(MirrorRegistry::default());
}

pub(crate) fn set_storage_area_mirror_client(client: Option<Rc<dyn StorageAreaMirrorClient>>) {
    CLIENT.set(client);
}

fn client() -> Option<Rc<dyn StorageAreaMirrorClient>> {
    // NB: A mirror that dies as the thread exits finds no client to disconnect through.
    CLIENT.try_with(|client| client.borrow().clone()).ok().flatten()
}

fn registry_key_for(endpoint_type: StorageEndpointType, session_page: Option<u64>, storage_key: &StorageKey) -> String {
    let endpoint = endpoint_type as u8;
    let storage_key = storage_key.origin.serialize();
    match session_page {
        Some(page) => format!("{endpoint}/{page}/{storage_key}"),
        None => format!("{endpoint}//{storage_key}"),
    }
}

fn requester_key(requester: &Requester<'_>) -> String {
    format!("{}/{}", requester.page_id, requester.environment_id)
}

// An entry's quota size is kept beside its value, so overwriting or removing it never measures the old value again.
struct Entry {
    value: Utf16String,
    quota_size: u64,
}

// The entries in the order their keys were first set, as AK's OrderedHashMap keeps them: setting a key that exists
// keeps its place, and removing one keeps the others' order.
#[derive(Default)]
struct OrderedEntries {
    slots: Vec<Option<(Utf16String, Entry)>>,
    positions: HashMap<Utf16String, usize>,
}

impl OrderedEntries {
    fn len(&self) -> usize {
        self.positions.len()
    }

    fn is_empty(&self) -> bool {
        self.positions.is_empty()
    }

    fn contains_key(&self, key: &Utf16String) -> bool {
        self.positions.contains_key(key)
    }

    fn get(&self, key: &Utf16String) -> Option<&Entry> {
        let position = *self.positions.get(key)?;
        self.slots[position].as_ref().map(|(_, entry)| entry)
    }

    fn set(&mut self, key: Utf16String, entry: Entry) {
        if let Some(&position) = self.positions.get(&key) {
            self.slots[position] = Some((key, entry));
            return;
        }
        self.positions.insert(key.clone(), self.slots.len());
        self.slots.push(Some((key, entry)));
    }

    fn remove(&mut self, key: &Utf16String) -> Option<Entry> {
        let position = self.positions.remove(key)?;
        let (_, entry) = self.slots[position].take()?;
        // Removed entries leave a hole, which is cheap; once holes outnumber entries, the slots are packed again.
        if self.slots.len() > 2 * self.positions.len() + 16 {
            self.slots.retain(Option::is_some);
            for (position, (key, _)) in self.slots.iter().flatten().enumerate() {
                *self.positions.get_mut(key).expect("every live slot has a position") = position;
            }
        }
        Some(entry)
    }

    fn clear(&mut self) {
        self.slots.clear();
        self.positions.clear();
    }

    fn iter(&self) -> impl Iterator<Item = (&Utf16String, &Entry)> {
        self.slots.iter().flatten().map(|(key, entry)| (key, entry))
    }
}

// The map as this process sees it, and the quota it takes up.
#[derive(Default)]
struct MapCopy {
    entries: OrderedEntries,
    quota_used: u64,
}

impl MapCopy {
    fn apply_set(&mut self, key: Utf16String, value: Utf16String, quota_size: u64) {
        if let Some(existing) = self.entries.get(&key) {
            self.quota_used -= existing.quota_size;
        }
        self.quota_used += quota_size;
        self.entries.set(key, Entry { value, quota_size });
    }

    fn apply_remove(&mut self, key: &Utf16String) {
        if let Some(existing) = self.entries.remove(key) {
            self.quota_used -= existing.quota_size;
        }
    }

    fn apply_clear(&mut self) {
        self.entries.clear();
        self.quota_used = 0;
    }
}

// A write this process applied and sent, until the owner acknowledges or refuses it. A null key is a clear; a null
// value is a removal.
struct PendingWrite {
    key: Option<Utf16String>,
    new_value: Option<Utf16String>,
}

#[derive(Default)]
struct MirrorState {
    map: MapCopy,
    id: Option<u64>,
    primed: bool,
    failed_requesters: HashSet<String>,

    // The writes the owner hasn't answered yet are the ones from first_pending_write on. Answered ones stay in place
    // until the queue drains, so a script that queues thousands of writes in one task costs nothing more per answer.
    pending_writes: Vec<PendingWrite>,
    first_pending_write: usize,
    pending_count_by_key: HashMap<Utf16String, u32>,
    pending_clear_count: u32,
}

impl MirrorState {
    fn push_pending_write(&mut self, key: Option<Utf16String>, new_value: Option<Utf16String>) {
        match &key {
            Some(key) => *self.pending_count_by_key.entry(key.clone()).or_insert(0) += 1,
            None => self.pending_clear_count += 1,
        }
        self.pending_writes.push(PendingWrite { key, new_value });
    }

    fn pop_pending_write(&mut self) -> PendingWrite {
        // The owner answers every write, in the order they were sent.
        assert!(
            self.first_pending_write < self.pending_writes.len(),
            "the owner answers only writes this process sent"
        );
        let write = PendingWrite {
            key: self.pending_writes[self.first_pending_write].key.take(),
            new_value: self.pending_writes[self.first_pending_write].new_value.take(),
        };
        self.first_pending_write += 1;
        if self.first_pending_write == self.pending_writes.len() {
            self.pending_writes.clear();
            self.first_pending_write = 0;
        }
        match &write.key {
            Some(key) => {
                let count = self
                    .pending_count_by_key
                    .get_mut(key)
                    .expect("a pending write of a key is counted");
                *count -= 1;
                if *count == 0 {
                    self.pending_count_by_key.remove(key);
                }
            }
            None => {
                assert!(self.pending_clear_count > 0, "a pending clear is counted");
                self.pending_clear_count -= 1;
            }
        }
        write
    }

    // The owner hasn't applied these yet, so whatever state it just described predates them.
    fn reapply_pending_writes(&mut self) {
        let Self {
            map,
            pending_writes,
            first_pending_write,
            ..
        } = self;
        for write in &pending_writes[*first_pending_write..] {
            match (&write.key, &write.new_value) {
                (None, _) => map.apply_clear(),
                (Some(key), Some(value)) => map.apply_set(
                    key.clone(),
                    value.clone(),
                    storage_quota_size(key) + storage_quota_size(value),
                ),
                (Some(key), None) => map.apply_remove(key),
            }
        }
    }

    // A change another process made, as the owner serialized it. One that reached the owner before a write this
    // process still has pending predates that write, so the mirror already shows the later state and leaves it be.
    // While a clear is pending, that goes for every change.
    fn apply_remote_change(&mut self, key: Option<&Utf16String>, new_value: Option<&Utf16String>) {
        if self.pending_clear_count > 0 {
            return;
        }

        let Some(key) = key else {
            // The owner emptied the bottle before applying this process's pending writes, so those come back on top.
            self.map.apply_clear();
            self.reapply_pending_writes();
            return;
        };

        if self.pending_count_by_key.contains_key(key) {
            return;
        }

        match new_value {
            Some(value) => self.map.apply_set(
                key.clone(),
                value.clone(),
                storage_quota_size(key) + storage_quota_size(value),
            ),
            None => self.map.apply_remove(key),
        }
    }
}

/// A change another party made to a mirrored map, as the owner relayed it (or, after a reset, as the diff shows it),
/// for the storage events it calls for. A null key is a clear.
pub(crate) struct RemoteChange {
    pub(crate) key: Option<Utf16String>,
    pub(crate) old_value: Option<Utf16String>,
    pub(crate) new_value: Option<Utf16String>,
    pub(crate) url: String,
}

/// The changes to report for one mirror.
pub(crate) struct RemoteChanges {
    pub(crate) mirror: Rc<StorageAreaMirror>,
    pub(crate) changes: Vec<RemoteChange>,
}

/// https://storage.spec.whatwg.org/#storage-bottle
///
/// This process's copy of one storage bottle's map. The map itself is kept by the browser process, which makes it
/// available across agent cluster boundaries this way: a process loads the map once, with one synchronous round trip
/// that also registers it for every later change, and from then on answers reads from the copy, applies writes to the
/// copy before sending them, and applies the changes other processes make as the owner relays them. Every storage
/// bottle in this process for the same area shares one mirror, so two windows of one origin can't see different maps.
pub(crate) struct StorageAreaMirror {
    endpoint_type: StorageEndpointType,
    session_page: Option<u64>,
    storage_key: StorageKey,
    quota: Option<u64>,
    registry_key: String,
    state: RefCell<MirrorState>,
}

impl Drop for StorageAreaMirror {
    fn drop(&mut self) {
        let id = self.state.get_mut().id;
        // NB: A mirror that dies as the thread exits finds the registry gone, and nothing left to remove itself from.
        let _ = REGISTRY.try_with(|registry| {
            let mut registry = registry.borrow_mut();
            registry.by_key.remove(&self.registry_key);
            if let Some(id) = id {
                registry.by_id.remove(&id);
            }
        });
        if let (Some(id), Some(client)) = (id, client()) {
            client.disconnect_from_storage_area(id);
        }
    }
}

fn mirror_for(id: u64) -> Option<Rc<StorageAreaMirror>> {
    REGISTRY.with_borrow(|registry| registry.by_id.get(&id).and_then(Weak::upgrade))
}

impl StorageAreaMirror {
    /// One mirror per storage key for local storage, and per page and storage key for session storage: a page's
    /// documents all belong to one traversable, and the traversable is what partitions session storage.
    pub(crate) fn obtain(
        endpoint_type: StorageEndpointType,
        session_page: Option<u64>,
        storage_key: &StorageKey,
        quota: Option<u64>,
    ) -> Rc<Self> {
        let key = registry_key_for(endpoint_type, session_page, storage_key);
        if let Some(live) = REGISTRY.with_borrow(|registry| registry.by_key.get(&key).and_then(Weak::upgrade)) {
            return live;
        }

        let mirror = Rc::new(Self {
            endpoint_type,
            session_page,
            storage_key: storage_key.clone(),
            quota,
            registry_key: key.clone(),
            state: RefCell::new(MirrorState::default()),
        });
        REGISTRY.with_borrow_mut(|registry| registry.by_key.insert(key, Rc::downgrade(&mirror)));
        mirror
    }

    pub(crate) fn endpoint_type(&self) -> StorageEndpointType {
        self.endpoint_type
    }

    pub(crate) fn storage_key(&self) -> &StorageKey {
        &self.storage_key
    }

    pub(crate) fn session_page(&self) -> Option<u64> {
        self.session_page
    }

    /// Loads the map now rather than on first access, so changes other processes make start arriving.
    pub(crate) fn connect(self: &Rc<Self>, requester: &Requester<'_>) {
        self.ensure_primed(requester);
    }

    // False when the owner couldn't resolve the requester's area; that requester is then not asked about again, but
    // another requester of the same mirror still can be (the first may have been a page on its way out).
    fn ensure_primed(self: &Rc<Self>, requester: &Requester<'_>) -> bool {
        if self.state.borrow().primed {
            return true;
        }
        let Some(client) = client() else {
            return false;
        };

        let key = requester_key(requester);
        if self.state.borrow().failed_requesters.contains(&key) {
            return false;
        }

        // One round trip loads the map and registers this process for every change after it, so nothing is missed or
        // seen twice.
        let connection = client.connect_to_storage_area(requester, self.endpoint_type);
        let mut state = self.state.borrow_mut();
        let Some(id) = connection.id else {
            state.failed_requesters.insert(key);
            return false;
        };

        // The owner sends one value per key. Taking the shorter of the two would prime a map that's quietly missing
        // entries, and nothing downstream could tell that from an empty bottle.
        assert_eq!(
            connection.keys.len(),
            connection.values.len(),
            "the owner sends one value per key"
        );

        state.id = Some(id);
        REGISTRY.with_borrow_mut(|registry| registry.by_id.insert(id, Rc::downgrade(self)));
        for (key, value) in connection.keys.into_iter().zip(connection.values) {
            let quota_size = storage_quota_size(&key) + storage_quota_size(&value);
            state.map.entries.set(key, Entry { value, quota_size });
        }
        state.map.quota_used = connection.usage;
        state.primed = true;
        state.failed_requesters.clear();
        true
    }

    pub(crate) fn size(self: &Rc<Self>, requester: &Requester<'_>) -> usize {
        if !self.ensure_primed(requester) {
            return 0;
        }
        self.state.borrow().map.entries.len()
    }

    pub(crate) fn keys(self: &Rc<Self>, requester: &Requester<'_>) -> Vec<Utf16String> {
        if !self.ensure_primed(requester) {
            return Vec::new();
        }
        self.state
            .borrow()
            .map
            .entries
            .iter()
            .map(|(key, _)| key.clone())
            .collect()
    }

    pub(crate) fn get(self: &Rc<Self>, requester: &Requester<'_>, key: &Utf16String) -> Option<Utf16String> {
        if !self.ensure_primed(requester) {
            return None;
        }
        self.state
            .borrow()
            .map
            .entries
            .get(key)
            .map(|entry| entry.value.clone())
    }

    /// The value the key had, or the quota's refusal. The write is applied here and sent on without waiting.
    pub(crate) fn set(
        self: &Rc<Self>,
        requester: &Requester<'_>,
        key: &Utf16String,
        value: &Utf16String,
        document_url: &str,
    ) -> Result<Option<Utf16String>, QuotaExceeded> {
        // OPTIMIZATION: An item too large to ever fit is refused before loading the map, so storing one enormous string
        // into a bottle this process hasn't read yet costs no round trip. Quota is measured in UTF-8 bytes, and a UTF-16
        // code unit never encodes to fewer than one of those, so the code unit count is a lower bound.
        if let Some(quota) = self.quota
            && (key.length_in_code_units() + value.length_in_code_units()) as u64 > quota
        {
            return Err(QuotaExceeded);
        }

        if !self.ensure_primed(requester) {
            return Err(QuotaExceeded);
        }

        let (id, old_value) = {
            let mut state = self.state.borrow_mut();
            let (old_value, replaced_size) = state.map.entries.get(key).map_or((None, 0), |existing| {
                (Some(existing.value.clone()), existing.quota_size)
            });

            // The same rule the owner applies, so a write accepted here is one the owner accepts too — unless another
            // process filled the bottle in between, which the owner then refuses and this mirror hears of.
            let new_size = storage_quota_size(key) + storage_quota_size(value);
            if let Some(quota) = self.quota
                && state.map.quota_used - replaced_size + new_size > quota
            {
                return Err(QuotaExceeded);
            }

            state.map.apply_set(key.clone(), value.clone(), new_size);
            state.push_pending_write(Some(key.clone()), Some(value.clone()));
            (state.id.expect("a primed mirror has an id"), old_value)
        };
        if let Some(client) = client() {
            client.set_storage_item(id, key, value, document_url);
        }
        Ok(old_value)
    }

    /// The value removed, if the key existed.
    pub(crate) fn remove(
        self: &Rc<Self>,
        requester: &Requester<'_>,
        key: &Utf16String,
        document_url: &str,
    ) -> Option<Utf16String> {
        if !self.ensure_primed(requester) {
            return None;
        }

        let (id, old_value) = {
            let mut state = self.state.borrow_mut();
            let old_value = state.map.entries.get(key)?.value.clone();
            state.map.apply_remove(key);
            state.push_pending_write(Some(key.clone()), None);
            (state.id.expect("a primed mirror has an id"), old_value)
        };
        if let Some(client) = client() {
            client.remove_storage_item(id, key, document_url);
        }
        Some(old_value)
    }

    /// False when there was nothing to clear.
    pub(crate) fn clear(self: &Rc<Self>, requester: &Requester<'_>, document_url: &str) -> bool {
        if !self.ensure_primed(requester) {
            return false;
        }

        let id = {
            let mut state = self.state.borrow_mut();
            if state.map.entries.is_empty() {
                return false;
            }
            state.map.apply_clear();
            state.push_pending_write(None, None);
            state.id.expect("a primed mirror has an id")
        };
        if let Some(client) = client() {
            client.clear_storage_area(id, document_url);
        }
        true
    }

    fn reset_to(&self, keys: Vec<Utf16String>, values: Vec<Utf16String>) -> Vec<RemoteChange> {
        assert_eq!(keys.len(), values.len(), "the owner sends one value per key");

        let mut state = self.state.borrow_mut();
        let old_entries = core::mem::take(&mut state.map.entries);
        state.map.quota_used = 0;
        for (key, value) in keys.into_iter().zip(values) {
            let quota_size = storage_quota_size(&key) + storage_quota_size(&value);
            state.map.apply_set(key, value, quota_size);
        }
        state.reapply_pending_writes();

        // AD-HOC: The spec's broadcast fires an event for each change a script makes, and never for a write refused after
        //         the fact; it has no such thing. Whatever differs from before is a change this process's windows didn't
        //         see being made, so each gets an event for it — the window whose write was refused included.
        let url = self.storage_key.origin.serialize();
        let mut changes = Vec::new();
        for (key, entry) in old_entries.iter() {
            let now = state.map.entries.get(key).map(|now| now.value.clone());
            if now.as_ref() != Some(&entry.value) {
                changes.push(RemoteChange {
                    key: Some(key.clone()),
                    old_value: Some(entry.value.clone()),
                    new_value: now,
                    url: url.clone(),
                });
            }
        }
        for (key, entry) in state.map.entries.iter() {
            if !old_entries.contains_key(key) {
                changes.push(RemoteChange {
                    key: Some(key.clone()),
                    old_value: None,
                    new_value: Some(entry.value.clone()),
                    url: url.clone(),
                });
            }
        }
        changes
    }

    /// What the owner relays, found by area id. A change for an area this process no longer mirrors is dropped.
    pub(crate) fn did_change(
        id: u64,
        key: Option<Utf16String>,
        old_value: Option<Utf16String>,
        new_value: Option<Utf16String>,
        url: String,
    ) -> Option<RemoteChanges> {
        let mirror = mirror_for(id)?;
        mirror
            .state
            .borrow_mut()
            .apply_remote_change(key.as_ref(), new_value.as_ref());
        Some(RemoteChanges {
            mirror,
            changes: vec![RemoteChange {
                key,
                old_value,
                new_value,
                url,
            }],
        })
    }

    pub(crate) fn did_acknowledge_write(id: u64) {
        if let Some(mirror) = mirror_for(id) {
            mirror.state.borrow_mut().pop_pending_write();
        }
    }

    pub(crate) fn did_refuse_write(id: u64, keys: Vec<Utf16String>, values: Vec<Utf16String>) -> Option<RemoteChanges> {
        let mirror = mirror_for(id)?;

        // Only a set can be refused: it was applied here on the strength of a quota check another process's write
        // overtook. The owner's map puts this one back in agreement.
        let refused = mirror.state.borrow_mut().pop_pending_write();
        assert!(
            refused.key.is_some() && refused.new_value.is_some(),
            "the owner refuses only a set"
        );
        let changes = mirror.reset_to(keys, values);
        Some(RemoteChanges { mirror, changes })
    }

    pub(crate) fn did_reset(id: u64, keys: Vec<Utf16String>, values: Vec<Utf16String>) -> Option<RemoteChanges> {
        let mirror = mirror_for(id)?;
        let changes = mirror.reset_to(keys, values);
        Some(RemoteChanges { mirror, changes })
    }
}

#[cfg(test)]
mod tests {
    use std::cell::{Cell, RefCell};

    use liburl_rust::origin::{Host, Origin};
    use liburl_rust::url::HostKind;

    use super::*;

    // Only short ASCII strings live without AK's C++, which unit tests don't link.
    fn string(string: &str) -> Utf16String {
        assert!(string.is_ascii() && string.len() < size_of::<usize>());
        Utf16String::from_utf8(string)
    }

    fn some(value: &str) -> Option<Utf16String> {
        Some(string(value))
    }

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum Kind {
        Connect,
        Disconnect,
        Set,
        Remove,
        Clear,
    }

    struct Sent {
        kind: Kind,
        id: Option<u64>,
        key: Option<Utf16String>,
        value: Option<Utf16String>,
        url: String,
    }

    #[derive(Default)]
    struct FakeClient {
        sent: RefCell<Vec<Sent>>,
        // What the next connect answers with.
        keys: RefCell<Vec<Utf16String>>,
        values: RefCell<Vec<Utf16String>>,
        usage: Cell<u64>,
        detached: Cell<bool>,
        next_id: Cell<u64>,
    }

    impl FakeClient {
        fn record(
            &self,
            kind: Kind,
            id: Option<u64>,
            key: Option<&Utf16String>,
            value: Option<&Utf16String>,
            url: &str,
        ) {
            self.sent.borrow_mut().push(Sent {
                kind,
                id,
                key: key.cloned(),
                value: value.cloned(),
                url: url.to_owned(),
            });
        }

        fn count(&self, kind: Kind) -> usize {
            self.sent.borrow().iter().filter(|sent| sent.kind == kind).count()
        }

        fn last_id(&self) -> u64 {
            self.sent
                .borrow()
                .last()
                .and_then(|sent| sent.id)
                .expect("a message names an area")
        }
    }

    impl StorageAreaMirrorClient for FakeClient {
        fn connect_to_storage_area(&self, _: &Requester<'_>, _: StorageEndpointType) -> StorageAreaConnection {
            self.record(Kind::Connect, None, None, None, "");
            if self.detached.get() {
                return StorageAreaConnection::default();
            }
            let id = self.next_id.get() + 1;
            self.next_id.set(id);
            StorageAreaConnection {
                id: Some(id),
                keys: self.keys.borrow().clone(),
                values: self.values.borrow().clone(),
                usage: self.usage.get(),
            }
        }

        fn disconnect_from_storage_area(&self, id: u64) {
            self.record(Kind::Disconnect, Some(id), None, None, "");
        }

        fn set_storage_item(&self, id: u64, key: &Utf16String, value: &Utf16String, url: &str) {
            self.record(Kind::Set, Some(id), Some(key), Some(value), url);
        }

        fn remove_storage_item(&self, id: u64, key: &Utf16String, url: &str) {
            self.record(Kind::Remove, Some(id), Some(key), None, url);
        }

        fn clear_storage_area(&self, id: u64, url: &str) {
            self.record(Kind::Clear, Some(id), None, None, url);
        }
    }

    const URL: &str = "https://a.example/page.html";
    const LOCAL_STORAGE_QUOTA: u64 = 5 * 1024 * 1024;

    fn requester(page_id: u64, environment_id: &str) -> Requester<'_> {
        Requester {
            page_id,
            environment_id,
            environment: core::ptr::null_mut(),
        }
    }

    struct Fixture {
        client: Rc<FakeClient>,
    }

    impl Fixture {
        fn new() -> Self {
            let client = Rc::new(FakeClient::default());
            set_storage_area_mirror_client(Some(client.clone()));
            Self { client }
        }

        fn requester(&self) -> Requester<'static> {
            requester(1, "environment")
        }

        fn mirror(&self, host: &str) -> Rc<StorageAreaMirror> {
            self.mirror_with_quota(host, Some(LOCAL_STORAGE_QUOTA))
        }

        fn mirror_with_quota(&self, host: &str, quota: Option<u64>) -> Rc<StorageAreaMirror> {
            StorageAreaMirror::obtain(StorageEndpointType::LocalStorage, None, &key_for(host), quota)
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            set_storage_area_mirror_client(None);
        }
    }

    fn key_for(host: &str) -> StorageKey {
        StorageKey {
            origin: Origin::Tuple {
                scheme: "https".to_owned(),
                host: Host {
                    kind: HostKind::Domain,
                    serialization: host.to_owned(),
                },
                port: None,
                domain: None,
            },
        }
    }

    impl StorageAreaMirror {
        fn quota_used(&self) -> u64 {
            self.state.borrow().map.quota_used
        }

        fn pending_write_count(&self) -> usize {
            let state = self.state.borrow();
            state.pending_writes.len() - state.first_pending_write
        }
    }

    fn quota_of(key: &str, value: &str) -> u64 {
        storage_quota_size(&string(key)) + storage_quota_size(&string(value))
    }

    #[test]
    fn prime_once_per_area_and_share_between_bottles() {
        let f = Fixture::new();
        *f.client.keys.borrow_mut() = vec![string("a")];
        *f.client.values.borrow_mut() = vec![string("1")];
        f.client.usage.set(2);

        let first = f.mirror("shared.example");
        let second = f.mirror("shared.example");
        assert!(Rc::ptr_eq(&first, &second));

        let other_page = StorageAreaMirror::obtain(
            StorageEndpointType::SessionStorage,
            Some(7),
            &key_for("shared.example"),
            None,
        );
        assert!(!Rc::ptr_eq(&other_page, &first));

        assert_eq!(f.client.count(Kind::Connect), 0);
        assert_eq!(first.get(&f.requester(), &string("a")), some("1"));
        assert_eq!(second.size(&f.requester()), 1);
        assert!(second.keys(&f.requester()) == vec![string("a")]);
        assert_eq!(f.client.count(Kind::Connect), 1);
        assert_eq!(first.quota_used(), 2);
    }

    #[test]
    fn local_write_applies_sends_and_tracks_pending_until_acked() {
        let f = Fixture::new();
        let mirror = f.mirror("write.example");

        assert_eq!(mirror.set(&f.requester(), &string("k"), &string("v"), URL), Ok(None));
        assert_eq!(mirror.get(&f.requester(), &string("k")), some("v"));
        assert_eq!(f.client.count(Kind::Set), 1);
        {
            let sent = f.client.sent.borrow();
            let last = sent.last().expect("the write was sent");
            assert_eq!(last.key, some("k"));
            assert_eq!(last.value, some("v"));
            assert_eq!(last.url, URL);
        }
        assert_eq!(mirror.pending_write_count(), 1);

        let id = f.client.last_id();
        StorageAreaMirror::did_acknowledge_write(id);
        assert_eq!(mirror.pending_write_count(), 0);

        // Overwrite returns the old value; removal returns the removed value; clear reports whether there was anything.
        assert_eq!(
            mirror.set(&f.requester(), &string("k"), &string("w"), URL),
            Ok(some("v"))
        );
        assert_eq!(mirror.remove(&f.requester(), &string("k"), URL), some("w"));
        assert_eq!(mirror.remove(&f.requester(), &string("k"), URL), None);
        assert_eq!(f.client.count(Kind::Remove), 1);
        assert!(!mirror.clear(&f.requester(), URL));
        assert_eq!(f.client.count(Kind::Clear), 0);
        let _ = mirror.set(&f.requester(), &string("k"), &string("v"), URL);
        assert!(mirror.clear(&f.requester(), URL));
        assert_eq!(f.client.count(Kind::Clear), 1);
        assert_eq!(mirror.size(&f.requester()), 0);
    }

    #[test]
    fn remote_changes_apply_except_over_pending_local_writes() {
        let f = Fixture::new();
        *f.client.keys.borrow_mut() = vec![string("a"), string("b")];
        *f.client.values.borrow_mut() = vec![string("1"), string("2")];
        f.client.usage.set(4);
        let mirror = f.mirror("remote.example");

        let _ = mirror.set(&f.requester(), &string("a"), &string("mine"), URL);
        let id = f.client.last_id();

        // The owner serialized this change before our pending write, so our mirror already reflects the later state.
        let reported = StorageAreaMirror::did_change(
            id,
            some("a"),
            some("1"),
            some("theirs"),
            "https://remote.example/other.html".to_owned(),
        )
        .expect("the area is mirrored");
        assert_eq!(mirror.get(&f.requester(), &string("a")), some("mine"));
        assert_eq!(reported.changes.len(), 1);
        assert_eq!(reported.changes[0].key, some("a"));
        assert_eq!(reported.changes[0].new_value, some("theirs"));
        assert_eq!(reported.changes[0].url, "https://remote.example/other.html");

        // A change to another key applies.
        let _ = StorageAreaMirror::did_change(id, some("b"), some("2"), some("3"), URL.to_owned());
        assert_eq!(mirror.get(&f.requester(), &string("b")), some("3"));
        let _ = StorageAreaMirror::did_change(id, some("b"), some("3"), None, URL.to_owned());
        assert_eq!(mirror.get(&f.requester(), &string("b")), None);

        StorageAreaMirror::did_acknowledge_write(id);
        let _ = StorageAreaMirror::did_change(id, some("a"), some("mine"), some("later"), URL.to_owned());
        assert_eq!(mirror.get(&f.requester(), &string("a")), some("later"));

        // While a local clear is unacknowledged, every incoming change predates it.
        mirror.clear(&f.requester(), URL);
        let _ = StorageAreaMirror::did_change(id, some("c"), None, some("4"), URL.to_owned());
        let _ = StorageAreaMirror::did_change(id, None, None, None, URL.to_owned());
        assert_eq!(mirror.size(&f.requester()), 0);
        StorageAreaMirror::did_acknowledge_write(id);
        let _ = StorageAreaMirror::did_change(id, some("c"), None, some("4"), URL.to_owned());
        assert_eq!(mirror.get(&f.requester(), &string("c")), some("4"));
        assert_eq!(mirror.quota_used(), 2);
    }

    #[test]
    fn refusal_resets_to_the_snapshot_reapplies_pending_writes_and_reports_the_diff() {
        let f = Fixture::new();
        let mirror = f.mirror("refused.example");

        let _ = mirror.set(&f.requester(), &string("big"), &string("xxxx"), URL);
        let id = f.client.last_id();
        let _ = mirror.set(&f.requester(), &string("b"), &string("2"), URL);
        assert_eq!(mirror.pending_write_count(), 2);

        // The owner refused the first write; the snapshot shows what another process stored in the meantime.
        let reported = StorageAreaMirror::did_refuse_write(id, vec![string("a")], vec![string("1")])
            .expect("the area is mirrored");

        assert_eq!(mirror.get(&f.requester(), &string("big")), None);
        assert_eq!(mirror.get(&f.requester(), &string("a")), some("1"));
        assert_eq!(mirror.get(&f.requester(), &string("b")), some("2"));
        assert_eq!(mirror.pending_write_count(), 1);
        assert_eq!(mirror.quota_used(), 4);

        // Two keys changed from the mirror's point of view: the refused one vanished, and "a" appeared.
        assert_eq!(reported.changes.len(), 2);
        let big = reported
            .changes
            .iter()
            .find(|change| change.key == some("big"))
            .expect("the refused key is reported");
        assert_eq!(big.old_value, some("xxxx"));
        assert_eq!(big.new_value, None);
        let a = reported
            .changes
            .iter()
            .find(|change| change.key == some("a"))
            .expect("the new key is reported");
        assert_eq!(a.old_value, None);
        assert_eq!(a.new_value, some("1"));

        // A reset from the browser takes the same path, with nothing pending to re-apply afterwards.
        StorageAreaMirror::did_acknowledge_write(id);
        let _ = StorageAreaMirror::did_reset(id, Vec::new(), Vec::new());
        assert_eq!(mirror.size(&f.requester()), 0);
        assert_eq!(mirror.quota_used(), 0);
    }

    // A script can queue thousands of writes in one task; their acknowledgments arrive after it yields. Each one has to
    // be cheap, and a refusal in the middle of the stream replays only the writes the owner hasn't answered yet.
    #[test]
    fn a_long_queue_of_pending_writes_drains_in_order_and_a_refusal_replays_only_the_outstanding_ones() {
        let f = Fixture::new();
        let mirror = f.mirror("queue.example");

        const WRITE_COUNT: usize = 5000;
        for i in 0..WRITE_COUNT {
            let _ = mirror.set(&f.requester(), &string(&i.to_string()), &string("v"), URL);
        }
        assert_eq!(mirror.pending_write_count(), WRITE_COUNT);
        let id = f.client.last_id();

        for _ in 0..WRITE_COUNT - 2 {
            StorageAreaMirror::did_acknowledge_write(id);
        }
        assert_eq!(mirror.pending_write_count(), 2);

        // The owner refused the second-to-last write and describes an empty bottle: everything acknowledged is gone from
        // it, and only the last write is still in flight on top.
        let reported = StorageAreaMirror::did_refuse_write(id, Vec::new(), Vec::new()).expect("the area is mirrored");
        assert_eq!(mirror.pending_write_count(), 1);
        assert_eq!(mirror.size(&f.requester()), 1);
        let last = (WRITE_COUNT - 1).to_string();
        assert_eq!(mirror.get(&f.requester(), &string(&last)), some("v"));
        assert_eq!(
            mirror.get(&f.requester(), &string(&(WRITE_COUNT - 2).to_string())),
            None
        );
        assert_eq!(mirror.quota_used(), quota_of(&last, "v"));

        // The diff reports every acknowledged write as gone, and nothing for the one still pending.
        assert_eq!(reported.changes.len(), WRITE_COUNT - 1);

        StorageAreaMirror::did_acknowledge_write(id);
        assert_eq!(mirror.pending_write_count(), 0);
    }

    #[test]
    fn quota_used_matches_the_jar_rule_after_mixed_writes() {
        let f = Fixture::new();
        let mirror = f.mirror_with_quota("quota.example", Some(12));

        let _ = mirror.set(&f.requester(), &string("k1"), &string("ab"), URL);
        let _ = mirror.set(&f.requester(), &string("k2"), &string("value"), URL);
        let _ = mirror.set(&f.requester(), &string("k1"), &string("x"), URL);
        let _ = mirror.remove(&f.requester(), &string("k2"), URL);
        assert_eq!(mirror.quota_used(), quota_of("k1", "x"));

        // A write that would exceed the quota is refused locally and leaves the mirror untouched.
        let result = mirror.set(&f.requester(), &string("k3333"), &string("yyyyyyy"), URL);
        assert_eq!(result, Err(QuotaExceeded));
        assert_eq!(mirror.get(&f.requester(), &string("k3333")), None);
        assert_eq!(mirror.quota_used(), quota_of("k1", "x"));
        assert_eq!(f.client.count(Kind::Set), 3);

        // One too large to ever fit is refused before the mirror is even primed.
        let fresh = Rc::new(FakeClient::default());
        set_storage_area_mirror_client(Some(fresh.clone()));
        let unprimed = f.mirror_with_quota("unprimed.example", Some(12));
        assert_eq!(
            unprimed.set(&f.requester(), &string("kkkkkkk"), &string("zzzzzzz"), URL),
            Err(QuotaExceeded)
        );
        assert!(fresh.sent.borrow().is_empty());
        set_storage_area_mirror_client(Some(f.client.clone()));
    }

    #[test]
    fn quota_size_is_the_length_in_utf8() {
        assert_eq!(quota_size_of_units(Utf16StringUnits::Ascii(b"abc")), 3);
        assert_eq!(quota_size_of_units(Utf16StringUnits::Latin1(&[0x61, 0xe9, 0xe9])), 5);
        assert_eq!(
            quota_size_of_units(Utf16StringUnits::Utf16(&[0x7f, 0x80, 0x7ff, 0x800, 0xffff])),
            11
        );
        // A surrogate pair is one code point of four bytes, and a lone surrogate is written as three.
        assert_eq!(quota_size_of_units(Utf16StringUnits::Utf16(&[0xd83d, 0xde00])), 4);
        assert_eq!(quota_size_of_units(Utf16StringUnits::Utf16(&[0xd83d, 0x61, 0xde00])), 7);
    }

    #[test]
    fn detached_mirror_reads_empty_and_refuses_writes() {
        let f = Fixture::new();
        f.client.detached.set(true);
        let mirror = f.mirror("detached.example");

        assert_eq!(mirror.size(&f.requester()), 0);
        assert_eq!(mirror.get(&f.requester(), &string("k")), None);
        assert_eq!(
            mirror.set(&f.requester(), &string("k"), &string("v"), URL),
            Err(QuotaExceeded)
        );
        assert_eq!(mirror.remove(&f.requester(), &string("k"), URL), None);
        assert!(!mirror.clear(&f.requester(), URL));
        assert_eq!(f.client.count(Kind::Connect), 1);
        assert_eq!(f.client.count(Kind::Set), 0);
    }

    #[test]
    fn a_failed_connect_detaches_only_the_requester_it_failed_for() {
        let f = Fixture::new();
        f.client.detached.set(true);
        let mirror = f.mirror("partly.example");
        let closing_page = requester(9, "closing");

        // A page the browser can't resolve (it's closing) reads empty and can't write; it isn't asked about again.
        assert_eq!(mirror.size(&closing_page), 0);
        assert_eq!(
            mirror.set(&closing_page, &string("k"), &string("v"), URL),
            Err(QuotaExceeded)
        );
        assert_eq!(f.client.count(Kind::Connect), 1);
        assert_eq!(mirror.size(&closing_page), 0);
        assert_eq!(f.client.count(Kind::Connect), 1);

        // Another document of the origin in this process, which the browser does resolve, still gets its storage.
        f.client.detached.set(false);
        *f.client.keys.borrow_mut() = vec![string("a")];
        *f.client.values.borrow_mut() = vec![string("1")];
        f.client.usage.set(2);
        assert_eq!(mirror.get(&f.requester(), &string("a")), some("1"));
        assert_eq!(f.client.count(Kind::Connect), 2);
        assert!(mirror.set(&f.requester(), &string("k"), &string("v"), URL).is_ok());
        // And once the mirror is connected, every requester reads it.
        assert_eq!(mirror.size(&closing_page), 2);
    }

    #[test]
    fn the_last_bottle_disconnects_the_mirror() {
        let f = Fixture::new();
        {
            let mirror = f.mirror("lastbottle.example");
            let _another = f.mirror("lastbottle.example");
            mirror.size(&f.requester());
            assert_eq!(f.client.count(Kind::Disconnect), 0);
        }
        assert_eq!(f.client.count(Kind::Disconnect), 1);

        // A mirror that was never primed has nothing to disconnect from.
        {
            let _never = f.mirror("never.example");
        }
        assert_eq!(f.client.count(Kind::Disconnect), 1);
    }
}
