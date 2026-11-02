/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::BTreeSet;
use std::collections::HashMap;
use std::collections::HashSet;
use std::collections::hash_map::Entry;
use std::hash::BuildHasher;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicU8, AtomicU32, AtomicU64, AtomicUsize, Ordering};

use super::index::StyleAtomID;
use crate::css::retained_fly_string::RetainedUtf16FlyString;

const TEXT_KEY_MASK: u64 = 0x0000_ffff_ffff_ffff;
const TEXT_KEY_FLOOR: usize = usize::MAX - TEXT_KEY_MASK as usize;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RawAtomLifetime {
    RetainedFlyString,
    SyntheticTextKey,
    /// A test's stand-in for a fly string, which nothing retains.
    #[cfg(test)]
    OpaqueTestToken,
}

impl RawAtomLifetime {
    fn for_raw(self, raw: usize) -> Self {
        match self {
            Self::RetainedFlyString if raw >= TEXT_KEY_FLOOR => Self::SyntheticTextKey,
            lifetime => lifetime,
        }
    }
}

/// What a number of the table says while a live atom has it. Only the table's lock writes a cell, and a cell a live
/// atom has is written once, but for its lowercase form and its references, so a thread that holds a reference to the
/// atom reads the rest of it without the lock.
#[derive(Default)]
struct AtomCell {
    /// The raw identity of the name, or the namespace and local name of a qualified one.
    key: AtomicU64,
    /// What the key is, or zero where no live atom has the number.
    kind: AtomicU8,
    /// The atom of the name's ASCII-lowercase form, once asked for, which the cell holds a reference to where it is
    /// another atom.
    lowercase: AtomicU32,
    document_references: AtomicU64,
}

const ATOM_KIND_FREE: u8 = 0;
const ATOM_KIND_QUALIFIED: u8 = 1;
const ATOM_KIND_RETAINED_RAW: u8 = 2;
const ATOM_KIND_SYNTHETIC_RAW: u8 = 3;
#[cfg(test)]
const ATOM_KIND_OPAQUE_TEST_RAW: u8 = 4;

impl AtomCell {
    fn key(&self) -> Option<AtomKey> {
        let key = self.key.load(Ordering::Acquire);
        match self.kind.load(Ordering::Acquire) {
            ATOM_KIND_FREE => None,
            ATOM_KIND_QUALIFIED => Some(AtomKey::Qualified(
                StyleAtomID((key >> 32) as u32),
                StyleAtomID(key as u32),
            )),
            _ => Some(AtomKey::Raw(key as usize)),
        }
    }

    fn raw_lifetime(&self) -> Option<RawAtomLifetime> {
        match self.kind.load(Ordering::Acquire) {
            ATOM_KIND_RETAINED_RAW => Some(RawAtomLifetime::RetainedFlyString),
            ATOM_KIND_SYNTHETIC_RAW => Some(RawAtomLifetime::SyntheticTextKey),
            #[cfg(test)]
            ATOM_KIND_OPAQUE_TEST_RAW => Some(RawAtomLifetime::OpaqueTestToken),
            _ => None,
        }
    }
}

fn qualified_key(namespace: StyleAtomID, name: StyleAtomID) -> u64 {
    (u64::from(namespace.0) << 32) | u64::from(name.0)
}

/// How many cells the first chunk of the table's cells holds. Each next chunk holds twice what all before it do.
const FIRST_ATOM_CHUNK_BITS: u32 = 10;

/// The cells of the table, by atom number, which never move: a chunk stays for as long as the table does.
struct AtomCells {
    chunks: [OnceLock<Box<[AtomCell]>>; 23],
}

impl Default for AtomCells {
    fn default() -> Self {
        Self {
            chunks: std::array::from_fn(|_| OnceLock::new()),
        }
    }
}

impl AtomCells {
    fn place_of(atom: StyleAtomID) -> (usize, usize) {
        let place = u64::from(atom.0) + (1 << FIRST_ATOM_CHUNK_BITS);
        let bits = place.ilog2();
        ((bits - FIRST_ATOM_CHUNK_BITS) as usize, (place - (1 << bits)) as usize)
    }

    /// The cell of `atom`, where one was ever made.
    fn get(&self, atom: StyleAtomID) -> Option<&AtomCell> {
        let (chunk, index) = Self::place_of(atom);
        Some(&self.chunks[chunk].get()?[index])
    }

    fn get_or_make(&self, atom: StyleAtomID) -> &AtomCell {
        let (chunk, index) = Self::place_of(atom);
        let cells = self.chunks[chunk].get_or_init(|| {
            (0..1_usize << (chunk as u32 + FIRST_ATOM_CHUNK_BITS))
                .map(|_| AtomCell::default())
                .collect()
        });
        &cells[index]
    }
}

#[derive(Default)]
struct AtomIndexSlot {
    /// The key the slot is for, or zero where it never held one. A slot holding a key stays in the probe sequences
    /// through it until the index is laid out again.
    key: AtomicU64,
    /// The atom of the key, or zero where the slot's key has none any more.
    atom: AtomicU32,
}

/// The atom of each key, which a thread reads without the table's lock and only the lock's holder writes: open
/// addressing over slots that never move. A read without the lock may miss, as the index is laid out again beside it,
/// or find the atom a key just lost: a caller goes on to the lock on a miss, and takes an answer only if the atom it
/// names is one it holds, whose cell names the key.
struct AtomIndex {
    /// The slots, by size: each is twice the one before, and the last made is the one in use.
    generations: [OnceLock<Box<[AtomIndexSlot]>>; 32],
    current: AtomicUsize,
    /// How many slots hold a key, and how many of those keys have an atom. Only the lock's holder reads these.
    used: AtomicUsize,
    live: AtomicUsize,
}

impl Default for AtomIndex {
    fn default() -> Self {
        Self {
            generations: std::array::from_fn(|_| OnceLock::new()),
            current: AtomicUsize::new(usize::MAX),
            used: AtomicUsize::new(0),
            live: AtomicUsize::new(0),
        }
    }
}

