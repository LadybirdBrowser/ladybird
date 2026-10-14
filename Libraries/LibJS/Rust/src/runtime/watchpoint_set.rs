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

/// A cache that relies on a watchpoint set, and forgets what it knows when the set fires.
struct Watcher {
    cache: GcWeak<SpecialPropertyCache>,
    /// The generation of the cache when it started to rely on the set. Once the cache forgot what it knew, it relies
    /// on the sets it watches anew, and the watchers of its earlier generations are stale.
    generation: u32,
}

/// The fewest watchers a set prunes.
const MIN_WATCHERS_TO_PRUNE: usize = 8;

/// What a watchpoint set keeps besides whether it is valid: the caches that rely on it.
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

// SAFETY: A watchpoint set only holds weak references to the caches that watch it.
unsafe impl Trace for WatchpointSet {
    fn trace(&self, _: &mut Visitor) {}
}

impl WatchpointSet {
    pub fn create(vm: &Vm) -> Gc<WatchpointSet> {
        vm.heap().allocate(WatchpointSet {
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
        assert!(self.is_valid());
        let mut watchers = self.storage.watchers.borrow_mut();
        if watchers
            .last()
            .is_some_and(|watcher| watcher.generation == generation && watcher.cache.get() == Some(cache))
        {
            return;
        }
        // NB: A set that never fires keeps the watchers of caches that died with their shapes, or that forgot what they
        //     knew. Pruning them each time the number of watchers doubled since it last pruned keeps them in check.
        if watchers.len() >= self.storage.prune_at.get() {
            watchers.retain(|watcher| {
                watcher
                    .cache
                    .get()
                    .is_some_and(|cache| cache.generation() == watcher.generation)
            });
            self.storage
                .prune_at
                .set((watchers.len() * 2).max(MIN_WATCHERS_TO_PRUNE));
        }
        watchers.push(Watcher {
            cache: GcWeak::new(vm.heap(), cache),
            generation,
        });
    }

    /// Notes that what the set stands for stopped holding, which has the caches that rely on it forget what they know,
    /// and invalidates the JIT code that depends on it.
    pub fn fire(&self, vm: &Vm) {
        if !self.is_valid() {
            return;
        }
        self.valid.set(false);
        let watchers = core::mem::take(&mut *self.storage.watchers.borrow_mut());
        for watcher in watchers {
            if let Some(cache) = watcher.cache.get() {
                cache.forget(watcher.generation);
            }
        }
        crate::jit::dependencies::invalidate_dependents(
            vm,
            Dependency::WatchpointSetValid(CellId(core::ptr::from_ref(self) as u64)),
        );
    }
}
