/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The custom-property stores behind the environments records are published with.
//!
//! An environment identity names what an element's custom properties resolve to. The store is the
//! resolved chain itself: what a descendant's own custom properties cascade over and what its
//! `var()` references read. C++ hands the store over with each publication, and the engine keeps
//! it for as long as a record names the environment, so an engine-computed environment can build
//! on any parent's.

use std::collections::hash_map::Entry;
use std::ffi::c_void;
use std::sync::Arc;

use super::fast_hash::FastMap as HashMap;
use super::fast_hash::FastSet as HashSet;
use super::index::StyleAtomID;
use crate::css::custom_properties::{CustomPropertyRegistry, CustomPropertyStore};
use crate::css::retained_fly_string::RetainedUtf16FlyString;

/// The identity of the environment `store` is: its address. A store names one environment, and the catalogs keep it
/// alive while they name it, so no other store takes its address while the identity is in use.
pub(super) fn environment_identity(store: &Arc<CustomPropertyStore>) -> u64 {
    Arc::as_ptr(store) as usize as u64
}

/// The store behind an environment, and for one the engine resolved, the environment it was resolved over.
#[derive(Clone)]
struct Environment {
    store: Arc<CustomPropertyStore>,
    /// `None` for an environment C++ published.
    parent: Option<ResolvedOver>,
}

/// The environment one the engine resolved was resolved over, whose store it holds, so that no other store takes the
/// address that identifies it for as long as this names it.
#[derive(Clone)]
struct ResolvedOver {
    identity: u64,
    _store: Option<Arc<CustomPropertyStore>>,
}

/// A strong reference to the custom-property store `store` names, if it names one.
///
/// # Safety
/// `store` must be null or a live raw `Arc` pointer to a `CustomPropertyStore`.
pub(super) unsafe fn retained_store(store: *const c_void) -> Option<Arc<CustomPropertyStore>> {
    if store.is_null() {
        return None;
    }
    let store = store.cast::<CustomPropertyStore>();
    // SAFETY: Guaranteed by the caller.
    unsafe {
        Arc::increment_strong_count(store);
        Some(Arc::from_raw(store))
    }
}

/// The name catalog's own reference to a custom property's fly string, held so the raw word that
/// keys it stays a unique identity for as long as the engine names the property.
#[derive(Clone)]
pub(super) struct SharedUtf16FlyString(RetainedUtf16FlyString);

// SAFETY: An `AK::Utf16String` reference count is atomic (`AK/Rust/src/lib.rs`
// `reference_utf16_string` / `release_utf16_string_with`), so taking and giving up a reference
// from a worker is not a race; only the *final* release is not, because it calls back into C++ to
// destroy the string and unregister it from the fly-string table. This handle is what makes that
// final release impossible off the engine's thread: it is one reference per name atom, taken in
// `note_name` before the transaction and given up only in `forget_names`, at a boundary. A walk
// reaches it through `&RetainedState` and reads `raw()`, a plain word; it can neither move nor
// drop the handle, so it can never hold the last reference. That is `Sync` and deliberately not
// `Send`.
unsafe impl Sync for SharedUtf16FlyString {}

impl SharedUtf16FlyString {
    pub(super) fn raw(&self) -> usize {
        self.0.raw()
    }

    /// What the name spells. The catalog's reference keeps the string alive and its text never changes, so a worker
    /// may read it.
    pub(super) fn to_utf16(&self) -> std::borrow::Cow<'_, [u16]> {
        self.0.to_utf16()
    }

    /// Answers `read` of the fly string the handle holds, borrowed: no reference is taken or given up.
    pub(super) fn with_fly_string<R>(&self, read: impl FnOnce(&ak::Utf16FlyString) -> R) -> R {
        // SAFETY: The handle's reference keeps the string alive for the call, and the borrowed owner is never
        //         dropped, so it gives up no reference.
        let string = std::mem::ManuallyDrop::new(unsafe { ak::Utf16FlyString::from_raw_owned(self.0.raw()) });
        read(&string)
    }
}