const FIRST_ATOM_INDEX_BITS: u32 = 10;

impl AtomIndex {
    fn slots(&self) -> Option<&[AtomIndexSlot]> {
        let current = self.current.load(Ordering::Acquire);
        (current != usize::MAX).then(|| &**self.generations[current].get().expect("the current slots are made"))
    }

    fn home(key: u64, slots: usize) -> usize {
        debug_assert!(slots.is_power_of_two());
        (key.wrapping_mul(0x9e37_79b9_7f4a_7c15) >> (u64::BITS - slots.ilog2())) as usize
    }

    fn get(&self, key: u64) -> Option<StyleAtomID> {
        let slots = self.slots()?;
        let mask = slots.len() - 1;
        let mut place = Self::home(key, slots.len());
        // NB: Fewer than all slots hold a key, whichever slots a reader sees written.
        for _ in 0..slots.len() {
            let slot = &slots[place];
            match slot.key.load(Ordering::Acquire) {
                0 => return None,
                found if found == key => {
                    let atom = slot.atom.load(Ordering::Acquire);
                    return (atom != 0).then_some(StyleAtomID(atom));
                }
                _ => place = (place + 1) & mask,
            }
        }
        None
    }

    /// Note the atom of `key`, which has none. Only the lock's holder calls this.
    fn insert(&self, key: u64, atom: StyleAtomID) {
        assert_ne!(key, 0, "no atom has the empty key");
        let used = self.used.load(Ordering::Relaxed);
        let live = self.live.load(Ordering::Relaxed);
        let capacity = self.slots().map_or(0, <[AtomIndexSlot]>::len);
        if (used + 1) * 4 > capacity * 3 {
            self.lay_out(live + 1);
        }
        let slots = self.slots().expect("the index was laid out");
        let mask = slots.len() - 1;
        let mut place = Self::home(key, slots.len());
        let mut reusable = None;
        loop {
            let slot = &slots[place];
            match slot.key.load(Ordering::Relaxed) {
                0 => break,
                found if found == key => {
                    reusable = Some(place);
                    break;
                }
                _ if reusable.is_none() && slot.atom.load(Ordering::Relaxed) == 0 => reusable = Some(place),
                _ => {}
            }
            place = (place + 1) & mask;
        }
        let slot = match reusable {
            Some(reusable) => &slots[reusable],
            None => {
                self.used.fetch_add(1, Ordering::Relaxed);
                &slots[place]
            }
        };
        slot.key.store(key, Ordering::Release);
        slot.atom.store(atom.0, Ordering::Release);
        self.live.fetch_add(1, Ordering::Relaxed);
    }

    /// Note that `key` has no atom any more. Only the lock's holder calls this.
    fn remove(&self, key: u64) {
        let slots = self.slots().expect("a key with an atom is in the index");
        let mask = slots.len() - 1;
        let mut place = Self::home(key, slots.len());
        loop {
            let slot = &slots[place];
            if slot.key.load(Ordering::Relaxed) == key {
                slot.atom.store(0, Ordering::Release);
                self.live.fetch_sub(1, Ordering::Relaxed);
                return;
            }
            place = (place + 1) & mask;
        }
    }

    /// Have the slots hold nothing but the keys with an atom, with room for `wanted` of them: in the slots in use where
    /// they fit in a quarter of those, as a reader that misses meanwhile goes on to the lock, or in twice as many.
    fn lay_out(&self, wanted: usize) {
        let current = self.current.load(Ordering::Relaxed);
        let live: Vec<(u64, u32)> = self.slots().map_or_else(Vec::new, |slots| {
            slots
                .iter()
                .filter_map(|slot| {
                    let atom = slot.atom.load(Ordering::Relaxed);
                    (atom != 0).then(|| (slot.key.load(Ordering::Relaxed), atom))
                })
                .collect()
        });
        let capacity = self.slots().map_or(0, <[AtomIndexSlot]>::len);
        if wanted * 4 <= capacity {
            for slot in self.slots().expect("the slots have a capacity") {
                slot.atom.store(0, Ordering::Release);
                slot.key.store(0, Ordering::Release);
            }
        } else {
            let next = current.wrapping_add(1);
            self.generations[next].get_or_init(|| {
                (0..1_usize << (next as u32 + FIRST_ATOM_INDEX_BITS))
                    .map(|_| AtomIndexSlot::default())
                    .collect()
            });
            self.current.store(next, Ordering::Release);
        }
        self.used.store(0, Ordering::Relaxed);
        self.live.store(0, Ordering::Relaxed);
        for (key, atom) in live {
            self.insert(key, StyleAtomID(atom));
        }
    }
}

/// What the table's lock writes and any thread reads without it.
#[derive(Default)]
struct SharedAtoms {
    cells: AtomCells,
    raw: AtomIndex,
    qualified: AtomIndex,
}

impl SharedAtoms {
    fn key_of(&self, atom: StyleAtomID) -> Option<AtomKey> {
        self.cells.get(atom).and_then(AtomCell::key)
    }
}

#[derive(Default)]
struct AtomTable {
    shared: Arc<SharedAtoms>,
    available: BTreeSet<u32>,
    next: u32,
}

impl AtomTable {
    fn allocate(&mut self) -> StyleAtomID {
        match self.available.pop_first() {
            Some(atom) => StyleAtomID(atom),
            None => {
                self.next = self
                    .next
                    .checked_add(1)
                    .expect("process-global style atom space exhausted");
                StyleAtomID(self.next)
            }
        }
    }

    fn cell(&self, atom: StyleAtomID) -> &AtomCell {
        self.shared
            .cells
            .get(atom)
            .expect("an atom read through the table is live")
    }

    /// Takes one more reference to the live atom `atom`.
    fn reference(&mut self, atom: StyleAtomID) {
        let references = &self.cell(atom).document_references;
        assert_ne!(references.load(Ordering::Relaxed), 0, "a reference is to a live atom");
        references.fetch_add(1, Ordering::Relaxed);
    }

