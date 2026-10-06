/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The atoms a document's host interned for its engine, and what it worked out of the attribute names among them. The
//! host asks again for every attribute name and value it publishes, so a name it interned before is answered on the
//! host's own thread, without a lock or a write to the engine: one hash lookup on the name's raw identity.

use super::atoms::{AtomLease, ReclaimedStyleAtom};
use super::engine_calls::{EngineWrite, with_engine};
use super::index::{AttributeNameForms, StyleAtomID};
use crate::css::ffi_support::{FfiUtf16View, ascii_lowercase};
use crate::css::parser::arbitrary_substitution::attr_may_read_name;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::fast_hash::{FastMap, FastSet};
use crate::render_state::{ArenaChange, BegunRead, DocumentHost};

/// What the host interned for its document's engine.
#[derive(Default)]
pub(crate) struct HostAtoms {
    /// The atom of each name the host interned, by the name's raw identity, with a reference to the name: while the
    /// atom is here, no other string can take the identity.
    names: FastMap<usize, (StyleAtomID, RetainedUtf16FlyString)>,
    /// How many times the engine reclaimed atoms, which a cache the host keys by atoms is only good for one of.
    generation: u64,
    /// The languages whose tag the engine was given.
    languages_with_text: FastSet<StyleAtomID>,
    /// The name each attribute is published under, by its local name and its namespace, none for no namespace.
    attribute_names: FastMap<(StyleAtomID, StyleAtomID), StyleAtomID>,
    /// What the host published of each attribute name.
    published_attribute_names: FastMap<StyleAtomID, PublishedAttributeName>,
    /// The attribute names the host knows nothing reads the value text of, as of the engine's requirements numbered
    /// `value_text_requirements`.
    names_with_unread_value_text: FastSet<StyleAtomID>,
    value_text_requirements: u64,
}

/// The other names an attribute name answers to, and the local name an `attr()` reads it by where it is in no namespace.
struct PublishedAttributeName {
    forms: AttributeNameForms,
    substitution_name: Option<Box<[u16]>>,
}

impl HostAtoms {
    /// Forgets the atoms the engine reclaimed, whose numbers may name other names from now on.
    pub(crate) fn forget(&mut self, reclaimed: &[ReclaimedStyleAtom]) {
        if reclaimed.is_empty() {
            return;
        }
        for reclaimed in reclaimed {
            // The engine names the raw identity of each name the host interned.
            if reclaimed.raw != 0 {
                let forgotten = self.names.remove(&reclaimed.raw);
                debug_assert_eq!(forgotten.map(|(atom, _)| atom), Some(reclaimed.atom));
            }
        }
        let reclaimed: FastSet<StyleAtomID> = reclaimed.iter().map(|reclaimed| reclaimed.atom).collect();
        self.languages_with_text
            .retain(|language| !reclaimed.contains(language));
        self.attribute_names
            .retain(|&(local, namespace), name| ![local, namespace, *name].iter().any(|atom| reclaimed.contains(atom)));
        self.published_attribute_names
            .retain(|name, _| !reclaimed.contains(name));
        self.names_with_unread_value_text
            .retain(|name| !reclaimed.contains(name));
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
fn intern_qualified(host: &DocumentHost, namespace: StyleAtomID, name: StyleAtomID) -> StyleAtomID {
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

/// Interns the name an attribute in `namespace` called `local_name` is published under, and has the engine know the
/// other names it answers to.
///
/// Three selectors ask three different questions of an attribute called `x`. `[ns|x]` reaches only the one in that
/// namespace, `[x]` reaches only the one in no namespace - which is what the bare local name is - and `[*|x]` reaches
/// whichever of them the element carries. The first two name exactly one of an element's attributes, so they are the
/// key: an element can hold `x` in several namespaces at once, and each is a fact with its own value. `[*|x]` asks
/// about all of them together, so the shared form is published as an identity of the name rather than as a fact of
/// its own, and one entry per attribute answers all three. Demand expansion revisits every live attribute, so the
/// forms are worked out once per name.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, `local_name` the raw identity of a live
/// `AK::Utf16FlyString`, and `namespace` that of a live non-empty one, or zero for no namespace.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_intern_attribute_name(
    host: &DocumentHost,
    local_name: usize,
    namespace: usize,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let local = unsafe { intern(host, local_name) };
    // SAFETY: Guaranteed by the caller.
    let namespace = if namespace == 0 {
        StyleAtomID::NONE
    } else {
        unsafe { intern(host, namespace) }
    };
    if let Some(name) = host
        .engine_memo()
        .atoms
        .borrow()
        .attribute_names
        .get(&(local, namespace))
    {
        return name.0;
    }
    let in_namespace = |name| {
        if namespace.is_none() {
            name
        } else {
            intern_qualified(host, namespace, name)
        }
    };
    let name = in_namespace(local);
    // `[*|x]` names an attribute in any namespace, which no interned namespace is: none keys it.
    let mut forms = AttributeNameForms {
        local: intern_qualified(host, StyleAtomID::NONE, local),
        ..AttributeNameForms::default()
    };
    // SAFETY: Guaranteed by the caller.
    let local_name: Box<[u16]> = match unsafe { ak::utf16_string_units(&local_name) } {
        ak::Utf16StringUnits::Ascii(units) => units.iter().map(|&unit| u16::from(unit)).collect(),
        ak::Utf16StringUnits::Utf16(units) => units.into(),
    };
    if local_name.iter().any(|&unit| ascii_lowercase(unit) != unit) {
        let folded =
            ak::Utf16FlyString::from_utf16(&local_name.iter().copied().map(ascii_lowercase).collect::<Vec<_>>());
        // SAFETY: The folded name is live for the call.
        let folded_local = unsafe { intern(host, folded.raw_identity()) };
        forms.folded_name = in_namespace(folded_local);
        forms.folded_local = intern_qualified(host, StyleAtomID::NONE, folded_local);
    }
    // An attr() reads an attribute in no namespace by its local name.
    let substitution_name = namespace.is_none().then_some(local_name);
    host.queue_change(ArenaChange::Engine(EngineWrite::AttributeNameForms {
        name,
        forms,
        substitution_name: substitution_name.clone(),
    }));
    let mut atoms = host.engine_memo().atoms.borrow_mut();
    atoms.published_attribute_names.insert(
        name,
        PublishedAttributeName {
            forms,
            substitution_name,
        },
    );
    atoms.attribute_names.insert((local, namespace), name);
    name.0
}

/// Interns the value of an attribute published under `name` whose raw identity is `value`, and hands the engine what
/// it spells unless the host knows that nothing reads the value text of `name`. The engine keeps the text only where
/// something reads it.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, `name` an attribute name the host interned, and
/// `value` the raw identity of a live `AK::Utf16FlyString`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_intern_attribute_value(host: &DocumentHost, name: u32, value: usize) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let atom = unsafe { intern(host, value) };
    let name = StyleAtomID(name);
    if !value_text_is_known_unread(host, name) {
        // SAFETY: Guaranteed by the caller.
        let text = unsafe { RetainedUtf16FlyString::from_borrowed_raw(value) };
        host.queue_change(ArenaChange::Engine(EngineWrite::AttributeValueText {
            name,
            value: atom,
            text,
        }));
    }
    atom.0
}

/// Whether the host knows that nothing reads what the values of the attribute name `name` spell, which spares it the
/// value's text.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_attribute_value_text_is_known_unread(host: &DocumentHost, name: u32) -> bool {
    value_text_is_known_unread(host, StyleAtomID(name))
}

