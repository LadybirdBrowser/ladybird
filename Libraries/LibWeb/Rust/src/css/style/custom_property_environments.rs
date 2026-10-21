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
use crate::css::style_value::RetainedStyleValueData;

/// How many substituted values the engine keeps before starting over.
const SUBSTITUTION_MEMO_LIMIT: usize = 1 << 16;

/// The bit that marks an environment identity as one the engine minted. C++ mints its own from a
/// counter; the two never collide.
pub(super) const ENGINE_ENVIRONMENT_IDENTITY_BIT: u64 = 1 << 62;

/// One custom property an element's cascade decided: its name, its importance, and the value it
/// was written with, by identity. Two spellings of one value can serialize differently, so the
/// written value is what an environment resolves from and what names it.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) struct CascadedCustomProperty {
    pub(super) name: StyleAtomID,
    pub(super) important: bool,
    pub(super) written_value: usize,
}

/// What decides an engine-computed environment: the environment inherited, the registrations in
/// force, and the custom properties the element's cascade decided, in cascade order. Two elements
/// alike in these resolve alike.
#[derive(Clone, PartialEq, Eq, Hash)]
pub(super) struct EnvironmentInputs {
    pub(super) parent: u64,
    pub(super) registration_generation: u64,
    pub(super) cascaded: Box<[CascadedCustomProperty]>,
}

/// An environment the engine resolved: its store, and the environment it was resolved over.
#[derive(Clone)]
struct EngineEnvironment {
    store: RetainedCustomPropertyStore,
    parent: u64,
}

/// An environment memo entry and the written values whose addresses name its key. Keeping those
/// values alive prevents later declarations from being allocated at the same addresses.
#[derive(Clone)]
struct MemoizedEnvironment {
    identity: u64,
    _written_values: Box<[RetainedStyleValueData]>,
}

/// One substituted value and the written value whose identity names it. Keeping the written value
/// alive prevents a later declaration from being allocated at the same address as the key.
#[derive(Clone)]
struct MemoizedSubstitution {
    written_value: RetainedStyleValueData,
    value: RetainedStyleValueData,
}

/// One strong reference to a custom-property store, released with the handle.
#[derive(Clone)]
pub(crate) struct RetainedCustomPropertyStore(Arc<CustomPropertyStore>);

impl RetainedCustomPropertyStore {
    /// # Safety
    /// `store` must be a live raw `Arc` pointer to a `CustomPropertyStore`.
    pub(super) unsafe fn from_borrowed(store: *const c_void) -> Self {
        let store = store.cast::<CustomPropertyStore>();
        // SAFETY: Guaranteed by the caller.
        unsafe {
            Arc::increment_strong_count(store);
            Self(Arc::from_raw(store))
        }
    }

    pub(super) fn store(&self) -> &Arc<CustomPropertyStore> {
        &self.0
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
}

/// What a custom property's name atom spells, and the fly string it is: a store names its entries
/// by the fly string, and a `var()` reference names one by its text.
#[derive(Clone)]
pub(super) struct CustomPropertyName {
    pub(super) raw: SharedUtf16FlyString,
    pub(super) text: Arc<[u16]>,
}

#[derive(Clone, Default)]
pub(super) struct CustomPropertyEnvironments {
    stores: HashMap<u64, RetainedCustomPropertyStore>,
    names: HashMap<StyleAtomID, CustomPropertyName>,
    engine: HashMap<u64, EngineEnvironment>,
    /// The environment a child inherits from one holding custom properties a registration keeps
    /// from inheriting, for each such environment; any other environment hands itself down.
    inheritable: HashMap<u64, u64>,
    /// The environments `inheritable` hands down, gathered by `retain_only`, which keeps the set to
    /// reuse its allocation.
    handed_down: HashSet<u64>,
    /// The environments holding a registered value resolved against the viewport.
    reads_viewport: HashSet<u64>,
    /// What inputs resolved to which environment, so an element alike in its inputs takes the
    /// environment an earlier one got.
    memo: HashMap<EnvironmentInputs, MemoizedEnvironment>,
    /// The environment an animation overlay's values laid over another base make, by the overlay
    /// and that base.
    overlays_moved: HashMap<(u64, u64), u64>,
    minted: u64,
    /// What a written value substitutes to for a property under an environment, by the written
    /// value's identity: the same declaration under the same environment substitutes alike.
    substitutions: HashMap<(usize, u16, u64), MemoizedSubstitution>,
}

impl CustomPropertyEnvironments {
    /// Record what a custom property's name atom spells. A name is noted once; the text of an
    /// atom never changes.
    ///
    /// # Safety
    /// `raw` must be zero or a live `AK::Utf16FlyString` raw representation.
    pub(super) unsafe fn note_name(&mut self, name: StyleAtomID, raw: usize, text: &[u16]) -> bool {
        if name.is_none() {
            return false;
        }
        let Entry::Vacant(entry) = self.names.entry(name) else {
            return false;
        };
        let text: Arc<[u16]> = text.into();
        entry.insert(CustomPropertyName {
            raw: SharedUtf16FlyString(unsafe { RetainedUtf16FlyString::from_borrowed_raw(raw) }),
            text,
        });
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
        self.stores
            .entry(identity)
            .or_insert_with(|| RetainedCustomPropertyStore(Arc::clone(store)));
    }