    /// Lets go of one reference to the live atom `atom`, which goes with the last of them.
    fn release(&mut self, atom: StyleAtomID) {
        let cell = self.cell(atom);
        if cell.document_references.fetch_sub(1, Ordering::Relaxed) != 1 {
            return;
        }
        let key = cell.key.load(Ordering::Relaxed);
        let lifetime = cell.raw_lifetime();
        let lowercase = StyleAtomID(cell.lowercase.swap(0, Ordering::Release));
        cell.kind.store(ATOM_KIND_FREE, Ordering::Release);
        match lifetime {
            Some(lifetime) => {
                self.shared.raw.remove(key);
                if lifetime == RawAtomLifetime::RetainedFlyString {
                    // SAFETY: acquire_raw retained exactly one global reference for this entry.
                    unsafe {
                        ak::release_utf16_string_with(key as usize, crate::css::ffi_stats::release_utf16_fly_string);
                    };
                }
            }
            None => self.shared.qualified.remove(key),
        }
        self.available.insert(atom.0);
        if !lowercase.is_none() && lowercase != atom {
            self.release(lowercase);
        }
    }

    /// What `atom` names, or none where no live atom has its number.
    fn key_of(&self, atom: StyleAtomID) -> Option<AtomKey> {
        self.shared.key_of(atom)
    }

    /// The raw identity of the name `atom` was interned from, for a live name the table holds a reference to.
    fn retained_raw(&self, atom: StyleAtomID) -> Option<usize> {
        let cell = self.shared.cells.get(atom)?;
        (cell.raw_lifetime() == Some(RawAtomLifetime::RetainedFlyString))
            .then(|| cell.key.load(Ordering::Relaxed) as usize)
    }

    /// The atom of the ASCII-lowercase form of the live atom `atom`'s name, which the cell of `atom` keeps live: the
    /// atom itself where the name has no uppercase letter, or where it was not interned from a name.
    fn ascii_lowercase(&mut self, atom: StyleAtomID) -> StyleAtomID {
        assert!(self.key_of(atom).is_some(), "an atom read through the table is live");
        let known = StyleAtomID(self.cell(atom).lowercase.load(Ordering::Relaxed));
        if !known.is_none() {
            return known;
        }
        let lowercase = match self.key_of(atom) {
            Some(AtomKey::Qualified(namespace, local)) => {
                let lowercase_local = self.ascii_lowercase(local);
                if lowercase_local == local {
                    atom
                } else {
                    self.acquire_qualified(namespace, lowercase_local)
                }
            }
            _ => match self.retained_raw(atom) {
                None => atom,
                Some(raw) => {
                    // SAFETY: The table holds a reference to the name, which this borrows.
                    let name = std::mem::ManuallyDrop::new(unsafe { ak::Utf16FlyString::from_raw_owned(raw) });
                    let text = name.to_utf16();
                    if !text
                        .iter()
                        .any(|unit| (u16::from(b'A')..=u16::from(b'Z')).contains(unit))
                    {
                        atom
                    } else {
                        let lowercase: Vec<u16> = text
                            .iter()
                            .map(|&unit| crate::css::ffi_support::ascii_lowercase(unit))
                            .collect();
                        let lowercase = ak::Utf16FlyString::from_utf16(&lowercase);
                        self.acquire_raw(lowercase.raw_identity(), RawAtomLifetime::RetainedFlyString)
                    }
                }
            },
        };
        self.cell(atom).lowercase.store(lowercase.0, Ordering::Release);
        lowercase
    }

    /// Makes the atom of a name that has none.
    fn make(&mut self, key: u64, kind: u8) -> StyleAtomID {
        let atom = self.allocate();
        let cell = self.shared.cells.get_or_make(atom);
        cell.key.store(key, Ordering::Release);
        cell.lowercase.store(0, Ordering::Release);
        cell.document_references.store(1, Ordering::Relaxed);
        cell.kind.store(kind, Ordering::Release);
        atom
    }

    fn acquire_raw(&mut self, raw: usize, lifetime: RawAtomLifetime) -> StyleAtomID {
        let lifetime = lifetime.for_raw(raw);
        if let Some(atom) = self.shared.raw.get(raw as u64) {
            let cell = self.cell(atom);
            assert_eq!(
                cell.raw_lifetime(),
                Some(lifetime),
                "one raw atom cannot mix live and test identity"
            );
            cell.document_references.fetch_add(1, Ordering::Relaxed);
            return atom;
        }
        let kind = match lifetime {
            RawAtomLifetime::RetainedFlyString => {
                // SAFETY: Live StyleEngine callers pass the raw identity of a referenced Utf16FlyString.
                unsafe { ak::reference_utf16_string(raw) };
                ATOM_KIND_RETAINED_RAW
            }
            RawAtomLifetime::SyntheticTextKey => ATOM_KIND_SYNTHETIC_RAW,
            #[cfg(test)]
            RawAtomLifetime::OpaqueTestToken => ATOM_KIND_OPAQUE_TEST_RAW,
        };
        let atom = self.make(raw as u64, kind);
        self.shared.raw.insert(raw as u64, atom);
        atom
    }

    fn acquire_qualified(&mut self, namespace: StyleAtomID, name: StyleAtomID) -> StyleAtomID {
        let key = qualified_key(namespace, name);
        if let Some(atom) = self.shared.qualified.get(key) {
            self.cell(atom).document_references.fetch_add(1, Ordering::Relaxed);
            return atom;
        }
        let atom = self.make(key, ATOM_KIND_QUALIFIED);
        self.shared.qualified.insert(key, atom);
        atom
    }
}

/// The style atoms of the process, which every style engine and document host of the process interns names in: one
/// name is one atom in every document. The process style holds the table, and each engine, host and lease names it.
pub(crate) struct ProcessAtoms {
    table: Mutex<AtomTable>,
    shared: Arc<SharedAtoms>,
}

