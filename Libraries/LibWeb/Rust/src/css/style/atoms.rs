/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::BTreeSet;
use std::collections::HashMap;
use std::collections::HashSet;
use std::hash::BuildHasher;
use std::rc::Rc;
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
    cpp_memoized_raws: HashSet<usize>,
    qualified: HashMap<(u32, u32), StyleAtomID>,
    scope: AtomScope,
    pins: Rc<AtomPins>,
    #[cfg(test)]
    available: BTreeSet<u32>,
    #[cfg(test)]
    next: u32,
    sweep_at: usize,
    reported_pin_releases: Cell<u64>,
}

pub(super) struct PinnedAtoms {
    atoms: Vec<StyleAtomID>,
    pins: Rc<AtomPins>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct ReclaimedStyleAtom {
    pub raw: usize,
    pub atom: StyleAtomID,
}

#[derive(Default)]
struct AtomPins {
    counts: RefCell<HashMap<StyleAtomID, u64>>,
    releases: Cell<u64>,
}

pub(super) struct AtomSweepDecision {
    pub should_sweep: bool,
    pub skipped_pin_releases: u64,
}

pub(super) const PIN_RELEASES_PER_SWEEP: u64 = 256;

impl Drop for PinnedAtoms {
    fn drop(&mut self) {
        let mut pinned = self.pins.counts.borrow_mut();
        for atom in &self.atoms {
            let count = pinned.get_mut(atom).expect("a pinned atom must have a live count");
            *count = count.checked_sub(1).expect("pinned atom count underflow");
            if *count == 0 {
                pinned.remove(atom);
            }
        }
        if !self.atoms.is_empty() {
            self.pins.releases.set(
                self.pins
                    .releases
                    .get()
                    .checked_add(1)
                    .expect("atom pin release count overflow"),
            );
        }
    }
}

impl DocumentAtoms {
    pub(super) fn for_live_engine() -> Self {
        Self {
            raw: HashMap::new(),
            cpp_memoized_raws: HashSet::new(),
            qualified: HashMap::new(),
            #[cfg(test)]
            scope: AtomScope::Document,
            #[cfg(not(test))]
            scope: AtomScope::Process(RawAtomLifetime::RetainedFlyString),
            pins: Rc::new(AtomPins::default()),
            #[cfg(test)]
            available: BTreeSet::new(),
            #[cfg(test)]
            next: 0,
            sweep_at: 256,
            reported_pin_releases: Cell::new(0),
        }
    }

