/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Interpreter feedback consumed by the optimizing JIT.
//!
//! Every instruction that collects feedback carries one slot index per feedback kind it records, and each kind has
//! its own index space in `ExecutableFeedback`. Instructions with a property lookup cache record their value feedback
//! in the slot of their cache instead, which spares them an index.
//!
//! Slot indices are encoded as u16. An executable with more sites of one kind than fit in a u16 shares the last slot
//! between the remaining sites, which only merges their feedback (see `generator::MAX_FEEDBACK_SLOT_COUNT`).

use core::cell::{Cell, OnceCell};
use core::ptr::NonNull;

use crate::build_configuration::HEAP_REGION_OFFSET_MASK;
use crate::gc::primitive_storage::{OwnedPrimitiveStorage, ZeroFillNewBytes};
use crate::gc::sweep::Sweep;
use crate::layout::cell::{CellHeader, Gc};
pub use crate::layout::feedback::*;
use crate::layout::object::Object;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;

/// How many feedback slots of each kind an executable's bytecode indexes into.
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct FeedbackSlotCounts {
    pub arith: u32,
    pub value: u32,
    pub call: u32,
    pub keyed: u32,
}

/// The `count` elements of type T at `offset` in `storage`.
fn array_in<T>(storage: Option<&OwnedPrimitiveStorage>, offset: usize, count: usize) -> &[T] {
    let Some(storage) = storage else {
        return &[];
    };
    // SAFETY: The feedback laid out `count` initialized elements of type T, aligned for T, at `offset` in the storage,
    //         which lives as long as the feedback. Its elements are cells, which allow shared mutation.
    unsafe { ak::slice_from_raw_parts(storage.data().add(offset).cast::<T>(), count) }
}

/// Where in the cage the interpreter finds the array at `offset` in `storage`, or 0 if there is none.
fn cage_offset_in(storage: Option<&OwnedPrimitiveStorage>, offset: usize) -> u64 {
    storage.map_or(0, |storage| (storage.offset() + offset) as u64)
}

fn allocate_feedback_storage(size: usize) -> Option<OwnedPrimitiveStorage> {
    // NB: No arith feedback, no call targets, no keyed feedback and no value feedback are all zero bytes.
    (size != 0).then(|| {
        OwnedPrimitiveStorage::allocate(size, ZeroFillNewBytes::Yes)
            .expect("there is memory for the feedback of an executable")
    })
}

/// The feedback of one executable, one array per kind, indexed by the slot indices of its instructions. The arrays
/// live in primitive storage, outside the GC heap.
///
/// Executables that warm up or that the JIT never compiles only run the profiling handlers of calls, returns and loop
/// back edges, which write arith and call feedback, and slow paths record keyed and value feedback for them. The value
/// buckets, which only the profiling tier writes, are made when the executable moves to it (see
/// `ensure_value_buckets()`), as most executables never get that warm. Until then, slow paths record values right into
/// the value feedback.
pub struct ExecutableFeedback {
    /// The arith feedback, the call feedback, the keyed feedback, the value feedback, then the value feedback of the
    /// sites of the property lookup caches.
    storage: Option<OwnedPrimitiveStorage>,
    counts: FeedbackSlotCounts,
    property_lookup_cache_count: u32,
    /// The value buckets of the sites of the property lookup caches, the last one first, then the other value buckets,
    /// once the profiling tier needs them. The interpreter finds both kinds from the start of the others.
    value_bucket_storage: OnceCell<OwnedPrimitiveStorage>,
}

const _: () = assert!(size_of::<ExecutableFeedback>() == 104);

impl ExecutableFeedback {
    /// Where the call feedback starts in the storage.
    fn call_offset(&self) -> usize {
        const _: () = assert!(align_of::<CallFeedback>() <= 8);
        (self.counts.arith as usize).next_multiple_of(8)
    }

    /// Where the value buckets of the sites without a property lookup cache start in their storage.
    fn value_buckets_offset(&self) -> usize {
        size_of::<Cell<u64>>() * self.property_lookup_cache_count as usize
    }

    /// Where the keyed feedback starts in the storage.
    fn keyed_offset(&self) -> usize {
        const _: () = assert!(align_of::<KeyedFeedback>() <= 8);
        self.call_offset() + size_of::<CallFeedback>() * self.counts.call as usize
    }

    /// Where the value feedback starts in the storage.
    fn value_offset(&self) -> usize {
        self.keyed_offset() + size_of::<KeyedFeedback>() * self.counts.keyed as usize
    }

    /// Where the value feedback of the sites of the property lookup caches starts in the storage.
    fn cache_value_offset(&self) -> usize {
        self.value_offset() + size_of::<Cell<u16>>() * self.counts.value as usize
    }

    fn value_buckets_storage(&self) -> Option<&OwnedPrimitiveStorage> {
        self.value_bucket_storage.get()
    }