impl Default for ProcessAtoms {
    fn default() -> Self {
        let table = AtomTable::default();
        Self {
            shared: Arc::clone(&table.shared),
            table: Mutex::new(table),
        }
    }
}

impl ProcessAtoms {
    fn lock(&self) -> MutexGuard<'_, AtomTable> {
        self.table.lock().expect("process style atom lock is poisoned")
    }

    /// The name `atom` was interned from, a qualified atom's being its local name's, or none where it was interned
    /// from text rather than from a name, or no live atom has the number.
    pub(crate) fn text(&self, atom: StyleAtomID) -> Option<RetainedUtf16FlyString> {
        let table = self.lock();
        let atom = match table.key_of(atom) {
            Some(AtomKey::Qualified(_, local)) => local,
            _ => atom,
        };
        let raw = table.retained_raw(atom)?;
        // SAFETY: The table holds a reference to the name, so it is live while the lock is held.
        Some(unsafe { RetainedUtf16FlyString::from_borrowed_raw(raw) })
    }

    /// The raw identity of the name `atom` was interned from, or of a qualified atom's local name, which the table
    /// holds a reference to while `atom` lives, or none where it was interned from text rather than from a name.
    pub(crate) fn local_raw_of(&self, atom: StyleAtomID) -> Option<usize> {
        let atom = match self.shared.key_of(atom) {
            Some(AtomKey::Qualified(_, local)) => local,
            _ => atom,
        };
        let cell = self.shared.cells.get(atom)?;
        (cell.raw_lifetime() == Some(RawAtomLifetime::RetainedFlyString))
            .then(|| cell.key.load(Ordering::Acquire) as usize)
    }

    /// A reference to the atom of the name `units` spell.
    pub(crate) fn intern(self: &Arc<Self>, units: &[u16]) -> AtomLease {
        let name = ak::Utf16FlyString::from_utf16(units);
        // SAFETY: The fly string is live for the call, and the lease takes a reference of its own.
        unsafe { AtomLease::acquire_raw(self, name.raw_identity()) }
    }

    /// The atom of the ASCII-lowercase form of `atom`'s name, which lives at least as long as `atom` does: what a
    /// quirks-mode document compares ids and classes by, and an HTML element in an HTML document its tag name by.
    pub(crate) fn ascii_lowercase(&self, atom: StyleAtomID) -> StyleAtomID {
        // NB: The caller holds a reference to `atom`, so its cell says what it says until the caller lets go. Only the
        //     first ask for a name takes the lock, as matching asks for each name it tests, on every thread.
        if let Some(cell) = self.shared.cells.get(atom) {
            let known = cell.lowercase.load(Ordering::Acquire);
            if known != 0 {
                return StyleAtomID(known);
            }
        }
        self.lock().ascii_lowercase(atom)
    }

    /// One reference to each distinct atom of `atoms`, all live, which the leases hold until they drop.
    pub(crate) fn lease(self: &Arc<Self>, atoms: impl IntoIterator<Item = StyleAtomID>) -> AtomLeases {
        let mut atoms: Vec<StyleAtomID> = atoms.into_iter().filter(|atom| !atom.is_none()).collect();
        atoms.sort_unstable_by_key(|atom| atom.0);
        atoms.dedup();
        let mut table = self.lock();
        for &atom in &atoms {
            table.reference(atom);
        }
        AtomLeases {
            process: Arc::clone(self),
            atoms: atoms.into_boxed_slice(),
        }
    }
}

/// A reference to each of a set of process atoms, which keeps every one of their numbers from naming another name
/// while the leases live. A structure that names atoms off the main thread, as parsed selectors do, holds its atoms
/// this way: one reference per distinct atom, however often the structure names it.
pub(crate) struct AtomLeases {
    process: Arc<ProcessAtoms>,
    /// The atoms, sorted by number.
    atoms: Box<[StyleAtomID]>,
}

impl std::fmt::Debug for AtomLeases {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_list()
            .entries(self.atoms.iter().map(|atom| atom.0))
            .finish()
    }
}

impl AtomLeases {
    /// The atoms the leases hold.
    pub(crate) fn atoms(&self) -> &[StyleAtomID] {
        &self.atoms
    }

    /// Whether the leases hold `atom`.
    #[cfg_attr(
        not(test),
        expect(dead_code, reason = "the selector arena checks its names through it")
    )]
    pub(crate) fn holds(&self, atom: StyleAtomID) -> bool {
        self.atoms.binary_search_by_key(&atom.0, |held| held.0).is_ok()
    }
}

impl Clone for AtomLeases {
    fn clone(&self) -> Self {
        self.process.lease(self.atoms.iter().copied())
    }
}

impl Drop for AtomLeases {
    fn drop(&mut self) {
        let mut table = self.process.lock();
        for &atom in &self.atoms {
            table.release(atom);
        }
    }
}

/// The atoms of the process, which parsed selectors name their names by.
pub(crate) fn process_atoms() -> &'static Arc<ProcessAtoms> {
    crate::render_state::process_style().atoms()
}

/// What a reference to a process-global atom is the reference to: the raw atom of a name, or a qualified name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum AtomKey {
    Raw(usize),
    Qualified(StyleAtomID, StyleAtomID),
}

/// A document host's reference to a process-global atom, which it takes as it interns a name beside its document's
/// engine, and which keeps the atom's number from being handed out again until the engine has adopted the name. It is
/// the reference itself, so it is neither `Clone` nor `Copy`, and dropping it releases it.
pub(crate) struct AtomLease {
    process: Arc<ProcessAtoms>,
    key: AtomKey,
    atom: StyleAtomID,
}

impl AtomLease {
    /// Takes a reference to the atom `process` has for the name whose raw identity is `raw`.
    ///
    /// # Safety
    /// `raw` must be the raw identity of a live `AK::Utf16FlyString`.
    pub(crate) unsafe fn acquire_raw(process: &Arc<ProcessAtoms>, raw: usize) -> Self {
        let atom = process.lock().acquire_raw(raw, RawAtomLifetime::RetainedFlyString);
        Self {
            process: Arc::clone(process),
            key: AtomKey::Raw(raw),
            atom,
        }
    }