/// The fly string a custom property's name atom is: a store names its entries by the fly string, and a `var()`
/// reference names one by its text, which the fly string spells.
pub(super) type CustomPropertyName = SharedUtf16FlyString;

#[derive(Clone, Default)]
pub(super) struct CustomPropertyEnvironments {
    /// Every environment a record names, whether C++ published it or the engine resolved it.
    environments: HashMap<u64, Environment>,
    names: HashMap<StyleAtomID, CustomPropertyName>,
    /// The environment a child inherits from one holding custom properties a registration keeps
    /// from inheriting, for each such environment; any other environment hands itself down.
    inheritable: HashMap<u64, u64>,
    /// The environments `inheritable` hands down, gathered by `retain_only`, which keeps the set to
    /// reuse its allocation.
    handed_down: HashSet<u64>,
}

impl CustomPropertyEnvironments {
    /// The bytes of the catalogs and the stores they hold, each store counted once.
    pub(super) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::{HeapSize, map_bytes, set_bytes};
        map_bytes(&self.environments)
            + self
                .environments
                .values()
                .map(|environment| environment.store.heap_size(sizer))
                .sum::<usize>()
            + map_bytes(&self.names)
            + map_bytes(&self.inheritable)
            + set_bytes(&self.handed_down)
    }

    /// Record what a custom property's name atom spells. A name is noted once; the text of an
    /// atom never changes.
    ///
    /// # Safety
    /// `raw` must be zero or a live `AK::Utf16FlyString` raw representation.
    pub(super) unsafe fn note_name(&mut self, name: StyleAtomID, raw: usize) -> bool {
        if name.is_none() {
            return false;
        }
        let Entry::Vacant(entry) = self.names.entry(name) else {
            return false;
        };
        entry.insert(SharedUtf16FlyString(unsafe {
            RetainedUtf16FlyString::from_borrowed_raw(raw)
        }));
        true
    }

    pub(super) fn name(&self, name: StyleAtomID) -> Option<&CustomPropertyName> {
        self.names.get(&name)
    }

    /// Forget names whose atom identities are about to become available for reuse.
    pub(super) fn forget_names(&mut self, names: &[StyleAtomID]) {
        for name in names {
            self.names.remove(name);
        }
    }

    /// Keep the store behind an environment while a record names it. The first publication of an
    /// identity decides its store; an identity never changes what it resolves to.
    pub(super) fn retain(&mut self, identity: u64, store: Option<&Arc<CustomPropertyStore>>) {
        let Some(store) = store.filter(|_| identity != 0) else {
            return;
        };
        self.environments.entry(identity).or_insert_with(|| Environment {
            store: Arc::clone(store),
            parent: None,
        });
    }

    /// [`Self::retain`], for a store C++ names by its raw `Arc` pointer.
    ///
    /// # Safety
    /// `store` must be null or a live raw `Arc` pointer to a `CustomPropertyStore`.
    pub(super) unsafe fn retain_raw(&mut self, identity: u64, store: *const c_void) {
        if identity == 0 || store.is_null() {
            return;
        }
        self.environments.entry(identity).or_insert_with(|| Environment {
            store: unsafe { retained_store(store) }.expect("an environment names a store"),
            parent: None,
        });
    }

    /// The store behind an environment, whether C++ published it or the engine resolved it.
    pub(super) fn store(&self, identity: u64) -> Option<&Arc<CustomPropertyStore>> {
        if identity == 0 {
            return None;
        }
        self.environments.get(&identity).map(|environment| &environment.store)
    }

    /// Keep the environment a child inherits from `identity`, which C++ made without the custom
    /// properties a registration keeps from inheriting.
    pub(super) fn note_inheritable(
        &mut self,
        identity: u64,
        inheritable: u64,
        store: Option<&Arc<CustomPropertyStore>>,
    ) {
        if identity == 0 {
            return;
        }
        // An environment that hands itself down may have handed down a filtered one before.
        if identity == inheritable {
            self.inheritable.remove(&identity);
            return;
        }
        self.retain(inheritable, store);
        self.inheritable.insert(identity, inheritable);
    }

    /// The environment a child inherits from `identity`.
    pub(super) fn inheritable(&self, identity: u64) -> u64 {
        self.inheritable.get(&identity).copied().unwrap_or(identity)
    }

    /// Whether the engine resolved the environment `identity` names, rather than C++.
    pub(super) fn is_engine_made(&self, identity: u64) -> bool {
        self.environments
            .get(&identity)
            .is_some_and(|environment| environment.parent.is_some())
    }

    /// An engine-resolved environment's store and the environment it was resolved over.
    pub(super) fn engine_environment(&self, identity: u64) -> Option<(&Arc<CustomPropertyStore>, u64)> {
        let environment = self.environments.get(&identity)?;
        Some((&environment.store, environment.parent.as_ref()?.identity))
    }

    /// Take over a store the engine resolved over `parent`, an environment children inherit, and
    /// give it an identity of the engine's own. What its children inherit is `handed_down`, the
    /// store without the custom properties `registry` keeps from inheriting, which the record
    /// holding `store` holds where it holds one: `parent` again when nothing else is left that
    /// `parent` does not hold.
    pub(super) fn adopt_engine_environment(
        &mut self,
        store: Arc<CustomPropertyStore>,
        handed_down: Option<&Arc<CustomPropertyStore>>,
        parent: u64,
        parent_store: Option<&CustomPropertyStore>,
        registry: &CustomPropertyRegistry,
    ) -> u64 {
        let inheritable = match handed_down {
            Some(handed_down) if Arc::ptr_eq(handed_down, &store) => None,
            Some(handed_down) => Some(handed_down.clone()),
            None => store.inheritable(registry),
        };
        let identity = self.mint(store, parent);
        if let Some(inheritable) = inheritable {
            let parent_holds_every_value = inheritable.own_values.iter().all(|(name, entry)| {
                parent_store.is_some_and(|parent| parent.value_is_identical(name, entry.value.pointer().cast()))
            });
            let inheritable = if parent_holds_every_value {
                parent
            } else {
                self.mint(inheritable, parent)
            };
            self.inheritable.insert(identity, inheritable);
        }
        identity
    }

    /// NB: A store resolved again over another environment, as one an element keeps where what
    ///     its parent hands down moved but its values did not, names the environment it was
    ///     resolved over last.
    fn mint(&mut self, store: Arc<CustomPropertyStore>, parent: u64) -> u64 {
        let identity = environment_identity(&store);
        let parent_store = self.environments.get(&parent).map(|parent| parent.store.clone());
        self.environments.insert(
            identity,
            Environment {
                store,
                parent: Some(ResolvedOver {
                    identity: parent,
                    _store: parent_store,
                }),
            },
        );
        identity
    }

    /// Release the stores of environments no record names any more, and forget what resolved to
    /// them.
    /// What a live environment hands its children stays as long as it does.
    pub(super) fn retain_only(&mut self, is_live: impl Fn(u64) -> bool) {
        self.inheritable.retain(|&identity, _| is_live(identity));
        let mut handed_down = std::mem::take(&mut self.handed_down);
        handed_down.clear();
        handed_down.extend(self.inheritable.values());
        let is_live = |identity| is_live(identity) || handed_down.contains(&identity);
        self.environments.retain(|&identity, _| is_live(identity));
        self.handed_down = handed_down;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nested_capacity_tracks_names() {
        let mut environments = CustomPropertyEnvironments::default();
        // SAFETY: A zero raw representation carries no borrowed fly string.
        unsafe {
            assert!(environments.note_name(StyleAtomID(1), 0));
            assert!(!environments.note_name(StyleAtomID(1), 0));
        }
        environments.forget_names(&[StyleAtomID(1), StyleAtomID(1)]);
        environments.retain_only(|_| false);
    }
}
