/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::BTreeSet;
use std::collections::HashMap;
use std::sync::Mutex;
use std::sync::OnceLock;

use super::index::StyleAtomID;

type RawAtomCallback = unsafe extern "C" fn(usize);

const TEXT_KEY_MASK: u64 = 0x0000_ffff_ffff_ffff;
const TEXT_KEY_FLOOR: usize = usize::MAX - TEXT_KEY_MASK as usize;

#[must_use]
pub(super) fn synthetic_text_atom_key(hash: u64) -> usize {
    usize::MAX - (hash & TEXT_KEY_MASK) as usize
}

struct RawAtomCallbacks {
    retain: RawAtomCallback,
    release: RawAtomCallback,
}

fn raw_atom_callbacks() -> &'static OnceLock<RawAtomCallbacks> {
    static CALLBACKS: OnceLock<RawAtomCallbacks> = OnceLock::new();
    &CALLBACKS
}

pub(super) fn install_raw_atom_callbacks(retain: RawAtomCallback, release: RawAtomCallback) {
    raw_atom_callbacks().get_or_init(|| RawAtomCallbacks { retain, release });
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RawAtomLifetime {
    RetainedFlyString,
    SyntheticTextKey,
    OpaqueReplayToken,
}

impl RawAtomLifetime {
    fn for_raw(self, raw: usize) -> Self {
        match self {
            Self::RetainedFlyString if raw >= TEXT_KEY_FLOOR => Self::SyntheticTextKey,
            lifetime => lifetime,
        }
    }
}

#[derive(Clone, Copy)]
struct GlobalAtomEntry {
    atom: StyleAtomID,
    document_references: u64,
    raw_lifetime: Option<RawAtomLifetime>,
}

#[derive(Default)]
struct GlobalAtoms {
    raw: HashMap<usize, GlobalAtomEntry>,
    qualified: HashMap<(u32, u32), GlobalAtomEntry>,
    available: BTreeSet<u32>,
    next: u32,
}

impl GlobalAtoms {
    fn allocate(&mut self) -> StyleAtomID {
        if let Some(atom) = self.available.pop_first() {
            return StyleAtomID(atom);
        }
        self.next = self
            .next
            .checked_add(1)
            .expect("process-global style atom space exhausted");
        StyleAtomID(self.next)
    }

    fn acquire_raw(&mut self, raw: usize, lifetime: RawAtomLifetime) -> StyleAtomID {
        let lifetime = lifetime.for_raw(raw);
        if let Some(entry) = self.raw.get_mut(&raw) {
            assert_eq!(
                entry.raw_lifetime,
                Some(lifetime),
                "one raw atom cannot mix live and replay identity"
            );
            entry.document_references += 1;
            return entry.atom;
        }
        let atom = self.allocate();
        if lifetime == RawAtomLifetime::RetainedFlyString {
            // SAFETY: Live StyleEngine callers pass the raw identity of a referenced Utf16FlyString.
            let callbacks = raw_atom_callbacks()
                .get()
                .expect("live atom callbacks must be installed");
            unsafe { (callbacks.retain)(raw) };
        }
        self.raw.insert(
            raw,
            GlobalAtomEntry {
                atom,
                document_references: 1,
                raw_lifetime: Some(lifetime),
            },
        );
        atom
    }

    fn acquire_qualified(&mut self, namespace: StyleAtomID, name: StyleAtomID) -> StyleAtomID {
        let key = (namespace.0, name.0);
        if let Some(entry) = self.qualified.get_mut(&key) {
            entry.document_references += 1;
            return entry.atom;
        }
        let atom = self.allocate();
        self.qualified.insert(
            key,
            GlobalAtomEntry {
                atom,
                document_references: 1,
                raw_lifetime: None,
            },
        );
        atom
    }

    fn release_raw(&mut self, raw: usize, expected: StyleAtomID) {
        let entry = self
            .raw
            .get_mut(&raw)
            .expect("a document must release a live global atom");
        assert_eq!(entry.atom, expected);
        entry.document_references -= 1;
        if entry.document_references != 0 {
            return;
        }
        let entry = self.raw.remove(&raw).unwrap();
        self.available.insert(entry.atom.0);
        if entry.raw_lifetime == Some(RawAtomLifetime::RetainedFlyString) {
            // SAFETY: acquire_raw retained exactly one global reference for this entry.
            let callbacks = raw_atom_callbacks()
                .get()
                .expect("live atom callbacks must be installed");
            unsafe { (callbacks.release)(raw) };
        }
    }

    fn release_qualified(&mut self, key: (u32, u32), expected: StyleAtomID) {
        let entry = self
            .qualified
            .get_mut(&key)
            .expect("a document must release a live global qualified atom");
        assert_eq!(entry.atom, expected);
        entry.document_references -= 1;
        if entry.document_references != 0 {
            return;
        }
        let entry = self.qualified.remove(&key).unwrap();
        self.available.insert(entry.atom.0);
    }
}

fn global_atoms() -> &'static Mutex<GlobalAtoms> {
    // The mutex supplies the `Sync` required by a process-global static. It does not make a
    // `DocumentAtoms` owner, or the StyleEngine containing it, safe to use from multiple threads.
    static GLOBAL_ATOMS: OnceLock<Mutex<GlobalAtoms>> = OnceLock::new();
    GLOBAL_ATOMS.get_or_init(|| Mutex::new(GlobalAtoms::default()))
}

