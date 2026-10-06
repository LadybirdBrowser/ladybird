/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The atoms a document's host interned for its engine. The host asks again for every attribute name and value it
//! publishes, so a name it interned before is answered on the host's own thread, without a lock or a write to the
//! engine: one hash lookup on the name's raw identity.

use super::atoms::AtomLease;
use super::bridge::FfiReclaimedStyleAtom;
use super::engine_calls::EngineWrite;
use super::index::StyleAtomID;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::fast_hash::{FastMap, FastSet};
use crate::render_state::{ArenaChange, DocumentHost};

/// What the host interned for its document's engine.
#[derive(Default)]
pub(crate) struct HostAtoms {
    /// The atom of each name the host interned, by the name's raw identity, with a reference to the name: while the
    /// atom is here, no other string can take the identity.
    names: FastMap<usize, (StyleAtomID, RetainedUtf16FlyString)>,
    /// How many times the engine reclaimed atoms. What the host keys by atoms is good for one of them.
    generation: u64,
    /// The languages whose tag the engine was given.
    languages_with_text: FastSet<StyleAtomID>,
}

impl HostAtoms {
    /// Forgets the atoms the engine reclaimed, whose numbers may name other names from now on.
    pub(crate) fn forget(&mut self, reclaimed: &[FfiReclaimedStyleAtom]) {
        if reclaimed.is_empty() {
            return;
        }
        for reclaimed in reclaimed {
            let atom = StyleAtomID(reclaimed.atom);
            self.languages_with_text.remove(&atom);
            // The engine names the raw identity of each name the host interned.
            if reclaimed.raw != 0 {
                let forgotten = self.names.remove(&reclaimed.raw);
                debug_assert_eq!(forgotten.map(|(atom, _)| atom), Some(atom));
            }
        }
        self.generation += 1;
    }
}

/// The atom of the name whose raw identity is `raw`: the one the host interned it as, or else a reference the host
/// takes to the name's process-global atom, which the engine adopts as it applies the host's writes.
///
/// # Safety
/// `raw` must be the raw identity of a live `AK::Utf16FlyString`.
pub(crate) unsafe fn intern(host: &DocumentHost, raw: usize) -> StyleAtomID {
    let atoms = &host.engine_memo().atoms;
    if let Some(&(atom, _)) = atoms.borrow().names.get(&raw) {
        return atom;
    }
    // SAFETY: Guaranteed by the caller.
    let (lease, name) = unsafe {
        (
            AtomLease::acquire_raw(raw),
            RetainedUtf16FlyString::from_borrowed_raw(raw),
        )
    };
    let atom = adopt(host, lease);
    atoms.borrow_mut().names.insert(raw, (atom, name));
    atom
}

/// The atom of `name` qualified by `namespace`, which the engine adopts as it does a name's.
pub(crate) fn intern_qualified(host: &DocumentHost, namespace: StyleAtomID, name: StyleAtomID) -> StyleAtomID {
    adopt(host, AtomLease::acquire_qualified(namespace, name))
}

/// Queues the engine's adoption of the atom `lease` holds, and answers the atom.
fn adopt(host: &DocumentHost, lease: AtomLease) -> StyleAtomID {
    let atom = lease.atom();
    host.queue_change(ArenaChange::Engine(EngineWrite::AdoptAtom(lease)));
    atom
}

/// Interns the name whose raw identity is `raw` for the document.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `raw` the raw identity of a live
/// `AK::Utf16FlyString`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_intern_atom(host: &DocumentHost, raw: usize) -> u32 {
    // SAFETY: Guaranteed by the caller.
    unsafe { intern(host, raw) }.0
}

/// How many times the engine reclaimed atoms the host interned, which tells apart what the host keyed by the atoms
/// before and after.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_atom_generation(host: &DocumentHost) -> u64 {
    host.engine_memo().atoms.borrow().generation
}

/// Records the language the element `node` resolves to, or none for no element. The first time the host names a
/// language, the engine is also given its tag: a range is not a name, so `:lang()` compares against the tag itself.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `text` must name readable code units.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_language(
    host: &DocumentHost,
    node: u32,
    language: u32,
    text: FfiUtf16View,
) {
    let tag_is_new = language != 0
        && text.length != 0
        && host
            .engine_memo()
            .atoms
            .borrow_mut()
            .languages_with_text
            .insert(StyleAtomID(language));
    // SAFETY: Guaranteed by the caller.
    let text: Box<[u16]> = if tag_is_new {
        unsafe { text.to_utf16() }.unwrap_or_default().into()
    } else {
        Box::default()
    };
    if node != 0 || !text.is_empty() {
        host.queue_change(ArenaChange::Engine(EngineWrite::ElementLanguage {
            node,
            language,
            text,
        }));
    }
}
