/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Interpreter feedback consumed by the optimizing JIT.
//!
//! Every instruction that collects feedback carries one slot index per feedback kind it records, and each kind has
//! its own index space in `ExecutableFeedback`.
//!
//! Slot indices are encoded as u16. An executable with more sites of one kind than fit in a u16 shares the last slot
//! between the remaining sites, which only merges their feedback (see `generator::MAX_FEEDBACK_SLOT_COUNT`).

use core::cell::Cell;
use core::ptr::NonNull;

use crate::build_configuration::HEAP_REGION_OFFSET_MASK;
use crate::gc::capi;
use crate::gc::class::{GcCell, class_of};
use crate::gc::heap::cell_is_dead;
use crate::gc::primitive_storage::{OwnedPrimitiveStorage, ZeroFillNewBytes};
use crate::layout::cell::{CellHeader, Gc};
pub use crate::layout::feedback::*;
use crate::layout::object::Object;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;

/// How many feedback slots of each kind an executable's bytecode indexes into.
#[derive(Clone, Copy, Default)]
pub struct FeedbackSlotCounts {
    pub arith: u32,
    pub value: u32,
    pub call: u32,
    pub keyed: u32,
}

/// Where the arrays the profiling interpreter writes start in an executable's feedback storage, and its size.
#[derive(Clone, Copy)]
struct FeedbackStorageLayout {
    value_buckets: usize,
    call: usize,
    keyed: usize,
    size: usize,
}

impl FeedbackStorageLayout {
    fn new(counts: FeedbackSlotCounts) -> Self {
        const _: () = assert!(align_of::<CallFeedback>() <= 8 && align_of::<KeyedFeedback>() <= 8);
        let value_buckets = (counts.arith as usize).next_multiple_of(8);
        let call = value_buckets + size_of::<Cell<u64>>() * counts.value as usize;
        let keyed = call + size_of::<CallFeedback>() * counts.call as usize;
        let size = keyed + size_of::<KeyedFeedback>() * counts.keyed as usize;
        Self {
            value_buckets,
            call,
            keyed,
            size,
        }
    }
}

/// The feedback of one executable, one array per kind, indexed by the slot indices of its instructions. The arrays the
/// profiling interpreter writes (arith, value buckets, call and keyed feedback) live back to back in primitive
/// storage, outside the GC heap; the value feedback the buckets are folded into is the runtime's own.
pub struct ExecutableFeedback {
    storage: Option<OwnedPrimitiveStorage>,
    counts: FeedbackSlotCounts,
    layout: FeedbackStorageLayout,
    pub value: Box<[Cell<u16>]>,
}

impl ExecutableFeedback {
    /// The `count` elements of type T at `offset` in the storage.
    fn array<T>(&self, offset: usize, count: u32) -> &[T] {
        let Some(storage) = &self.storage else {
            return &[];
        };
        // SAFETY: new() laid out `count` initialized elements of type T, aligned for T, at `offset` in the storage,
        //         which lives as long as the feedback. Its elements are cells, which allow shared mutation.
        unsafe { core::slice::from_raw_parts(storage.data().add(offset).cast::<T>(), count as usize) }
    }

    pub fn arith(&self) -> &[Cell<u8>] {
        self.array(0, self.counts.arith)
    }

    /// One bucket per value feedback slot, see `value_feedback`.
    pub fn value_buckets(&self) -> &[Cell<u64>] {
        self.array(self.layout.value_buckets, self.counts.value)
    }

    pub fn call(&self) -> &[CallFeedback] {
        self.array(self.layout.call, self.counts.call)
    }

    pub fn keyed(&self) -> &[KeyedFeedback] {
        self.array(self.layout.keyed, self.counts.keyed)
    }

    /// Forgets the call targets and keys that died in this collection, which the feedback holds weakly. The value
    /// buckets are not references, but they are folded into the value feedback here as well.
    pub fn remove_dead_cells(&self) {
        self.update_value_feedback();
        let clear_if_dead = |cell: &Cell<Option<Gc<CellHeader>>>| {
            if decode_weak_reference(cell).is_some_and(cell_is_dead) {
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

    /// Folds the value buckets into the value feedback bits. The buckets are not references, so this runs when the
    /// feedback is about to be used and at every garbage collection.
    pub fn update_value_feedback(&self) {
        for (bucket, value) in self.value_buckets().iter().zip(&self.value) {
            let bits = bucket.get();
            if bits != value_feedback::EMPTY_VALUE_BUCKET {
                value.set(value.get() | value_feedback::for_bits(bits));
            }
        }
    }

    pub fn new(counts: FeedbackSlotCounts) -> Self {
        let layout = FeedbackStorageLayout::new(counts);
        // NB: No arith feedback, no call feedback and no keyed feedback are all zero bytes.
        let storage = (layout.size != 0).then(|| {
            OwnedPrimitiveStorage::allocate(layout.size, ZeroFillNewBytes::Yes)
                .expect("there is memory for the feedback of an executable")
        });
        let feedback = Self {
            storage,
            counts,
            layout,
            value: (0..counts.value).map(|_| Cell::new(value_feedback::NONE)).collect(),
        };
        const _: () = assert!(arith_feedback::NONE == 0);
        for bucket in feedback.value_buckets() {
            bucket.set(value_feedback::EMPTY_VALUE_BUCKET);
        }
        feedback
    }

    /// Where the arith feedback, the value buckets, the call feedback and the keyed feedback start, as offsets into
    /// the primitive storage cage, for the interpreter, or 0 if there are none.
    pub fn array_offsets(&self) -> [u64; 4] {
        let Some(storage) = &self.storage else {
            return [0; 4];
        };
        [0, self.layout.value_buckets, self.layout.call, self.layout.keyed]
            .map(|offset| (storage.offset() + offset) as u64)
    }
}

/// A weak reference the feedback holds, decoded into the heap region like a cell value. The feedback lives in primitive
/// storage, where anything may have overwritten it, so the runtime only ever dereferences its references as cells of
/// the heap region.
fn decode_weak_reference(reference: &Cell<Option<Gc<CellHeader>>>) -> Option<Gc<CellHeader>> {
    let cell = reference.get()?;
    // SAFETY: LibGC fixed the heap region base before any cell existed.
    let base = unsafe { capi::js_heap_region_base } as u64;
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
    let function_of = |cell: Gc<CellHeader>| {
        if !class_of(cell).is_subclass_of(Object::CLASS) {
            return None;
        }
        // SAFETY: The cell is an object.
        as_ecmascript_function_object(unsafe { Gc::<Object>::from_non_null(cell.as_non_null().cast()) })
    };
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
