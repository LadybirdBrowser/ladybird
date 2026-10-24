/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The TransformStream of an interface that includes GenericTransformStream,
//! https://streams.spec.whatwg.org/#generictransformstream, which LibWeb implements in C++.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{
    EmbedderCell, EmbedderValue, ForeignCellSlot, Gc, Object, RawNativeFunctionResult, Trace, Value,
};

use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::realm_arguments;
use crate::bindings::{CallContext, WebIdlResult};

/// The steps of the transformAlgorithm, with the chunk, and of the flushAlgorithm, without one.
type StepsEntry = unsafe extern "C" fn(
    vm: *mut c_void,
    realm: *mut c_void,
    steps: *mut c_void,
    transform: *mut c_void,
    chunk: *const u64,
) -> RawNativeFunctionResult;

/// Web::Streams::TransformStreamSide
#[repr(u8)]
enum Side {
    Readable,
    Writable,
}

unsafe extern "C" {
    /// A new TransformStream in the realm, set up with a transformAlgorithm and a flushAlgorithm that call the entry
    /// with the steps, a cell that the stream keeps alive.
    fn web_transform_stream_create(realm: *mut c_void, steps: *mut c_void, run_steps: StepsEntry) -> *mut c_void;

    fn web_transform_stream_enqueue(transform: *mut c_void, chunk: u64);

    /// The wrapper in the realm of the stream's [[readable]] or [[writable]].
    fn web_transform_stream_side(transform: *mut c_void, realm: *mut c_void, side: Side) -> *mut c_void;
}

/// The transformAlgorithm and flushAlgorithm that a TransformStream is set up with,
/// https://streams.spec.whatwg.org/#transformstream-set-up, which the stream holds in a cell of their own. They
/// resolve the promise that the algorithm returns, or reject it with what they throw.
pub(crate) trait TransformSteps: EmbedderValue {
    fn transform(&self, cx: &CallContext, transform: &TransformStream, chunk: Value) -> WebIdlResult<()>;

    fn flush(&self, cx: &CallContext, transform: &TransformStream) -> WebIdlResult<()>;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TransformStream(ForeignCellSlot);

impl TransformStream {
    /// # Safety
    ///
    /// `transform` must be a live TransformStream.
    unsafe fn from_raw(transform: *mut c_void) -> Self {
        let slot = ForeignCellSlot::empty();
        // SAFETY: The caller passes a live TransformStream, a cell of the heap.
        unsafe { slot.set(Some(NonNull::new(transform).expect("LibWeb passes a TransformStream"))) };
        Self(slot)
    }

    /// A new TransformStream, set up with the steps,
    /// https://streams.spec.whatwg.org/#transformstream-set-up
    pub(crate) fn create_and_set_up<T: TransformSteps>(cx: &CallContext, steps: T) -> Self {
        let steps = T::CLASS.allocate(cx.vm(), steps);
        // SAFETY: The realm and the cell are live, and the entry takes a cell of the class of the steps.
        unsafe {
            Self::from_raw(web_transform_stream_create(
                cx.realm().as_ptr().cast(),
                steps.as_ptr().cast(),
                run_steps::<T>,
            ))
        }
    }

    /// https://streams.spec.whatwg.org/#transformstream-enqueue
    pub(crate) fn enqueue(&self, chunk: Value) {
        // SAFETY: The slot keeps the stream alive.
        unsafe { web_transform_stream_enqueue(self.0.as_ptr(), chunk.0) };
    }

    /// The stream's [[readable]].
    pub(crate) fn readable(&self, cx: &CallContext) -> CppInterface {
        self.side(cx, Side::Readable)
    }

    /// The stream's [[writable]].
    pub(crate) fn writable(&self, cx: &CallContext) -> CppInterface {
        self.side(cx, Side::Writable)
    }

    fn side(&self, cx: &CallContext, side: Side) -> CppInterface {
        // SAFETY: The slot keeps the stream alive, and the realm is live.
        let wrapper = unsafe { web_transform_stream_side(self.0.as_ptr(), cx.realm().as_ptr().cast(), side) };
        let wrapper = NonNull::new(wrapper.cast::<Object>()).expect("LibWeb returns a wrapper");
        // SAFETY: LibWeb returns a live wrapper of the ReadableStream or the WritableStream that the side names.
        unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
    }
}

/// The steps of the transformAlgorithm, given a chunk, or else of the flushAlgorithm, of a TransformStream that
/// create_and_set_up() set up.
///
/// # Safety
///
/// LibWeb passes the VM, the realm of the stream, the cell of the steps, which the stream keeps alive, the stream, and
/// a chunk that is live for the call or null.
unsafe extern "C" fn run_steps<T: TransformSteps>(
    vm: *mut c_void,
    realm: *mut c_void,
    steps: *mut c_void,
    transform: *mut c_void,
    chunk: *const u64,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the live VM, realm, cell and stream.
    let (cx, steps, transform) = unsafe { steps_arguments::<T>(vm, realm, steps, transform) };
    let _type_error_realm = cx.vm().type_error_realm_scope(cx.realm());
    // SAFETY: The caller passes a chunk that is live for the call, or null.
    let result = match unsafe { chunk.as_ref() } {
        Some(&chunk) => steps.transform(&cx, &transform, Value(chunk)),
        None => steps.flush(&cx, &transform),
    };
    RawNativeFunctionResult::from_completion(result.map(|()| Value::UNDEFINED).map_err(|error| error.throw(&cx)))
}

/// # Safety
///
/// The pointers must be the live VM, a live realm, a live cell of the class of the steps and a live TransformStream.
unsafe fn steps_arguments<'vm, T: TransformSteps>(
    vm: *mut c_void,
    realm: *mut c_void,
    steps: *mut c_void,
    transform: *mut c_void,
) -> (CallContext<'vm>, Gc<EmbedderCell<T>>, TransformStream) {
    // SAFETY: The caller passes the live VM and realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let steps = NonNull::new(steps).expect("LibWeb passes the steps");
    // SAFETY: The caller passes a live cell of an embedder class.
    let steps = unsafe { T::CLASS.downcast(steps) }.expect("LibWeb passes the steps it was given");
    // SAFETY: The caller passes a live TransformStream.
    let transform = unsafe { TransformStream::from_raw(transform) };
    (CallContext::new(vm, realm), steps, transform)
}