    fn value_bucket_count(&self) -> usize {
        self.counts.value as usize + self.property_lookup_cache_count as usize
    }

    pub fn arith(&self) -> &[Cell<u8>] {
        array_in(self.storage.as_ref(), 0, self.counts.arith as usize)
    }

    pub fn call(&self) -> &[CallFeedback] {
        array_in(self.storage.as_ref(), self.call_offset(), self.counts.call as usize)
    }

    /// Whether the value buckets exist, which the profiling tier writes. Executables without value feedback slots and
    /// property lookup caches need none, so they count as having them.
    pub fn has_value_buckets(&self) -> bool {
        self.value_bucket_count() == 0 || self.value_bucket_storage.get().is_some()
    }

    /// One bucket per value feedback slot, see `value_feedback`. Empty while there are none.
    pub fn value_buckets(&self) -> &[Cell<u64>] {
        if !self.has_value_buckets() {
            return &[];
        }
        array_in(
            self.value_buckets_storage(),
            self.value_buckets_offset(),
            self.counts.value as usize,
        )
    }

    /// The value bucket of the site of property lookup cache `cache`. There have to be value buckets.
    pub fn cache_value_bucket(&self, cache: u32) -> &Cell<u64> {
        let buckets = self.cache_value_buckets();
        &buckets[buckets.len() - 1 - cache as usize]
    }

    /// One value bucket per property lookup cache, the last one first. Empty while there are none.
    fn cache_value_buckets(&self) -> &[Cell<u64>] {
        if !self.has_value_buckets() {
            return &[];
        }
        array_in(
            self.value_buckets_storage(),
            0,
            self.property_lookup_cache_count as usize,
        )
    }

    pub fn keyed(&self) -> &[KeyedFeedback] {
        array_in(self.storage.as_ref(), self.keyed_offset(), self.counts.keyed as usize)
    }

    /// The value feedback, one per value feedback slot.
    pub fn value(&self) -> &[Cell<u16>] {
        array_in(self.storage.as_ref(), self.value_offset(), self.counts.value as usize)
    }

    /// The value feedback of the sites of the property lookup caches, indexed like the caches.
    pub fn cache_value(&self) -> &[Cell<u16>] {
        array_in(
            self.storage.as_ref(),
            self.cache_value_offset(),
            self.property_lookup_cache_count as usize,
        )
    }

    /// Records `value` for value feedback slot `slot`: in its bucket once there are buckets, and right into its value
    /// feedback before.
    pub fn record_value(&self, slot: usize, value: u64) {
        if self.has_value_buckets() {
            self.value_buckets()[slot].set(value);
        } else {
            let bits = &self.value()[slot];
            bits.set(bits.get() | value_feedback::for_bits(value));
        }
    }

    /// record_value() for the site of property lookup cache `cache`.
    pub fn record_cache_value(&self, cache: u32, value: u64) {
        if self.has_value_buckets() {
            self.cache_value_bucket(cache).set(value);
        } else {
            let bits = &self.cache_value()[cache as usize];
            bits.set(bits.get() | value_feedback::for_bits(value));
        }
    }

    /// Forgets the call targets and keys that died in this collection, which the feedback holds weakly. The value
    /// buckets are not references, but they are folded into the value feedback here as well.
    pub fn remove_dead_cells(&self, sweep: &Sweep<'_>) {
        self.update_value_feedback();
        let clear_if_dead = |cell: &Cell<Option<Gc<CellHeader>>>| {
            if decode_weak_reference(cell).is_some_and(|cell| sweep.is_dead(cell)) {
                cell.set(None);
            }
        };
        for call in self.call() {
            clear_if_dead(&call.target);
            clear_if_dead(&call.forwarded_target);
        }
        for keyed in self.keyed() {
            clear_if_dead(&keyed.last_key);
        }
    }

    /// Folds the value buckets, the call sites' too, into the value feedback bits. The buckets are not references, so
    /// this runs when the feedback is about to be used and at every garbage collection.
    pub fn update_value_feedback(&self) {
        let buckets = self.value_buckets().iter().zip(self.value());
        let cache_buckets = self.cache_value_buckets().iter().rev().zip(self.cache_value());
        for (bucket, value) in buckets.chain(cache_buckets) {
            let bits = bucket.get();
            if bits != value_feedback::EMPTY_VALUE_BUCKET {
                value.set(value.get() | value_feedback::for_bits(bits));
            }
        }
        for call in self.call() {
            let tag = call.value_bucket.get();
            if tag != value_feedback::EMPTY_VALUE_BUCKET_TAG {
                call.value
                    .set(call.value.get() | value_feedback::for_bits(u64::from(tag) << 48));
            }
        }
    }