    /// Takes a reference to the atom `process` has for `name` qualified by `namespace`.
    pub(crate) fn acquire_qualified(process: &Arc<ProcessAtoms>, namespace: StyleAtomID, name: StyleAtomID) -> Self {
        let atom = process.lock().acquire_qualified(namespace, name);
        Self {
            process: Arc::clone(process),
            key: AtomKey::Qualified(namespace, name),
            atom,
        }
    }

    /// Whether the lease is a reference to an atom of `process`.
    pub(super) fn is_of(&self, process: &Arc<ProcessAtoms>) -> bool {
        Arc::ptr_eq(&self.process, process)
    }

    pub(crate) fn atom(&self) -> StyleAtomID {
        self.atom
    }

    pub(super) fn key(&self) -> AtomKey {
        self.key
    }
}

impl Drop for AtomLease {
    fn drop(&mut self) {
        self.process.lock().release(self.atom);
    }
}

#[derive(Clone, Copy)]
enum AtomScope {
    #[cfg(test)]
    Document,
    Process(RawAtomLifetime),
}

/// What a document holds of the process's atoms: a reference to each atom it interned, which a sweep lets go of where
/// nothing in the document names it any more. The table answers what each name's atom is.
pub(super) struct DocumentAtoms {
    /// The atoms the document holds a reference to, as one bit per number of the table's atoms.
    held: Vec<u64>,
    held_count: usize,
    scope: AtomScope,
    /// The process's table, which the atoms are the process's references to where the scope is the process's.
    process: Arc<ProcessAtoms>,
    /// The table of a document whose atoms are its own, which no other document's are.
    #[cfg(test)]
    own: Option<Arc<ProcessAtoms>>,
    /// Atoms a layout publication names, and how many publications name each.
    ///
    /// A name the document carries is kept live by the state that carries it, but a published
    /// fact can name an atom no live element answers to - an SVG reference to an id that is not
    /// in the document. Nothing else roots such an atom, so a sweep would hand its number out
    /// again and the publication would then read as naming whatever took it.
    published: HashMap<StyleAtomID, u64>,
    sweep_at: usize,
}

const HELD_BITS_PER_WORD: u32 = u64::BITS;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ReclaimedStyleAtom {
    pub raw: usize,
    pub atom: StyleAtomID,
}

impl DocumentAtoms {
    pub(super) fn for_live_engine(process: &Arc<ProcessAtoms>) -> Self {
        #[cfg(test)]
        let scope = AtomScope::Document;
        #[cfg(not(test))]
        let scope = AtomScope::Process(RawAtomLifetime::RetainedFlyString);
        Self::new(scope, process)
    }

    /// As a live engine's outside tests: the process's atoms, which the selectors it parses name too.
    #[cfg(test)]
    pub(super) fn for_process_engine(process: &Arc<ProcessAtoms>) -> Self {
        Self::new(AtomScope::Process(RawAtomLifetime::RetainedFlyString), process)
    }

    #[cfg(test)]
    pub(super) fn for_test(process: &Arc<ProcessAtoms>) -> Self {
        Self::new(AtomScope::Process(RawAtomLifetime::OpaqueTestToken), process)
    }

    fn new(scope: AtomScope, process: &Arc<ProcessAtoms>) -> Self {
        Self {
            held: Vec::new(),
            held_count: 0,
            scope,
            process: Arc::clone(process),
            #[cfg(test)]
            own: matches!(scope, AtomScope::Document).then(Arc::default),
            published: HashMap::new(),
            sweep_at: 256,
        }
    }

    /// Interns the names of an engine that holds none of the process's yet in `process`, the table of the process
    /// style it shares.
    pub(super) fn share_process_atoms(&mut self, process: &Arc<ProcessAtoms>) {
        assert!(
            Arc::ptr_eq(&self.process, process) || !self.is_process_global() || self.held_count == 0,
            "an engine shares the process's atoms before it interns any"
        );
        self.process = Arc::clone(process);
    }

    /// The table the atoms are interned in, where they are the process's.
    pub(super) fn process_atoms(&self) -> &Arc<ProcessAtoms> {
        &self.process
    }

    /// The table the document's atoms are numbers of.
    fn table(&self) -> &Arc<ProcessAtoms> {
        #[cfg(test)]
        if let Some(own) = &self.own {
            return own;
        }
        &self.process
    }

    /// How raw identities of names are held.
    fn raw_lifetime(&self) -> RawAtomLifetime {
        match self.scope {
            #[cfg(test)]
            AtomScope::Document => RawAtomLifetime::OpaqueTestToken,
            AtomScope::Process(lifetime) => lifetime,
        }
    }

    fn holds(&self, atom: StyleAtomID) -> bool {
        let word = (atom.0 / HELD_BITS_PER_WORD) as usize;
        self.held
            .get(word)
            .is_some_and(|word| word & (1 << (atom.0 % HELD_BITS_PER_WORD)) != 0)
    }

    /// Note that the document took a reference to `atom`.
    fn note_held(&mut self, atom: StyleAtomID) {
        let word = (atom.0 / HELD_BITS_PER_WORD) as usize;
        if self.held.len() <= word {
            self.held.resize(word + 1, 0);
        }
        self.held[word] |= 1 << (atom.0 % HELD_BITS_PER_WORD);
        self.held_count += 1;
    }

