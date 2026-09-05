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
use super::index::StyleAtomID;
use crate::css::custom_properties::CustomPropertyStore;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::RetainedUtf16FlyString;

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
    pub(super) cascaded: Vec<CascadedCustomProperty>,
}

/// An environment the engine resolved: its store, and the environment it was resolved over.
struct EngineEnvironment {
    store: RetainedCustomPropertyStore,
    parent: u64,
}

/// An environment memo entry and the written values whose addresses name its key. Keeping those
/// values alive prevents later declarations from being allocated at the same addresses.
struct MemoizedEnvironment {
    identity: u64,
    written_values: Vec<RetainedStyleValueData>,
}

/// One substituted value and the written value whose identity names it. Keeping the written value
/// alive prevents a later declaration from being allocated at the same address as the key.
struct MemoizedSubstitution {
    written_value: RetainedStyleValueData,
    value: RetainedStyleValueData,
}

/// One strong reference to a custom-property store, released with the handle.
pub(super) struct RetainedCustomPropertyStore(*const c_void);

impl RetainedCustomPropertyStore {
    /// # Safety
    /// `store` must be a live raw `Arc` pointer to a `CustomPropertyStore`.
    unsafe fn from_borrowed(store: *const c_void) -> Self {
        unsafe { Arc::increment_strong_count(store.cast::<CustomPropertyStore>()) };
        Self(store)
    }

    /// # Safety
    /// `store` must be a live raw `Arc` pointer whose one strong reference this handle takes over.
    unsafe fn from_transferred(store: *const c_void) -> Self {
        Self(store)
    }

    fn pointer(&self) -> *const c_void {
        self.0
    }
}

impl Drop for RetainedCustomPropertyStore {
    fn drop(&mut self) {
        // SAFETY: The handle owns exactly one strong reference taken at construction.
        unsafe { Arc::decrement_strong_count(self.0.cast::<CustomPropertyStore>()) };
    }
}

/// What a custom property's name atom spells, and the fly string it is: a store names its entries
/// by the fly string, and a `var()` reference names one by its text.
pub(super) struct CustomPropertyName {
    pub(super) raw: RetainedUtf16FlyString,
    pub(super) text: Vec<u16>,
}

#[derive(Default)]
pub(super) struct CustomPropertyEnvironments {
    stores: HashMap<u64, RetainedCustomPropertyStore>,
    names: HashMap<StyleAtomID, CustomPropertyName>,
    engine: HashMap<u64, EngineEnvironment>,
    /// What inputs resolved to which environment, so an element alike in its inputs takes the
    /// environment an earlier one got.
    memo: HashMap<EnvironmentInputs, MemoizedEnvironment>,
    minted: u64,
    /// What a written value substitutes to for a property under an environment, by the written
    /// value's identity: the same declaration under the same environment substitutes alike.
    substitutions: HashMap<(usize, u16, u64), MemoizedSubstitution>,
    /// Storage owned by map entries, maintained when entries change rather than rescanned
    /// on every style publication.
    nested_capacity_bytes: u64,
}

impl CustomPropertyEnvironments {
    /// Record what a custom property's name atom spells. A name is noted once; the text of an
    /// atom never changes.
    ///
    /// # Safety
    /// `raw` must be zero or a live `AK::Utf16FlyString` raw representation.
    pub(super) unsafe fn note_name(&mut self, name: StyleAtomID, raw: usize, text: &[u16]) -> bool {
        if name.is_none() || self.names.contains_key(&name) {
            return false;
        }
        let text = text.to_vec();
        self.nested_capacity_bytes += (text.capacity() * size_of::<u16>()) as u64;
        self.names.insert(
            name,
            CustomPropertyName {
                raw: unsafe { RetainedUtf16FlyString::from_borrowed_raw(raw) },
                text,
            },
        );
        true
    }

    pub(super) fn name(&self, name: StyleAtomID) -> Option<&CustomPropertyName> {
        self.names.get(&name)
    }

    /// Forget names whose atom identities are about to become available for reuse.
    pub(super) fn forget_names(&mut self, names: &[StyleAtomID]) {
        for name in names {
            if let Some(name) = self.names.remove(name) {
                self.nested_capacity_bytes -= (name.text.capacity() * size_of::<u16>()) as u64;
            }
        }
    }

    /// Keep the store behind an environment while a record names it. The first publication of an
    /// identity decides its store; an identity never changes what it resolves to.
    ///
    /// # Safety
    /// `store` must be null or a live raw `Arc` pointer to a `CustomPropertyStore`.
    pub(super) unsafe fn retain(&mut self, identity: u64, store: *const c_void) {
        if identity == 0 || store.is_null() || self.stores.contains_key(&identity) {
            return;
        }
        self.stores
            .insert(identity, unsafe { RetainedCustomPropertyStore::from_borrowed(store) });
    }

    /// The store behind an environment, whether C++ published it or the engine resolved it.
    pub(super) fn store(&self, identity: u64) -> Option<*const c_void> {
        if identity == 0 {
            return None;
        }
        self.stores
            .get(&identity)
            .map(RetainedCustomPropertyStore::pointer)
            .or_else(|| {
                self.engine
                    .get(&identity)
                    .map(|environment| environment.store.pointer())
            })
    }

