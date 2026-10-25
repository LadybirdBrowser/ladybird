/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Watchpoint sets: what the runtime and JIT code may rely on until it stops holding, when the set fires.

use core::cell::{Cell, RefCell};

use crate::gc::class::{GcCell, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::gc::weak::GcWeak;
use crate::interpreter::vm::Vm;
use crate::jit::dependencies::{CellId, Dependency};
use crate::layout::cell::{CellHeader, Gc};
pub use crate::layout::shape::WatchpointSet;
use crate::runtime::special_property_cache::SpecialPropertyCache;

/// What relies on a watchpoint set, and stops relying on it when the set fires.
enum Watcher {
    /// A cache that forgets what it knows.
    Cache {
        cache: GcWeak<SpecialPropertyCache>,
        /// The generation of the cache when it started to rely on the set. Once the cache forgot what it knew, it
        /// relies on the sets it watches anew, and the watchers of its earlier generations are stale.
        generation: u32,
    },
    /// A set that stands for something that holds only while this set is valid, and fires with it.
    Set(GcWeak<WatchpointSet>),
}

impl Watcher {
    fn is_stale(&self) -> bool {
        match self {
            Watcher::Cache { cache, generation } => cache.get().is_none_or(|cache| cache.generation() != *generation),
            Watcher::Set(set) => set.get().is_none_or(|set| !set.is_valid()),
        }
    }
}

/// The fewest watchers a set prunes.
const MIN_WATCHERS_TO_PRUNE: usize = 8;

/// What a watchpoint set keeps besides whether it is valid: what relies on it.
pub struct WatchpointSetStorage {
    watchers: RefCell<Vec<Watcher>>,
    /// How many watchers the set has when it next prunes those that are stale.
    prune_at: Cell<usize>,
}

impl Default for WatchpointSetStorage {
    fn default() -> Self {
        Self {
            watchers: RefCell::new(Vec::new()),
            prune_at: Cell::new(MIN_WATCHERS_TO_PRUNE),
        }
    }
}

define_cell!(WatchpointSet, Other);

// SAFETY: A watchpoint set only holds weak references to what watches it.
unsafe impl Trace for WatchpointSet {
    fn trace(&self, _: &mut Visitor) {}
}

impl WatchpointSet {
    pub fn create(vm: &Vm) -> Gc<WatchpointSet> {
        vm.heap().allocate(|_| WatchpointSet {
            header: CellHeader::for_class(Self::CLASS),
            valid: Cell::new(true),
            storage: WatchpointSetStorage::default(),
        })
    }

    pub fn is_valid(&self) -> bool {
        self.valid.get()
    }

    /// Has `cache` forget what it knows of its generation `generation` when the set fires.
    pub(crate) fn add_watcher(&self, vm: &Vm, cache: Gc<SpecialPropertyCache>, generation: u32) {
        if self
            .storage
            .watchers
            .borrow()
            .last()
            .is_some_and(|watcher| matches!(watcher, Watcher::Cache { cache: last, generation: last_generation } if *last_generation == generation && last.get() == Some(cache)))
        {
            return;
        }
        self.push_watcher(Watcher::Cache {
            cache: GcWeak::new(vm.heap(), cache),
            generation,
        });
    }

    /// Has `set` fire when this set fires.
    pub fn add_dependent_set(&self, vm: &Vm, set: Gc<WatchpointSet>) {
        assert!(set.is_valid());
        self.push_watcher(Watcher::Set(GcWeak::new(vm.heap(), set)));
    }

    fn push_watcher(&self, watcher: Watcher) {
        assert!(self.is_valid());
        let mut watchers = self.storage.watchers.borrow_mut();
        // NB: A set that never fires keeps the watchers of caches that died with their shapes, or that forgot what they
        //     knew, and of sets that fired. Pruning them each time the number of watchers doubled since it last pruned
        //     keeps them in check.
        if watchers.len() >= self.storage.prune_at.get() {
            watchers.retain(|watcher| !watcher.is_stale());
            self.storage
                .prune_at
                .set((watchers.len() * 2).max(MIN_WATCHERS_TO_PRUNE));
        }
        watchers.push(watcher);
    }

    /// Notes that what the set stands for stopped holding, which has the caches that rely on it forget what they know,
    /// fires the sets that depend on it, and invalidates the JIT code that depends on it.
    pub fn fire(&self, vm: &Vm) {
        if !self.is_valid() {
            return;
        }
        self.valid.set(false);
        let watchers = core::mem::take(&mut *self.storage.watchers.borrow_mut());
        for watcher in watchers {
            match watcher {
                Watcher::Cache { cache, generation } => {
                    if let Some(cache) = cache.get() {
                        cache.forget(generation);
                    }
                }
                Watcher::Set(set) => {
                    if let Some(set) = set.get() {
                        set.fire(vm);
                    }
                }
            }
        }
        crate::jit::dependencies::invalidate_dependents(
            vm,
            Dependency::WatchpointSetValid(CellId(core::ptr::from_ref(self) as u64)),
        );
    }
}