    /// Every atom the document holds a reference to, by increasing number.
    fn held_atoms(&self) -> impl Iterator<Item = StyleAtomID> + '_ {
        self.held.iter().enumerate().flat_map(|(index, &word)| {
            let base = u32::try_from(index).expect("style atom numbers are u32") * HELD_BITS_PER_WORD;
            (0..HELD_BITS_PER_WORD)
                .filter(move |bit| word & (1 << bit) != 0)
                .map(move |bit| StyleAtomID(base + bit))
        })
    }

    /// The atom of `key` the document holds a reference to, read without the table's lock.
    fn held_without_lock(
        &self,
        index: &AtomIndex,
        key: u64,
        kind_of_key: impl Fn(&AtomCell) -> bool,
    ) -> Option<StyleAtomID> {
        let atom = index.get(key)?;
        if !self.holds(atom) {
            return None;
        }
        // NB: The document's reference keeps the cell as it is. The index may have answered for a key that just lost
        //     its atom, with a number another name took since.
        let cell = self.table().shared.cells.get(atom)?;
        (cell.key.load(Ordering::Acquire) == key && kind_of_key(cell)).then_some(atom)
    }

    pub(super) fn intern_raw(&mut self, raw: usize) -> StyleAtomID {
        if let Some(atom) = self.held_without_lock(&self.table().shared.raw, raw as u64, |cell| {
            cell.raw_lifetime().is_some()
        }) {
            return atom;
        }
        let atom = {
            let mut table = self.table().lock();
            match table.shared.raw.get(raw as u64) {
                Some(atom) if self.holds(atom) => return atom,
                _ => table.acquire_raw(raw, self.raw_lifetime()),
            }
        };
        self.note_held(atom);
        atom
    }

    pub(super) fn intern_qualified(&mut self, namespace: StyleAtomID, name: StyleAtomID) -> StyleAtomID {
        let key = qualified_key(namespace, name);
        if let Some(atom) = self.held_without_lock(&self.table().shared.qualified, key, |cell| {
            cell.kind.load(Ordering::Acquire) == ATOM_KIND_QUALIFIED
        }) {
            return atom;
        }
        let atom = {
            let mut table = self.table().lock();
            match table.shared.qualified.get(key) {
                Some(atom) if self.holds(atom) => return atom,
                _ => table.acquire_qualified(namespace, name),
            }
        };
        self.note_held(atom);
        atom
    }

    /// Whether the atoms are the process's, which every other engine's also are.
    pub(super) fn is_process_global(&self) -> bool {
        matches!(self.scope, AtomScope::Process(_))
    }

    pub(super) fn should_sweep(&self) -> bool {
        self.held_count >= self.sweep_at
    }

    /// Keep `atom` out of every sweep until the publication naming it is cleared or replaced.
    pub(super) fn retain_published(&mut self, atom: StyleAtomID) {
        if atom.is_none() {
            return;
        }
        *self.published.entry(atom).or_default() += 1;
    }

    pub(super) fn release_published(&mut self, atom: StyleAtomID) {
        if atom.is_none() {
            return;
        }
        let Entry::Occupied(mut entry) = self.published.entry(atom) else {
            unreachable!("a published atom must have a live count");
        };
        let count = entry.get_mut();
        *count = count.checked_sub(1).expect("published atom count underflow");
        if *count == 0 {
            entry.remove();
        }
    }

    /// Add published names and the raw components of every live qualified name.
    pub(super) fn mark_sweep_dependencies<S>(&self, live: &mut HashSet<StyleAtomID, S>)
    where
        S: BuildHasher,
    {
        live.extend(self.published.keys().copied());
        let shared = &self.table().shared;
        for atom in self.held_atoms() {
            if live.contains(&atom)
                && let Some(AtomKey::Qualified(namespace, name)) = shared.key_of(atom)
            {
                if !namespace.is_none() {
                    live.insert(namespace);
                }
                if !name.is_none() {
                    live.insert(name);
                }
            }
        }
    }

    /// Return atoms not owned by semantic state after all derived dependencies have been marked.
    /// Derived atom-keyed catalogs must forget these identities before finish_sweep makes their
    /// integers available for reuse.
    pub(super) fn reclaimable_for_sweep<S>(&self, live: &HashSet<StyleAtomID, S>) -> Vec<StyleAtomID>
    where
        S: BuildHasher,
    {
        self.held_atoms().filter(|atom| !live.contains(atom)).collect()
    }

    /// The raw identity of the name each of `atoms` was interned from, a qualified atom's being its name's, or none
    /// where one was interned from text rather than from a name.
    pub(super) fn raw_identities_of(&self, atoms: impl IntoIterator<Item = StyleAtomID>) -> Option<Vec<usize>> {
        let shared = &self.table().shared;
        atoms
            .into_iter()
            .map(|atom| {
                let atom = match shared.key_of(atom) {
                    Some(AtomKey::Qualified(_, local)) => local,
                    _ => atom,
                };
                match shared.key_of(atom) {
                    Some(AtomKey::Raw(raw)) if raw < TEXT_KEY_FLOOR => Some(raw),
                    _ => None,
                }
            })
            .collect()
    }

    /// The name `atom` was interned from, a qualified atom's being its name's, for a diagnostic to
    /// show, or none where it was interned from text or the table holds no names.
    pub(super) fn name_of(&self, atom: StyleAtomID) -> Option<String> {
        if !matches!(self.scope, AtomScope::Process(RawAtomLifetime::RetainedFlyString)) {
            return None;
        }
        let name = self.process.text(atom)?;
        Some(String::from_utf16_lossy(&name.to_utf16()))
    }

    /// A sweep marks everything live whatever it reclaims, so the next one waits until the table has doubled: its cost
    /// spreads over as many new atoms as this one left live.
    fn schedule_next_sweep(&mut self) {
        self.sweep_at = (2 * self.held_count).max(256);
    }

    pub(super) fn finish_sweep(&mut self, reclaimable: &[StyleAtomID]) -> Vec<ReclaimedStyleAtom> {
        let reclaimed = {
            let mut table = self.table().lock();
            reclaimable
                .iter()
                .map(|&atom| {
                    let raw = match table.key_of(atom) {
                        Some(AtomKey::Raw(raw)) => raw,
                        _ => 0,
                    };
                    table.release(atom);
                    ReclaimedStyleAtom { raw, atom }
                })
                .collect::<Vec<_>>()
        };
        for &atom in reclaimable {
            self.held[(atom.0 / HELD_BITS_PER_WORD) as usize] &= !(1 << (atom.0 % HELD_BITS_PER_WORD));
        }
        self.held_count -= reclaimable.len();
        self.schedule_next_sweep();
        reclaimed
    }
}