    pub(super) fn for_replay() -> Self {
        Self {
            raw: HashMap::new(),
            cpp_memoized_raws: HashSet::new(),
            qualified: HashMap::new(),
            scope: AtomScope::Process(RawAtomLifetime::OpaqueReplayToken),
            pins: Rc::new(AtomPins::default()),
            #[cfg(test)]
            available: BTreeSet::new(),
            #[cfg(test)]
            next: 0,
            sweep_at: 256,
            reported_pin_releases: Cell::new(0),
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

    pub(super) fn intern_cpp_raw(&mut self, raw: usize) -> StyleAtomID {
        self.cpp_memoized_raws.insert(raw);
        self.intern_raw(raw)
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
    fn allocate_document_atom(&mut self) -> StyleAtomID {
        if let Some(atom) = self.available.pop_first() {
            return StyleAtomID(atom);
        }
        self.next = self.next.checked_add(1).expect("document style atom space exhausted");
        StyleAtomID(self.next)
    }

    pub(super) fn pin(&self, atoms: impl IntoIterator<Item = StyleAtomID>) -> PinnedAtoms {
        let atoms = atoms.into_iter().filter(|atom| !atom.is_none()).collect::<Vec<_>>();
        let mut pinned = self.pins.counts.borrow_mut();
        for &atom in &atoms {
            *pinned.entry(atom).or_default() += 1;
        }
        drop(pinned);
        PinnedAtoms {
            atoms,
            pins: Rc::clone(&self.pins),
        }
    }

    pub(super) fn sweep_decision(&self) -> AtomSweepDecision {
        let growth_requires_sweep = self.raw.len() + self.qualified.len() >= self.sweep_at;
        let pin_releases = self.pins.releases.get();
        let pin_releases_require_sweep = pin_releases >= PIN_RELEASES_PER_SWEEP;
        let skipped_pin_releases = if growth_requires_sweep || pin_releases_require_sweep {
            0
        } else {
            pin_releases
                .checked_sub(self.reported_pin_releases.get())
                .expect("reported atom pin releases exceed releases")
        };
        self.reported_pin_releases.set(pin_releases);
        AtomSweepDecision {
            should_sweep: growth_requires_sweep || pin_releases_require_sweep,
            skipped_pin_releases,
        }
    }

    /// Add transient pins and the raw components of every live qualified name.
    pub(super) fn mark_sweep_dependencies<S>(&self, live: &mut HashSet<StyleAtomID, S>)
    where
        S: BuildHasher,
    {
        live.extend(self.pins.counts.borrow().keys().copied());
        for (&(namespace, name), &qualified) in &self.qualified {
            if live.contains(&qualified) {
                if namespace != 0 {
                    live.insert(StyleAtomID(namespace));
                }
                if name != 0 {
                    live.insert(StyleAtomID(name));
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
        let mut reclaimable = self
            .raw
            .values()
            .chain(self.qualified.values())
            .copied()
            .filter(|atom| !live.contains(atom))
            .collect::<Vec<_>>();
        reclaimable.sort_unstable_by_key(|atom| atom.0);
        reclaimable
    }

    pub(super) fn finish_sweep(&mut self, reclaimable: &[StyleAtomID]) -> Vec<ReclaimedStyleAtom> {
        let reclaimable = reclaimable.iter().copied().collect::<HashSet<_>>();
        let mut raw = Vec::new();
        self.raw.retain(|&identity, &mut atom| {
            if !reclaimable.contains(&atom) {
                return true;
            }
            raw.push((identity, atom));
            false
        });
        let mut qualified = Vec::new();
        self.qualified.retain(|&key, &mut atom| {
            if !reclaimable.contains(&atom) {
                return true;
            }
            qualified.push((key, atom));
            false
        });

        match self.scope {
            #[cfg(test)]
            AtomScope::Document => {
                self.available.extend(reclaimable.iter().map(|atom| atom.0));
            }
            AtomScope::Process(_) => {
                let mut global = global_atoms()
                    .lock()
                    .expect("process-global style atom lock is poisoned");
                for &(key, atom) in &qualified {
                    global.release_qualified(key, atom);
                }
                for &(identity, atom) in &raw {
                    global.release_raw(identity, atom);
                }
            }
        }

        self.sweep_at = self.raw.len() + self.qualified.len() + 256;
        self.pins.releases.set(0);
        self.reported_pin_releases.set(0);
        let mut reclaimed = raw
            .into_iter()
            .map(|(raw, atom)| ReclaimedStyleAtom {
                raw: if self.cpp_memoized_raws.remove(&raw) { raw } else { 0 },
                atom,
            })
            .chain(
                qualified
                    .into_iter()
                    .map(|(_, atom)| ReclaimedStyleAtom { raw: 0, atom }),
            )
            .collect::<Vec<_>>();
        reclaimed.sort_unstable_by_key(|entry| entry.atom.0);
        reclaimed
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

    fn prepare_sweep(atoms: &DocumentAtoms, live: &mut HashSet<StyleAtomID>) -> Vec<StyleAtomID> {
        atoms.mark_sweep_dependencies(live);
        atoms.reclaimable_for_sweep(live)
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
    fn process_atoms_wait_for_every_raw_and_qualified_owner() {
        let _test_lock = GLOBAL_ATOM_TEST_LOCK.lock().unwrap();
        let mut first = DocumentAtoms::for_replay();
        let mut second = DocumentAtoms::for_replay();
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
        let mut later = DocumentAtoms::for_replay();
        assert_eq!(later.intern_raw(0x3000), namespace);
        assert_eq!(later.intern_raw(0x4000), name);
        assert_eq!(later.intern_qualified(namespace, name), qualified);
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

    #[test]
    fn qualified_atoms_keep_their_component_atoms_live() {
        let mut atoms = DocumentAtoms::for_live_engine();
        let namespace = atoms.intern_raw(0x1000);
        let name = atoms.intern_raw(0x2000);
        let qualified = atoms.intern_qualified(namespace, name);
        let mut live = HashSet::from([qualified]);
        assert!(prepare_sweep(&atoms, &mut live).is_empty());
        assert_eq!(live, HashSet::from([namespace, name, qualified]));
    }

    #[test]
    fn pins_delay_reclamation_until_their_owner_is_destroyed() {
        let mut atoms = DocumentAtoms::for_live_engine();
        let atom = atoms.intern_cpp_raw(0x1000);
        let pin = atoms.pin([atom]);
        assert!(prepare_sweep(&atoms, &mut HashSet::new()).is_empty());
        drop(pin);
        let decision = atoms.sweep_decision();
        assert!(!decision.should_sweep);
        assert_eq!(decision.skipped_pin_releases, 1);
        assert_eq!(atoms.sweep_decision().skipped_pin_releases, 0);
        for _ in 1..PIN_RELEASES_PER_SWEEP {
            drop(atoms.pin([atom]));
        }
        assert!(atoms.sweep_decision().should_sweep);
        let reclaimable = prepare_sweep(&atoms, &mut HashSet::new());
        assert_eq!(reclaimable, [atom]);
        assert_eq!(
            atoms.finish_sweep(&reclaimable),
            [ReclaimedStyleAtom { raw: 0x1000, atom }]
        );
    }

    #[test]
    fn document_atoms_reuse_reclaimed_identities_in_sorted_order() {
        let mut atoms = DocumentAtoms::for_live_engine();
        let first = atoms.intern_cpp_raw(0x1000);
        let second = atoms.intern_raw(0x2000);
        let third = atoms.intern_cpp_raw(0x3000);
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
    fn compiler_only_atoms_do_not_claim_a_cpp_memo_entry() {
        let mut atoms = DocumentAtoms::for_live_engine();
        let atom = atoms.intern_raw(0x1000);
        let reclaimable = prepare_sweep(&atoms, &mut HashSet::new());
        assert_eq!(atoms.finish_sweep(&reclaimable), [ReclaimedStyleAtom { raw: 0, atom }]);
    }

    #[test]
    fn repeated_churn_keeps_the_document_identity_space_bounded() {
        let mut atoms = DocumentAtoms::for_live_engine();
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