    /// The feedback of an executable, without the value buckets.
    pub fn new(counts: FeedbackSlotCounts, property_lookup_cache_count: u32) -> Self {
        let mut feedback = Self {
            storage: None,
            counts,
            property_lookup_cache_count,
            value_bucket_storage: OnceCell::new(),
        };
        feedback.storage = allocate_feedback_storage(
            feedback.cache_value_offset() + size_of::<Cell<u16>>() * property_lookup_cache_count as usize,
        );
        const _: () = assert!(arith_feedback::NONE == 0 && value_feedback::NONE == 0);
        for call in feedback.call() {
            call.value_bucket.set(value_feedback::EMPTY_VALUE_BUCKET_TAG);
        }
        feedback
    }

    /// Makes the value buckets, if they are not there yet. The interpreter has to be pointed at them.
    pub fn ensure_value_buckets(&self) {
        if self.has_value_buckets() {
            return;
        }
        let Some(storage) = allocate_feedback_storage(size_of::<Cell<u64>>() * self.value_bucket_count()) else {
            unreachable!("has_value_buckets() is true for executables without value buckets");
        };
        if self.value_bucket_storage.set(storage).is_err() {
            unreachable!("the value buckets are made once");
        }
        for bucket in self.value_buckets().iter().chain(self.cache_value_buckets()) {
            bucket.set(value_feedback::EMPTY_VALUE_BUCKET);
        }
    }

    /// Where the arith feedback, the value buckets, the call feedback and the keyed feedback start, as offsets into
    /// the primitive storage cage, for the interpreter, or 0 if there are none.
    pub fn array_offsets(&self) -> [u64; 4] {
        let value_buckets_storage = self.value_buckets_storage();
        [
            cage_offset_in(self.storage.as_ref(), 0),
            cage_offset_in(value_buckets_storage, self.value_buckets_offset()),
            cage_offset_in(self.storage.as_ref(), self.call_offset()),
            cage_offset_in(self.storage.as_ref(), self.keyed_offset()),
        ]
    }
}

/// A weak reference the feedback holds, decoded into the heap region like a cell value. The feedback lives in primitive
/// storage, where anything may have overwritten it, so the runtime only ever dereferences its references as cells of
/// the heap region.
fn decode_weak_reference(reference: &Cell<Option<Gc<CellHeader>>>) -> Option<Gc<CellHeader>> {
    let cell = reference.get()?;
    // NB: The heap region base was fixed before any cell existed.
    let base = crate::layout_forward::heap_region_base() as u64;
    let address = base + (cell.as_ptr() as u64 & HEAP_REGION_OFFSET_MASK);
    let address = NonNull::new(core::ptr::with_exposed_provenance_mut::<CellHeader>(address as usize))
        .expect("the heap region does not start at null");
    // SAFETY: The address is in the heap region, where the feedback's references to cells that are alive point.
    Some(unsafe { Gc::from_non_null(address) })
}

impl CallFeedback {
    /// The first callee seen at this site, if it is still alive.
    pub fn target(&self) -> Option<Gc<CellHeader>> {
        decode_weak_reference(&self.target)
    }

    /// The function the first callee forwarded the first call through it to, if it is still alive.
    pub fn forwarded_target(&self) -> Option<Gc<CellHeader>> {
        decode_weak_reference(&self.forwarded_target)
    }

    /// Records that the first callee forwarded a call to `target`.
    pub fn record_forwarded_call(
        &self,
        target: Gc<CellHeader>,
        forwarding: CallFeedbackForwarding,
        argument_count: usize,
    ) {
        let argument_count = u16::try_from(argument_count).unwrap_or(u16::MAX);
        let Some(forwarded_target) = self.forwarded_target() else {
            self.forwarded_target.set(Some(target));
            self.forwarding.set(forwarding as u8);
            self.forwarded_argument_count.set(argument_count);
            return;
        };
        if self.forwarding.get() != forwarding as u8 || self.forwarded_argument_count.get() != argument_count {
            self.flags
                .set(self.flags.get() | call_feedback_flags::FORWARDED_POLYMORPHIC);
        } else if forwarded_target != target {
            let flag = if are_closures_of_one_function(forwarded_target, target) {
                call_feedback_flags::FORWARDED_CLOSURES
            } else {
                call_feedback_flags::FORWARDED_POLYMORPHIC
            };
            self.flags.set(self.flags.get() | flag);
        }
    }
}

/// Whether two cells are closures of one ECMAScript function.
pub fn are_closures_of_one_function(first: Gc<CellHeader>, second: Gc<CellHeader>) -> bool {
    let function_of = |cell: Gc<CellHeader>| as_ecmascript_function_object(cell.downcast::<Object>()?);
    match (function_of(first), function_of(second)) {
        (Some(first), Some(second)) => first.shared_data() == second.shared_data(),
        _ => false,
    }
}

impl KeyedFeedback {
    /// The last string or symbol key seen at this site, if it is still alive.
    pub fn last_key(&self) -> Option<Gc<CellHeader>> {
        decode_weak_reference(&self.last_key)
    }
}