/// Whether the host knows that nothing reads what the values of the attribute name `name` spell: no selector of the
/// engine, by `name` or the other forms the host published with it, and no `attr()`, by its local name if it has one.
/// The host knows what the selectors read where it queued no rule since its last job; what `attr()`s read is the
/// process's. Where it does not know, the host hands the engine each value's text, which the engine keeps only where
/// something reads it.
fn value_text_is_known_unread(host: &DocumentHost, name: StyleAtomID) -> bool {
    let atoms = host.engine_memo().atoms.borrow();
    if atoms.names_with_unread_value_text.contains(&name) {
        return true;
    }
    // The host interned every name it asks about, with its forms. Where it knows none, it hands the text over.
    let Some(PublishedAttributeName {
        forms,
        substitution_name,
    }) = atoms.published_attribute_names.get(&name)
    else {
        return false;
    };
    let unread = !substitution_name.as_deref().is_some_and(attr_may_read_name)
        && host.known_selectors_read_value_text_of(&[name, forms.local, forms.folded_name, forms.folded_local])
            == Some(false);
    drop(atoms);
    if unread {
        host.engine_memo()
            .atoms
            .borrow_mut()
            .names_with_unread_value_text
            .insert(name);
    }
    unread
}

/// Whether the engine's requirements of attribute value text moved since the host last asked, which makes the host ask
/// again about the names it knew nothing read.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_refresh_attribute_value_text_requirements(
    host: &DocumentHost,
    read: &BegunRead,
) -> bool {
    // The host knows where the requirements are where it queued no rule since its last job.
    let requirements = match host.known_selector_attribute_value_text_requirements_version() {
        Some(version) => super::inputs::with_attr_names_read(version),
        None => with_engine(read, host, |engine| engine.attribute_value_text_requirements_version()),
    };
    let mut atoms = host.engine_memo().atoms.borrow_mut();
    if requirements == atoms.value_text_requirements {
        return false;
    }
    atoms.value_text_requirements = requirements;
    atoms.names_with_unread_value_text.clear();
    true
}