    /// [`Self::retain`], for a store C++ names by its raw `Arc` pointer.
    ///
    /// # Safety
    /// `store` must be null or a live raw `Arc` pointer to a `CustomPropertyStore`.
    pub(super) unsafe fn retain_raw(&mut self, identity: u64, store: *const c_void) {
        if identity == 0 || store.is_null() {
            return;
        }
        self.stores
            .entry(identity)
            .or_insert_with(|| unsafe { RetainedCustomPropertyStore::from_borrowed(store) });
    }

    /// The store behind an environment, whether C++ published it or the engine resolved it.
    pub(super) fn store(&self, identity: u64) -> Option<&Arc<CustomPropertyStore>> {
        if identity == 0 {
            return None;
        }
        self.stores
            .get(&identity)
            .map(RetainedCustomPropertyStore::store)
            .or_else(|| self.engine.get(&identity).map(|environment| environment.store.store()))
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

    /// The values `overlay`, an element's animation overlay, sampled over another environment, laid
    /// over `base`, which the element's declarations resolve to now.
    pub(super) fn overlay_moved_over(&mut self, overlay: u64, base: u64) -> u64 {
        if let Some(&identity) = self.overlays_moved.get(&(overlay, base)) {
            return identity;
        }
        let overlay_store = self.store(overlay);
        debug_assert!(overlay_store.is_some(), "an element's overlay was noted with its store");
        let Some(overlay_store) = overlay_store else {
            return base;
        };
        let store = CustomPropertyStore::animated_values_over(overlay_store, self.store(base).map(Arc::as_ref));
        let identity = self.mint(Arc::new(store), base);
        self.overlays_moved.insert((overlay, base), identity);
        identity
    }

    /// The environment a child inherits from `identity`.
    pub(super) fn inheritable(&self, identity: u64) -> u64 {
        self.inheritable.get(&identity).copied().unwrap_or(identity)
    }

    /// Note that an environment holds a registered value resolved against the viewport.
    pub(super) fn note_reads_viewport(&mut self, identity: u64) {
        if identity != 0 {
            self.reads_viewport.insert(identity);
        }
    }

    /// Whether an environment holds a registered value resolved against the viewport.
    pub(super) fn reads_viewport(&self, identity: u64) -> bool {
        self.reads_viewport.contains(&identity)
    }

    /// An engine-resolved environment's store and the environment it was resolved over.
    pub(super) fn engine_environment(&self, identity: u64) -> Option<(&Arc<CustomPropertyStore>, u64)> {
        self.engine
            .get(&identity)
            .map(|environment| (environment.store.store(), environment.parent))
    }

    pub(super) fn memoized(&self, inputs: &EnvironmentInputs) -> Option<u64> {
        self.memo.get(inputs).map(|environment| environment.identity)
    }

    /// Remember what inputs resolved to, which may be the inherited environment itself.
    pub(super) fn remember(
        &mut self,
        inputs: EnvironmentInputs,
        identity: u64,
        written_values: Vec<RetainedStyleValueData>,
    ) {
        let environment = MemoizedEnvironment {
            identity,
            _written_values: written_values.into_boxed_slice(),
        };
        match self.memo.entry(inputs) {
            Entry::Occupied(mut entry) => {
                entry.insert(environment);
            }
            Entry::Vacant(entry) => {
                entry.insert(environment);
            }
        }
    }

    /// Take over a store the engine resolved over `parent`, an environment children inherit, and
    /// give it an identity of the engine's own. What its children inherit is the store without
    /// the custom properties `registry` keeps from inheriting: `parent` again when nothing else
    /// is left that `parent` does not hold.
    pub(super) fn adopt_engine_environment(
        &mut self,
        store: Arc<CustomPropertyStore>,
        parent: u64,
        parent_store: Option<&CustomPropertyStore>,
        registry: &CustomPropertyRegistry,
    ) -> u64 {
        let inheritable = store.inheritable(registry);
        let identity = self.mint(store, parent);
        if let Some(inheritable) = inheritable {
            let parent_holds_every_value = inheritable.own_values.iter().all(|(&name, entry)| {
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

    fn mint(&mut self, store: Arc<CustomPropertyStore>, parent: u64) -> u64 {
        self.minted += 1;
        let identity = ENGINE_ENVIRONMENT_IDENTITY_BIT | self.minted;
        self.engine.insert(
            identity,
            EngineEnvironment {
                store: RetainedCustomPropertyStore(store),
                parent,
            },
        );
        identity
    }

    pub(super) fn substitution(
        &self,
        written: &RetainedStyleValueData,
        property: u16,
        environment: u64,
    ) -> Option<RetainedStyleValueData> {
        self.substitutions
            .get(&(written.pointer() as usize, property, environment))
            .map(|substitution| {
                debug_assert_eq!(substitution.written_value.pointer(), written.pointer());
                substitution.value.clone_retained()
            })
    }

    /// The registrations changed. What a written value substitutes to under an environment depends
    /// on them: forget those. So does what an environment C++ published hands down, which C++
    /// names again for every environment an element holds as the registrations change, and as it
    /// hands any environment over.
    pub(super) fn registrations_changed(&mut self) {
        self.substitutions.clear();
    }

    pub(super) fn remember_substitution(
        &mut self,
        written: RetainedStyleValueData,
        property: u16,
        environment: u64,
        value: RetainedStyleValueData,
    ) {
        if self.substitutions.len() >= SUBSTITUTION_MEMO_LIMIT {
            self.substitutions.clear();
        }
        self.substitutions.insert(
            (written.pointer() as usize, property, environment),
            MemoizedSubstitution {
                written_value: written,
                value,
            },
        );
    }

    /// Release the stores of environments no record names any more, and forget what resolved to
    /// them and what substituted under them.
    /// What a live environment hands its children stays as long as it does.
    pub(super) fn retain_only(&mut self, is_live: impl Fn(u64) -> bool) {
        self.inheritable.retain(|&identity, _| is_live(identity));
        let mut handed_down = std::mem::take(&mut self.handed_down);
        handed_down.clear();
        handed_down.extend(self.inheritable.values());
        let is_live = |identity| is_live(identity) || handed_down.contains(&identity);
        self.stores.retain(|&identity, _| is_live(identity));
        self.engine.retain(|&identity, _| is_live(identity));
        self.reads_viewport.retain(|&identity| is_live(identity));
        self.memo.retain(|inputs, environment| {
            is_live(environment.identity) && (inputs.parent == 0 || is_live(inputs.parent))
        });
        self.substitutions
            .retain(|&(_, _, environment), _| environment == 0 || is_live(environment));
        self.overlays_moved
            .retain(|&(overlay, base), &mut moved| is_live(overlay) && (base == 0 || is_live(base)) && is_live(moved));
        self.handed_down = handed_down;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nested_capacity_tracks_names_and_replaced_environment_entries() {
        let mut environments = CustomPropertyEnvironments::default();
        // SAFETY: A zero raw representation carries no borrowed fly string.
        unsafe {
            assert!(environments.note_name(StyleAtomID(1), 0, &[45, 45, 120]));
            assert!(!environments.note_name(StyleAtomID(1), 0, &[45, 45, 120]));
        }
        let inputs = || EnvironmentInputs {
            parent: 0,
            registration_generation: 1,
            cascaded: Box::new([CascadedCustomProperty {
                name: StyleAtomID(1),
                important: false,
                written_value: 0,
            }]),
        };
        environments.remember(inputs(), 1, Vec::with_capacity(2));
        // Equal keys retain the original key's allocation when replacing the value.
        environments.remember(inputs(), 2, Vec::with_capacity(5));
        environments.forget_names(&[StyleAtomID(1), StyleAtomID(1)]);
        environments.retain_only(|_| false);
    }
}
