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
use std::sync::Mutex;
use std::sync::OnceLock;

use super::index::StyleAtomID;

const TEXT_KEY_MASK: u64 = 0x0000_ffff_ffff_ffff;
const TEXT_KEY_FLOOR: usize = usize::MAX - TEXT_KEY_MASK as usize;

#[must_use]
pub(super) fn synthetic_text_atom_key(hash: u64) -> usize {
    usize::MAX - (hash & TEXT_KEY_MASK) as usize
}

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
                "one raw atom cannot mix live and test identity"
            );
            entry.document_references += 1;
            return entry.atom;
        }
        let atom = self.allocate();
        if lifetime == RawAtomLifetime::RetainedFlyString {
            // SAFETY: Live StyleEngine callers pass the raw identity of a referenced Utf16FlyString.
            unsafe { ak::reference_utf16_string(raw) };
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
        let Entry::Occupied(mut occupied) = self.raw.entry(raw) else {
            unreachable!("a document must release a live global atom");
        };
        let entry = occupied.get_mut();
        assert_eq!(entry.atom, expected);
        entry.document_references -= 1;
        if entry.document_references != 0 {
            return;
        }
        let entry = occupied.remove();
        self.available.insert(entry.atom.0);
        if entry.raw_lifetime == Some(RawAtomLifetime::RetainedFlyString) {
            // SAFETY: acquire_raw retained exactly one global reference for this entry.
            unsafe { ak::release_utf16_string_with(raw, crate::css::ffi_stats::release_utf16_fly_string) };
        }
    }

    /// Takes one more document reference to the live raw atom `raw`, for a fork of a document that holds it.
    fn reference_raw(&mut self, raw: usize, expected: StyleAtomID) {
        let entry = self
            .raw
            .get_mut(&raw)
            .expect("a document references a live global atom");
        assert_eq!(entry.atom, expected);
        entry.document_references += 1;
    }

    /// Takes one more document reference to the live qualified atom `key`, for a fork of a document that holds it.
    fn reference_qualified(&mut self, key: (u32, u32), expected: StyleAtomID) {
        let entry = self
            .qualified
            .get_mut(&key)
            .expect("a document references a live global qualified atom");
        assert_eq!(entry.atom, expected);
        entry.document_references += 1;
    }

    fn release_qualified(&mut self, key: (u32, u32), expected: StyleAtomID) {
        let Entry::Occupied(mut occupied) = self.qualified.entry(key) else {
            unreachable!("a document must release a live global qualified atom");
        };
        let entry = occupied.get_mut();
        assert_eq!(entry.atom, expected);
        entry.document_references -= 1;
        if entry.document_references != 0 {
            return;
        }
        let entry = occupied.remove();
        self.available.insert(entry.atom.0);
    }
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
    key: AtomKey,
    atom: StyleAtomID,
}

impl AtomLease {
    /// Takes a reference to the global atom of the name whose raw identity is `raw`.
    ///
    /// # Safety
    /// `raw` must be the raw identity of a live `AK::Utf16FlyString`.
    pub(crate) unsafe fn acquire_raw(raw: usize) -> Self {
        let atom = global_atoms()
            .lock()
            .expect("process-global style atom lock is poisoned")
            .acquire_raw(raw, RawAtomLifetime::RetainedFlyString);
        Self {
            key: AtomKey::Raw(raw),
            atom,
        }
    }