#[derive(Clone, Copy)]
enum AtomScope {
    #[cfg(test)]
    Document,
    Process(RawAtomLifetime),
}

pub(super) struct DocumentAtoms {
    raw: HashMap<usize, StyleAtomID>,
    qualified: HashMap<(u32, u32), StyleAtomID>,
    scope: AtomScope,
}

impl DocumentAtoms {
    pub(super) fn for_live_engine() -> Self {
        Self {
            raw: HashMap::new(),
            qualified: HashMap::new(),
            #[cfg(test)]
            scope: AtomScope::Document,
            #[cfg(not(test))]
            scope: AtomScope::Process(RawAtomLifetime::RetainedFlyString),
        }
    }

    pub(super) fn for_replay() -> Self {
        Self {
            raw: HashMap::new(),
            qualified: HashMap::new(),
            scope: AtomScope::Process(RawAtomLifetime::OpaqueReplayToken),
        }
    }

    pub(super) fn intern_raw(&mut self, raw: usize) -> StyleAtomID {
        if let Some(&atom) = self.raw.get(&raw) {
            return atom;
        }
        let atom = match self.scope {
            #[cfg(test)]
            AtomScope::Document => self.allocate_document_atom(),
            AtomScope::Process(lifetime) => global_atoms()
                .lock()
                .expect("process-global style atom lock is poisoned")
                .acquire_raw(raw, lifetime),
        };
        self.raw.insert(raw, atom);
        atom
    }

    pub(super) fn intern_qualified(&mut self, namespace: StyleAtomID, name: StyleAtomID) -> StyleAtomID {
        let key = (namespace.0, name.0);
        if let Some(&atom) = self.qualified.get(&key) {
            return atom;
        }
        let atom = match self.scope {
            #[cfg(test)]
            AtomScope::Document => self.allocate_document_atom(),
            AtomScope::Process(_) => global_atoms()
                .lock()
                .expect("process-global style atom lock is poisoned")
                .acquire_qualified(namespace, name),
        };
        self.qualified.insert(key, atom);
        atom
    }

    #[cfg(test)]
    fn allocate_document_atom(&self) -> StyleAtomID {
        let next =
            u32::try_from(self.raw.len() + self.qualified.len()).expect("document style atom space exhausted") + 1;
        StyleAtomID(next)
    }

    #[cfg(feature = "style-recording")]
    pub(super) fn raw(&self) -> &HashMap<usize, StyleAtomID> {
        &self.raw
    }

    #[cfg(feature = "style-recording")]
    pub(super) fn qualified(&self) -> &HashMap<(u32, u32), StyleAtomID> {
        &self.qualified
    }
}

impl Drop for DocumentAtoms {
    fn drop(&mut self) {
        if !matches!(self.scope, AtomScope::Process(_)) {
            return;
        }
        let mut global = global_atoms()
            .lock()
            .expect("process-global style atom lock is poisoned");
        for (&key, &atom) in &self.qualified {
            global.release_qualified(key, atom);
        }
        for (&raw, &atom) in &self.raw {
            global.release_raw(raw, atom);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    static GLOBAL_ATOM_TEST_LOCK: Mutex<()> = Mutex::new(());

    #[test]
    fn synthetic_text_keys_do_not_retain_fly_strings() {
        assert_eq!(
            RawAtomLifetime::RetainedFlyString.for_raw(TEXT_KEY_FLOOR.saturating_sub(1)),
            RawAtomLifetime::RetainedFlyString
        );
        assert_eq!(
            RawAtomLifetime::RetainedFlyString.for_raw(TEXT_KEY_FLOOR),
            RawAtomLifetime::SyntheticTextKey
        );
    }

    #[test]
    fn process_atoms_share_and_recycle_after_the_last_document() {
        let _test_lock = GLOBAL_ATOM_TEST_LOCK.lock().unwrap();
        let first_atom;
        {
            let mut first = DocumentAtoms::for_replay();
            let mut second = DocumentAtoms::for_replay();
            first_atom = first.intern_raw(0x1234);
            assert_eq!(second.intern_raw(0x1234), first_atom);
            assert_ne!(second.intern_raw(0x5678), first_atom);
        }
        let mut later = DocumentAtoms::for_replay();
        assert_eq!(later.intern_raw(0x9abc), first_atom);
    }

    #[test]
    fn qualified_atoms_share_the_global_identity_space() {
        let _test_lock = GLOBAL_ATOM_TEST_LOCK.lock().unwrap();
        let mut first = DocumentAtoms::for_replay();
        let mut second = DocumentAtoms::for_replay();
        let namespace = first.intern_raw(0x1000);
        let name = first.intern_raw(0x2000);
        assert_eq!(second.intern_raw(0x1000), namespace);
        assert_eq!(second.intern_raw(0x2000), name);
        assert_eq!(
            first.intern_qualified(namespace, name),
            second.intern_qualified(namespace, name)
        );
    }
}