    /// An engine-resolved environment's store and the environment it was resolved over.
    pub(super) fn engine_environment(&self, identity: u64) -> Option<(*const c_void, u64)> {
        self.engine
            .get(&identity)
            .map(|environment| (environment.store.pointer(), environment.parent))
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
            written_values,
        };
        self.nested_capacity_bytes +=
            (environment.written_values.capacity() * size_of::<RetainedStyleValueData>()) as u64;
        match self.memo.entry(inputs) {
            Entry::Occupied(mut entry) => {
                self.nested_capacity_bytes -=
                    (entry.get().written_values.capacity() * size_of::<RetainedStyleValueData>()) as u64;
                entry.insert(environment);
            }
            Entry::Vacant(entry) => {
                self.nested_capacity_bytes +=
                    (entry.key().cascaded.capacity() * size_of::<CascadedCustomProperty>()) as u64;
                entry.insert(environment);
            }
        }
    }

    /// Take over a store the engine resolved and give it an identity of the engine's own.
    ///
    /// # Safety
    /// `store` must be a live raw `Arc` pointer whose one strong reference is transferred here.
    pub(super) unsafe fn adopt_engine_environment(&mut self, store: *const c_void, parent: u64) -> u64 {
        self.minted += 1;
        let identity = ENGINE_ENVIRONMENT_IDENTITY_BIT | self.minted;
        self.engine.insert(
            identity,
            EngineEnvironment {
                store: unsafe { RetainedCustomPropertyStore::from_transferred(store) },
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

    pub(super) fn remember_substitution(
        &mut self,
        written: &RetainedStyleValueData,
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
                written_value: written.clone_retained(),
                value,
            },
        );
    }

    /// Release the stores of environments no record names any more, and forget what resolved to
    /// them and what substituted under them.
    pub(super) fn retain_only(&mut self, is_live: impl Fn(u64) -> bool) {
        self.stores.retain(|&identity, _| is_live(identity));
        self.engine.retain(|&identity, _| is_live(identity));
        self.memo.retain(|inputs, environment| {
            let retain = is_live(environment.identity) && (inputs.parent == 0 || is_live(inputs.parent));
            if !retain {
                self.nested_capacity_bytes -= (inputs.cascaded.capacity() * size_of::<CascadedCustomProperty>()
                    + environment.written_values.capacity() * size_of::<RetainedStyleValueData>())
                    as u64;
            }
            retain
        });
        self.substitutions
            .retain(|&(_, _, environment), _| environment == 0 || is_live(environment));
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        (self.stores.capacity() * (size_of::<u64>() + size_of::<RetainedCustomPropertyStore>())
            + self.engine.capacity() * (size_of::<u64>() + size_of::<EngineEnvironment>())
            + self.memo.capacity() * (size_of::<EnvironmentInputs>() + size_of::<MemoizedEnvironment>())
            + self.substitutions.capacity() * (size_of::<(usize, u16, u64)>() + size_of::<MemoizedSubstitution>())
            + self.names.capacity() * (size_of::<StyleAtomID>() + size_of::<CustomPropertyName>())) as u64
            + self.nested_capacity_bytes
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_nested_capacity(environments: &CustomPropertyEnvironments) {
        let expected = environments
            .names
            .values()
            .map(|name| name.text.capacity() * size_of::<u16>())
            .sum::<usize>()
            + environments
                .memo
                .iter()
                .map(|(inputs, environment)| {
                    inputs.cascaded.capacity() * size_of::<CascadedCustomProperty>()
                        + environment.written_values.capacity() * size_of::<RetainedStyleValueData>()
                })
                .sum::<usize>();
        assert_eq!(environments.nested_capacity_bytes, expected as u64);
    }

    #[test]
    fn nested_capacity_tracks_names_and_replaced_environment_entries() {
        let mut environments = CustomPropertyEnvironments::default();
        // SAFETY: A zero raw representation carries no borrowed fly string.
        unsafe {
            assert!(environments.note_name(StyleAtomID(1), 0, &[45, 45, 120]));
            assert!(!environments.note_name(StyleAtomID(1), 0, &[45, 45, 120]));
        }
        assert_nested_capacity(&environments);
        let inputs = |capacity| EnvironmentInputs {
            parent: 0,
            registration_generation: 1,
            cascaded: Vec::with_capacity(capacity),
        };
        environments.remember(inputs(3), 1, Vec::with_capacity(2));
        assert_nested_capacity(&environments);
        // Equal keys retain the original key's allocation when replacing the value.
        environments.remember(inputs(7), 2, Vec::with_capacity(5));
        assert_nested_capacity(&environments);
        environments.forget_names(&[StyleAtomID(1), StyleAtomID(1)]);
        assert_nested_capacity(&environments);
        environments.retain_only(|_| false);
        assert_nested_capacity(&environments);
        assert_eq!(environments.nested_capacity_bytes, 0);
    }
}