    /// Takes a reference to the global atom of `name` qualified by `namespace`.
    pub(crate) fn acquire_qualified(namespace: StyleAtomID, name: StyleAtomID) -> Self {
        let atom = global_atoms()
            .lock()
            .expect("process-global style atom lock is poisoned")
            .acquire_qualified(namespace, name);
        Self {
            key: AtomKey::Qualified(namespace, name),
            atom,
        }
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
        let mut global = global_atoms()
            .lock()
            .expect("process-global style atom lock is poisoned");
        match self.key {
            AtomKey::Raw(raw) => global.release_raw(raw, self.atom),
            AtomKey::Qualified(namespace, name) => global.release_qualified((namespace.0, name.0), self.atom),
        }
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
    /// Atoms a layout publication names, and how many publications name each.
    ///
    /// A name the document carries is kept live by the state that carries it, but a published
    /// fact can name an atom no live element answers to - an SVG reference to an id that is not
    /// in the document. Nothing else roots such an atom, so a sweep would hand its number out
    /// again and the publication would then read as naming whatever took it.
    published: HashMap<StyleAtomID, u64>,
    #[cfg(test)]
    available: BTreeSet<u32>,
    #[cfg(test)]
    next: u32,
    sweep_at: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ReclaimedStyleAtom {
    pub raw: usize,
    pub atom: StyleAtomID,
}

impl DocumentAtoms {
    pub(super) fn for_live_engine() -> Self {
        #[cfg(test)]
        let scope = AtomScope::Document;
        #[cfg(not(test))]
        let scope = AtomScope::Process(RawAtomLifetime::RetainedFlyString);
        Self::new(scope)
    }

    #[cfg(test)]
    pub(super) fn for_test() -> Self {
        Self::new(AtomScope::Process(RawAtomLifetime::OpaqueTestToken))
    }

    fn new(scope: AtomScope) -> Self {
        Self {
            raw: HashMap::new(),
            cpp_memoized_raws: HashSet::new(),
            qualified: HashMap::new(),
            scope,
            published: HashMap::new(),
            #[cfg(test)]
            available: BTreeSet::new(),
            #[cfg(test)]
            next: 0,
            sweep_at: 256,
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

    /// Whether the atoms are the process's, which every other engine's also are.
    pub(super) fn is_process_global(&self) -> bool {
        matches!(self.scope, AtomScope::Process(_))
    }

    pub(super) fn should_sweep(&self) -> bool {
        self.raw.len() + self.qualified.len() >= self.sweep_at
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

    /// A sweep marks everything live whatever it reclaims, so the next one waits until the table has doubled: its cost
    /// spreads over as many new atoms as this one left live.
    fn schedule_next_sweep(&mut self) {
        self.sweep_at = (2 * (self.raw.len() + self.qualified.len())).max(256);
    }

    pub(super) fn finish_sweep(&mut self, reclaimable: &[StyleAtomID]) -> Vec<ReclaimedStyleAtom> {
        if reclaimable.is_empty() {
            self.schedule_next_sweep();
            return Vec::new();
        }
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

        self.schedule_next_sweep();
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
}

/// A fork of a document holds its own document reference to each atom the document holds.
impl Clone for DocumentAtoms {
    fn clone(&self) -> Self {
        if matches!(self.scope, AtomScope::Process(_)) {
            let mut global = global_atoms()
                .lock()
                .expect("process-global style atom lock is poisoned");
            for (&key, &atom) in &self.qualified {
                global.reference_qualified(key, atom);
            }
            for (&raw, &atom) in &self.raw {
                global.reference_raw(raw, atom);
            }
        }
        Self {
            raw: self.raw.clone(),
            cpp_memoized_raws: self.cpp_memoized_raws.clone(),
            qualified: self.qualified.clone(),
            scope: self.scope,
            published: self.published.clone(),
            #[cfg(test)]
            available: self.available.clone(),
            #[cfg(test)]
            next: self.next.clone(),
            sweep_at: self.sweep_at,
        }
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
            let mut first = DocumentAtoms::for_test();
            let mut second = DocumentAtoms::for_test();
            first_atom = first.intern_raw(0x1234);
            assert_eq!(second.intern_raw(0x1234), first_atom);
            assert_ne!(second.intern_raw(0x5678), first_atom);
        }
        let mut later = DocumentAtoms::for_test();
        assert_eq!(later.intern_raw(0x9abc), first_atom);
    }

    #[test]
    fn process_atoms_wait_for_every_raw_and_qualified_owner() {
        let _test_lock = GLOBAL_ATOM_TEST_LOCK.lock().unwrap();
        let mut first = DocumentAtoms::for_test();
        let mut second = DocumentAtoms::for_test();
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
        let mut later = DocumentAtoms::for_test();
        assert_eq!(later.intern_raw(0x3000), namespace);
        assert_eq!(later.intern_raw(0x4000), name);
        assert_eq!(later.intern_qualified(namespace, name), qualified);
    }

    #[test]
    fn qualified_atoms_share_the_global_identity_space() {
        let _test_lock = GLOBAL_ATOM_TEST_LOCK.lock().unwrap();
        let mut first = DocumentAtoms::for_test();
        let mut second = DocumentAtoms::for_test();
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
    fn a_published_name_survives_a_sweep_that_no_other_owner_reaches() {
        let mut atoms = DocumentAtoms::for_live_engine();
        let referenced = atoms.intern_cpp_raw(0x1000);
        let other = atoms.intern_cpp_raw(0x2000);
        atoms.retain_published(referenced);
        assert_eq!(prepare_sweep(&atoms, &mut HashSet::new()), [other]);

        // The publication is replaced by one naming a different name, so the old one becomes
        // reclaimable and its number can be issued again.
        let replacement = atoms.intern_cpp_raw(0x3000);
        atoms.retain_published(replacement);
        atoms.release_published(referenced);
        let reclaimable = prepare_sweep(&atoms, &mut HashSet::new());
        assert_eq!(reclaimable, [referenced, other]);
        atoms.finish_sweep(&reclaimable);
        assert_eq!(atoms.intern_raw(0x4000), referenced);
    }

    #[test]
    fn two_publications_of_one_name_each_keep_it() {
        let mut atoms = DocumentAtoms::for_live_engine();
        let referenced = atoms.intern_cpp_raw(0x1000);
        atoms.retain_published(referenced);
        atoms.retain_published(referenced);
        atoms.release_published(referenced);
        assert!(prepare_sweep(&atoms, &mut HashSet::new()).is_empty());
        atoms.release_published(referenced);
        assert_eq!(prepare_sweep(&atoms, &mut HashSet::new()), [referenced]);
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