/// A fork of a document holds its own document reference to each atom the document holds.
impl Clone for DocumentAtoms {
    fn clone(&self) -> Self {
        {
            let mut table = self.table().lock();
            for atom in self.held_atoms() {
                table.reference(atom);
            }
        }
        Self {
            held: self.held.clone(),
            held_count: self.held_count,
            scope: self.scope,
            process: Arc::clone(&self.process),
            #[cfg(test)]
            own: self.own.clone(),
            published: self.published.clone(),
            sweep_at: self.sweep_at,
        }
    }
}

impl Drop for DocumentAtoms {
    fn drop(&mut self) {
        let mut table = self.table().lock();
        for atom in self.held_atoms() {
            table.release(atom);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn prepare_sweep(atoms: &DocumentAtoms, live: &mut HashSet<StyleAtomID>) -> Vec<StyleAtomID> {
        atoms.mark_sweep_dependencies(live);
        atoms.reclaimable_for_sweep(live)
    }

    fn lease_name(process: &Arc<ProcessAtoms>, text: &str) -> AtomLease {
        let name = ak::Utf16FlyString::from_utf16(&text.encode_utf16().collect::<Vec<_>>());
        // SAFETY: The fly string is live for the call, and the lease takes a reference of its own.
        unsafe { AtomLease::acquire_raw(process, name.raw_identity()) }
    }

    #[test]
    fn a_name_is_lowercased_to_the_atom_its_lowercase_form_has() {
        let process = Arc::default();
        let mixed = lease_name(&process, "FooBar");
        let lowercase = lease_name(&process, "foobar");
        assert_ne!(mixed.atom(), lowercase.atom());
        assert_eq!(process.ascii_lowercase(mixed.atom()), lowercase.atom());
        assert_eq!(process.ascii_lowercase(lowercase.atom()), lowercase.atom());
        let text = process.text(mixed.atom()).unwrap();
        assert_eq!(String::from_utf16_lossy(&text.to_utf16()), "FooBar");
    }

    #[test]
    fn a_lowercase_form_lives_as_long_as_the_name_it_folds() {
        let process = Arc::default();
        let mixed = lease_name(&process, "ClassName");
        let lowercase = process.ascii_lowercase(mixed.atom());
        assert_ne!(lowercase, mixed.atom());
        // Nothing but the mixed-case entry references the lowercase form, which a later lease still finds.
        let again = lease_name(&process, "classname");
        assert_eq!(again.atom(), lowercase);
        drop(again);
        let mixed_atom = mixed.atom();
        drop(mixed);
        // Both numbers are free again, so the next two names take them.
        let first = lease_name(&process, "x");
        let second = lease_name(&process, "y");
        let mut taken = [first.atom().0, second.atom().0];
        taken.sort_unstable();
        let mut freed = [mixed_atom.0, lowercase.0];
        freed.sort_unstable();
        assert_eq!(taken, freed);
    }

    #[test]
    fn leases_keep_each_distinct_atom_until_they_drop() {
        let process = Arc::default();
        let name = lease_name(&process, "kept");
        let atom = name.atom();
        let leases = process.lease([atom, atom, StyleAtomID::NONE]);
        assert!(leases.holds(atom));
        let copy = leases.clone();
        drop((name, leases));
        assert!(process.text(atom).is_some());
        drop(copy);
        assert!(process.text(atom).is_none());
    }

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
        let process = Arc::default();
        let first_atom;
        {
            let mut first = DocumentAtoms::for_test(&process);
            let mut second = DocumentAtoms::for_test(&process);
            first_atom = first.intern_raw(0x1234);
            assert_eq!(second.intern_raw(0x1234), first_atom);
            assert_ne!(second.intern_raw(0x5678), first_atom);
        }
        let mut later = DocumentAtoms::for_test(&process);
        assert_eq!(later.intern_raw(0x9abc), first_atom);
    }

    #[test]
    fn process_atoms_wait_for_every_raw_and_qualified_owner() {
        let process = Arc::default();
        let mut first = DocumentAtoms::for_test(&process);
        let mut second = DocumentAtoms::for_test(&process);
        let namespace = first.intern_raw(0x1000);
        let name = first.intern_raw(0x2000);
        let qualified = first.intern_qualified(namespace, name);
        assert_eq!(second.intern_raw(0x1000), namespace);
        assert_eq!(second.intern_raw(0x2000), name);
        assert_eq!(second.intern_qualified(namespace, name), qualified);

        let first_reclaimable = prepare_sweep(&first, &mut HashSet::new());
        first.finish_sweep(&first_reclaimable);
        assert_eq!(second.intern_raw(0x1000), namespace);
        assert_eq!(second.intern_qualified(namespace, name), qualified);

        let second_reclaimable = prepare_sweep(&second, &mut HashSet::new());
        second.finish_sweep(&second_reclaimable);
        let mut later = DocumentAtoms::for_test(&process);
        assert_eq!(later.intern_raw(0x3000), namespace);
        assert_eq!(later.intern_raw(0x4000), name);
        assert_eq!(later.intern_qualified(namespace, name), qualified);
    }

    #[test]
    fn qualified_atoms_share_the_global_identity_space() {
        let process = Arc::default();
        let mut first = DocumentAtoms::for_test(&process);
        let mut second = DocumentAtoms::for_test(&process);
        let namespace = first.intern_raw(0x1000);
        let name = first.intern_raw(0x2000);
        assert_eq!(second.intern_raw(0x1000), namespace);
        assert_eq!(second.intern_raw(0x2000), name);
        assert_eq!(
            first.intern_qualified(namespace, name),
            second.intern_qualified(namespace, name)
        );
    }

    #[test]
    fn qualified_atoms_keep_their_component_atoms_live() {
        let mut atoms = DocumentAtoms::for_live_engine(&Arc::default());
        let namespace = atoms.intern_raw(0x1000);
        let name = atoms.intern_raw(0x2000);
        let qualified = atoms.intern_qualified(namespace, name);
        let mut live = HashSet::from([qualified]);
        assert!(prepare_sweep(&atoms, &mut live).is_empty());
        assert_eq!(live, HashSet::from([namespace, name, qualified]));
    }

    #[test]
    fn a_published_name_survives_a_sweep_that_no_other_owner_reaches() {
        let mut atoms = DocumentAtoms::for_live_engine(&Arc::default());
        let referenced = atoms.intern_raw(0x1000);
        let other = atoms.intern_raw(0x2000);
        atoms.retain_published(referenced);
        assert_eq!(prepare_sweep(&atoms, &mut HashSet::new()), [other]);

        // The publication is replaced by one naming a different name, so the old one becomes
        // reclaimable and its number can be issued again.
        let replacement = atoms.intern_raw(0x3000);
        atoms.retain_published(replacement);
        atoms.release_published(referenced);
        let reclaimable = prepare_sweep(&atoms, &mut HashSet::new());
        assert_eq!(reclaimable, [referenced, other]);
        atoms.finish_sweep(&reclaimable);
        assert_eq!(atoms.intern_raw(0x4000), referenced);
    }

    #[test]
    fn two_publications_of_one_name_each_keep_it() {
        let mut atoms = DocumentAtoms::for_live_engine(&Arc::default());
        let referenced = atoms.intern_raw(0x1000);
        atoms.retain_published(referenced);
        atoms.retain_published(referenced);
        atoms.release_published(referenced);
        assert!(prepare_sweep(&atoms, &mut HashSet::new()).is_empty());
        atoms.release_published(referenced);
        assert_eq!(prepare_sweep(&atoms, &mut HashSet::new()), [referenced]);
    }

    #[test]
    fn document_atoms_reuse_reclaimed_identities_in_sorted_order() {
        let mut atoms = DocumentAtoms::for_live_engine(&Arc::default());
        let first = atoms.intern_raw(0x1000);
        let second = atoms.intern_raw(0x2000);
        let third = atoms.intern_raw(0x3000);
        let reclaimable = prepare_sweep(&atoms, &mut HashSet::from([second]));
        assert_eq!(reclaimable, [first, third]);
        assert_eq!(
            atoms.finish_sweep(&reclaimable),
            [
                ReclaimedStyleAtom {
                    raw: 0x1000,
                    atom: first,
                },
                ReclaimedStyleAtom {
                    raw: 0x3000,
                    atom: third,
                },
            ]
        );
        assert_eq!(atoms.intern_raw(0x4000), first);
        assert_eq!(atoms.intern_raw(0x5000), third);
    }

    #[test]
    fn the_index_finds_what_a_table_holds_through_churn_and_growth() {
        let process = Arc::<ProcessAtoms>::default();
        let mut atoms = DocumentAtoms::for_test(&process);
        // Far more names than the first slots hold over time, few at once: the slots are laid out again in place.
        for round in 0..64_usize {
            let names: Vec<_> = (0..256)
                .map(|offset| atoms.intern_raw(0x10_0000 + round * 256 + offset))
                .collect();
            assert_eq!(atoms.intern_raw(0x10_0000 + round * 256 + 7), names[7]);
            let reclaimable = prepare_sweep(&atoms, &mut HashSet::new());
            atoms.finish_sweep(&reclaimable);
        }
        assert_eq!(process.shared.raw.current.load(Ordering::Relaxed), 0);
        // Many names at once grow them, and each name keeps its atom.
        let names: Vec<_> = (0..10_000).map(|offset| atoms.intern_raw(0x20_0000 + offset)).collect();
        for (offset, &atom) in names.iter().enumerate() {
            assert_eq!(atoms.intern_raw(0x20_0000 + offset), atom);
            assert_eq!(process.shared.raw.get(0x20_0000 + offset as u64), Some(atom));
        }
    }

    #[test]
    fn documents_on_several_threads_read_each_name_as_the_one_atom_it_is() {
        let process = Arc::<ProcessAtoms>::default();
        let threads: Vec<_> = (0..4_usize)
            .map(|thread| {
                let process = Arc::clone(&process);
                std::thread::spawn(move || {
                    let mut atoms = DocumentAtoms::for_test(&process);
                    for round in 0..200_usize {
                        // Names every thread shares, and names that come and go in this thread alone.
                        for offset in 0..64 {
                            let shared = 0x10_0000 + offset;
                            let atom = atoms.intern_raw(shared);
                            assert_eq!(process.shared.key_of(atom), Some(AtomKey::Raw(shared)));
                            assert_eq!(atoms.intern_raw(shared), atom);
                            let own = 0x100_0000 * (thread + 1) + round * 64 + offset;
                            let atom = atoms.intern_raw(own);
                            assert_eq!(process.shared.key_of(atom), Some(AtomKey::Raw(own)));
                        }
                        let reclaimable = prepare_sweep(&atoms, &mut HashSet::new());
                        atoms.finish_sweep(&reclaimable);
                    }
                })
            })
            .collect();
        for thread in threads {
            thread.join().expect("the thread keeps its names apart");
        }
    }

    #[test]
    fn repeated_churn_keeps_the_document_identity_space_bounded() {
        let mut atoms = DocumentAtoms::for_live_engine(&Arc::default());
        let mut highest = 0;
        for round in 0..8 {
            for offset in 0..256 {
                highest = highest.max(atoms.intern_raw(0x1000 + round * 256 + offset).0);
            }
            let reclaimable = prepare_sweep(&atoms, &mut HashSet::new());
            assert_eq!(reclaimable.len(), 256);
            atoms.finish_sweep(&reclaimable);
        }
        assert_eq!(highest, 256);
    }
}
