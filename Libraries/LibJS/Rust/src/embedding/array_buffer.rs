/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ArrayBuffers, SharedArrayBuffers and DataViews.
//!
//! An ArrayBuffer's bytes live in LibGC's primitive storage, in one of three kinds of storage: storage the buffer owns,
//! storage that a GC cell of the embedder owns (a wasm memory, the channel of an AudioBuffer), and a shared memory
//! object that agents in other processes map too. Storage handles cross as the GCPrimitiveStorageHandle word of
//! LibGC/CAPI.h. Every function here must be called on the thread that runs the VM.

use core::ffi::{c_int, c_void};
use core::ptr::NonNull;

use crate::embedding::abi_types::{
    JSRealm, cell_from_abi, completion_into_abi, completion_writing_result_to, object_into_abi,
    optional_object_into_abi, vm_from_abi,
};
use crate::gc::capi::GC_PRIMITIVE_STORAGE_NULL_HANDLE;
use crate::gc::class::{Extends, GcCell};
use crate::gc::foreign::ForeignCellSlot;
use crate::gc::shared_memory::{borrowed_from_raw_descriptor, into_raw_descriptor};
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSCompletion, JSObject, JSVM, JSValue};
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array_buffer::{
    ArrayBuffer, DataBlock, DataBlockStorage, ExternalPrimitiveStorage, Order, OwnedBackingStore, Shared,
    clone_array_buffer, detach_array_buffer,
};
use crate::runtime::byte_length::ByteLength;
use crate::runtime::data_view::{
    DataView, DataViewWithBufferWitness, get_view_byte_length, is_view_out_of_bounds,
    make_data_view_with_buffer_witness_record,
};

/// JSArrayBufferStorage.kind of a detached buffer, which has no storage.
pub const JS_ARRAY_BUFFER_STORAGE_DETACHED: u8 = 0;
/// JSArrayBufferStorage.kind of storage the buffer owns, which has no handle while it has no bytes.
pub const JS_ARRAY_BUFFER_STORAGE_OWNED: u8 = 1;
/// JSArrayBufferStorage.kind of storage that a GC cell of the embedder owns.
pub const JS_ARRAY_BUFFER_STORAGE_EXTERNAL: u8 = 2;
/// JSArrayBufferStorage.kind of a fixed-length SharedArrayBuffer in a shared memory object.
pub const JS_ARRAY_BUFFER_STORAGE_SHARED_MEMORY: u8 = 3;

/// JS::ArrayBuffer::Order, the order of the read of a SharedArrayBuffer's byte length.
pub const JS_ARRAY_BUFFER_ORDER_SEQ_CST: u8 = 0;
pub const JS_ARRAY_BUFFER_ORDER_UNORDERED: u8 = 1;

/// JS::ByteLength: kind is one of the JS_BYTE_LENGTH_* constants, which are the alternative indices of the ByteLength
/// that a typed array keeps, and length is meaningful only for JS_BYTE_LENGTH_LENGTH.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct JSByteLength {
    pub length: u32,
    pub kind: u8,
}

pub const JS_BYTE_LENGTH_AUTO: u8 = 0;
pub const JS_BYTE_LENGTH_DETACHED: u8 = 1;
pub const JS_BYTE_LENGTH_LENGTH: u8 = 2;

/// Storage in LibGC's primitive storage that a GC cell of the embedder owns: DataBlock::ExternalPrimitiveStorage. A
/// buffer over it keeps the owner alive and never frees or resizes the storage.
#[repr(C)]
pub struct JSExternalPrimitiveStorage {
    /// The GC cell, C++ or engine, that owns the storage.
    pub owner: *mut c_void,
    /// A GCPrimitiveStorageHandle.
    pub handle: u64,
    /// The byte length of the buffer, unless has_fixed_byte_length is false and the buffer has the size of the storage,
    /// which then follows the owner's resizing.
    pub fixed_byte_length: usize,
    pub has_fixed_byte_length: bool,
}

/// What an ArrayBuffer's [[ArrayBufferData]] is made of, as js_array_buffer_storage() describes it.
///
/// The handle of JS_ARRAY_BUFFER_STORAGE_OWNED storage stays the buffer's: the embedder must never resize or free it.
/// It may alias it, as structured deserialization does, by making a SharedArrayBuffer over a
/// JSExternalPrimitiveStorage whose owner is the buffer itself, which it may only do for a SharedArrayBuffer, since
/// detaching any other buffer frees its storage. A growable alias without a fixed byte length grows by growing the
/// buffer that owns the storage. An alias with a fixed byte length may only alias a buffer that never grows, as the
/// typed arrays of a fixed-length buffer cache where its bytes are.
#[repr(C)]
pub struct JSArrayBufferStorage {
    /// One of the JS_ARRAY_BUFFER_STORAGE_* constants.
    pub kind: u8,
    /// The GCPrimitiveStorageHandle of the bytes, or GC_PRIMITIVE_STORAGE_NULL_HANDLE when there are none.
    pub handle: u64,
    /// For JS_ARRAY_BUFFER_STORAGE_EXTERNAL, the owner and the byte length; zero otherwise.
    pub external: JSExternalPrimitiveStorage,
    /// For JS_ARRAY_BUFFER_STORAGE_SHARED_MEMORY, the id that names the shared memory object in every agent, or 0 if it
    /// has none; zero otherwise.
    pub shared_memory_object_id: u64,
}

/// The DataView With Buffer Witness Record of the spec, which C++ calls DataViewWithBufferWitness.
#[repr(C)]
pub struct JSDataViewWithBufferWitness {
    pub data_view: *mut JSObject,
    pub cached_buffer_byte_length: JSByteLength,
}

/// # Safety
///
/// `object` must be the address of a live object.
pub(super) unsafe fn cell_of_class_from_abi<T: GcCell + Extends<Object>>(
    object: *mut JSObject,
    class_name: &str,
) -> Gc<T> {
    // SAFETY: The caller passes a live object.
    unsafe { cell_from_abi::<JSObject>(object) }
        .downcast::<T>()
        .unwrap_or_else(|| panic!("the embedder passes an object of class {class_name}"))
}

/// # Safety
///
/// `buffer` must be the address of a live ArrayBuffer.
unsafe fn array_buffer_from_abi(buffer: *mut JSObject) -> Gc<ArrayBuffer> {
    // SAFETY: The caller guarantees that the pointer is the address of a live object.
    unsafe { cell_of_class_from_abi(buffer, "ArrayBuffer") }
}

/// # Safety
///
/// `data_view` must be the address of a live DataView.
unsafe fn data_view_from_abi(data_view: *mut JSObject) -> Gc<DataView> {
    // SAFETY: The caller guarantees that the pointer is the address of a live object.
    unsafe { cell_of_class_from_abi(data_view, "DataView") }
}

pub(super) fn byte_length_into_abi(byte_length: ByteLength) -> JSByteLength {
    match byte_length {
        ByteLength::Auto => JSByteLength {
            length: 0,
            kind: JS_BYTE_LENGTH_AUTO,
        },
        ByteLength::Detached => JSByteLength {
            length: 0,
            kind: JS_BYTE_LENGTH_DETACHED,
        },
        ByteLength::Length(length) => JSByteLength {
            length,
            kind: JS_BYTE_LENGTH_LENGTH,
        },
    }
}

pub(super) fn byte_length_from_abi(byte_length: JSByteLength) -> ByteLength {
    match byte_length.kind {
        JS_BYTE_LENGTH_AUTO => ByteLength::Auto,
        JS_BYTE_LENGTH_DETACHED => ByteLength::Detached,
        JS_BYTE_LENGTH_LENGTH => ByteLength::Length(byte_length.length),
        kind => panic!("the embedder passes a byte length of unknown kind {kind}"),
    }
}

pub(super) fn order_from_abi(order: u8) -> Order {
    match order {
        JS_ARRAY_BUFFER_ORDER_SEQ_CST => Order::SeqCst,
        JS_ARRAY_BUFFER_ORDER_UNORDERED => Order::Unordered,
        order => panic!("the embedder passes an order of unknown kind {order}"),
    }
}

fn shared_from_abi(shared: bool) -> Shared {
    if shared { Shared::Yes } else { Shared::No }
}

/// # Safety
///
/// `storage` must point to a JSExternalPrimitiveStorage whose owner is a live cell of the VM's heap.
unsafe fn external_storage_from_abi(storage: *const JSExternalPrimitiveStorage) -> ExternalPrimitiveStorage {
    assert!(!storage.is_null(), "the embedder passes its storage");
    // SAFETY: The caller guarantees that the pointer is valid for reads.
    let storage = unsafe { &*storage };
    let owner_cell = NonNull::new(storage.owner).expect("external storage has an owner");
    let owner = ForeignCellSlot::empty();
    // SAFETY: The caller guarantees that the owner is a live cell of the heap, which the slot then keeps alive.
    unsafe { owner.set(Some(owner_cell)) };
    let fixed_byte_length = storage.has_fixed_byte_length.then_some(storage.fixed_byte_length);
    ExternalPrimitiveStorage::new(owner, storage.handle, fixed_byte_length)
}

/// ArrayBuffer::create(Realm&, size_t, DataBlock::Shared): a fixed-length buffer of `byte_length` zero bytes that the
/// buffer owns, with the prototype of %ArrayBuffer% or, if `shared`, %SharedArrayBuffer%. The buffer is the payload of
/// the normal completion. Throws a RangeError when there is not enough memory. Main thread only.
///
/// # Safety
///
/// `vm` and `realm` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_create(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    byte_length: usize,
    shared: bool,
) -> JSCompletion {
    // SAFETY: The caller passes a live VM and realm.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi(realm)) };
    completion_into_abi(ArrayBuffer::create(vm, realm, byte_length, shared_from_abi(shared)))
}

/// ArrayBuffer::create(Realm&, ByteBuffer, DataBlock::Shared): a fixed-length buffer that the buffer owns, with a copy
/// of `length` bytes. Aborts when there is not enough memory. Main thread only.
///
/// # Safety
///
/// `vm` and `realm` must be live, and `bytes` valid for reading `length` bytes, or null if `length` is 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_create_from_bytes(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    bytes: *const u8,
    length: usize,
    shared: bool,
) -> *mut JSObject {
    // SAFETY: The caller passes a live VM and realm.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi(realm)) };
    let bytes = if length == 0 {
        &[]
    } else {
        // SAFETY: The caller passes `length` readable bytes.
        unsafe { core::slice::from_raw_parts(bytes, length) }
    };
    object_into_abi(ArrayBuffer::create_from_bytes(
        vm,
        realm,
        bytes,
        shared_from_abi(shared),
    ))
}

/// ArrayBuffer::create(Realm&, DataBlock) over storage that a GC cell of the embedder owns: a fixed-length buffer, and
/// a SharedArrayBuffer if `shared`. The buffer keeps the owner alive. The owner may resize the storage, which a buffer
/// without a fixed byte length follows, but the typed arrays of a fixed-length buffer cache the offset of the storage,
/// so once the owner moves the storage, it must detach such a buffer before any script runs, as WebAssembly.Memory
/// does. The embedder makes a buffer resizable with js_array_buffer_set_max_byte_length(). Main thread only.
///
/// # Safety
///
/// `vm` and `realm` must be live, and `storage` must be valid with an owner that is a live cell of the VM's heap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_create_with_external_storage(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    storage: *const JSExternalPrimitiveStorage,
    shared: bool,
) -> *mut JSObject {
    // SAFETY: The caller passes a live VM and realm, and valid storage.
    let (vm, realm, storage) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(realm),
            external_storage_from_abi(storage),
        )
    };
    let block = DataBlock::external(storage, shared_from_abi(shared));
    object_into_abi(ArrayBuffer::create_from_data_block(vm, realm, block))
}

/// ArrayBuffer::create(Realm&, Core::AnonymousBuffer, u64 shared_object_id): a fixed-length SharedArrayBuffer over the
/// first `byte_length` bytes of a shared memory object, which `object_id` names in every agent, as structured
/// deserialization creates it. The embedder keeps owning `shared_memory_fd` and may close it right away, as the buffer
/// keeps a duplicate. Returns null if the object cannot be mapped into the cage, which includes a `byte_length` of 0
/// and an object smaller than `byte_length`. Main thread only.
///
/// # Safety
///
/// `vm` and `realm` must be live, and `shared_memory_fd` an open descriptor.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_create_from_shared_memory(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    shared_memory_fd: c_int,
    byte_length: usize,
    object_id: u64,
) -> *mut JSObject {
    // SAFETY: The caller passes a live VM and realm.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi(realm)) };
    assert!(shared_memory_fd >= 0, "the embedder passes an open descriptor");
    // SAFETY: The caller guarantees that the descriptor stays open for the duration of the call.
    let shared_memory = unsafe { borrowed_from_raw_descriptor(shared_memory_fd) };
    optional_object_into_abi(
        ArrayBuffer::create_from_shared_memory(vm, realm, shared_memory, byte_length, object_id).ok(),
    )
}

/// ArrayBuffer::set_data_block() with storage that a GC cell of the embedder owns, as WebAssembly.Memory refreshes a
/// resizable buffer after it grew: the buffer drops its old storage, keeps `storage`'s owner alive, and becomes a
/// SharedArrayBuffer if `shared`. Main thread only.
///
/// # Safety
///
/// `vm` must be live, `buffer` a live ArrayBuffer, and `storage` valid with an owner that is a live cell of the VM's
/// heap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_set_external_storage(
    vm: *mut JSVM,
    buffer: *mut JSObject,
    storage: *const JSExternalPrimitiveStorage,
    shared: bool,
) {
    // SAFETY: The caller passes a live VM and buffer, and valid storage.
    let (vm, buffer, storage) = unsafe {
        (
            vm_from_abi(vm),
            array_buffer_from_abi(buffer),
            external_storage_from_abi(storage),
        )
    };
    buffer.set_data_block(vm, DataBlock::external(storage, shared_from_abi(shared)));
}

/// Describes the storage of the buffer's data block into `out`, for code that shares it with another block, as
/// structured serialization does. Main thread only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer, and `out` valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_storage(buffer: *mut JSObject, out: *mut JSArrayBufferStorage) {
    assert!(!out.is_null(), "the embedder passes an out parameter");
    // SAFETY: The caller passes a live buffer.
    let buffer = unsafe { array_buffer_from_abi(buffer) };
    let mut storage = JSArrayBufferStorage {
        kind: JS_ARRAY_BUFFER_STORAGE_DETACHED,
        handle: GC_PRIMITIVE_STORAGE_NULL_HANDLE,
        external: JSExternalPrimitiveStorage {
            owner: core::ptr::null_mut(),
            handle: GC_PRIMITIVE_STORAGE_NULL_HANDLE,
            fixed_byte_length: 0,
            has_fixed_byte_length: false,
        },
        shared_memory_object_id: 0,
    };
    buffer.with_data_block(|block| match &block.byte_buffer {
        DataBlockStorage::Empty => {}
        DataBlockStorage::Owned(owned) => {
            storage.kind = JS_ARRAY_BUFFER_STORAGE_OWNED;
            storage.handle = owned.handle();
        }
        DataBlockStorage::External(external) => {
            storage.kind = JS_ARRAY_BUFFER_STORAGE_EXTERNAL;
            storage.handle = external.handle();
            storage.external = JSExternalPrimitiveStorage {
                owner: external.owner().as_ptr(),
                handle: external.handle(),
                fixed_byte_length: external.fixed_byte_length().unwrap_or(0),
                has_fixed_byte_length: external.fixed_byte_length().is_some(),
            };
        }
        DataBlockStorage::Shared(shared) => {
            storage.kind = JS_ARRAY_BUFFER_STORAGE_SHARED_MEMORY;
            storage.handle = shared.handle();
            storage.shared_memory_object_id = shared.object_id();
        }
    });
    // SAFETY: The caller passes a valid out parameter.
    unsafe { out.write(storage) };
}

/// A new descriptor of the shared memory object behind a buffer of kind JS_ARRAY_BUFFER_STORAGE_SHARED_MEMORY, which
/// the caller owns and has to close, with the byte length of the buffer in `out_byte_length`. This is what the
/// embedder sends another agent, along with the id from js_array_buffer_storage(). Returns -1 for any other buffer, or
/// when no descriptor is left. Main thread only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer, and `out_byte_length` valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_duplicate_shared_memory(
    buffer: *mut JSObject,
    out_byte_length: *mut usize,
) -> c_int {
    assert!(!out_byte_length.is_null(), "the embedder passes an out parameter");
    // SAFETY: The caller passes a live buffer.
    let buffer = unsafe { array_buffer_from_abi(buffer) };
    let duplicate = buffer.with_data_block(|block| match &block.byte_buffer {
        DataBlockStorage::Shared(shared) => shared
            .shared_memory()
            .try_clone_to_owned()
            .ok()
            .map(|descriptor| (descriptor, shared.size())),
        _ => None,
    });
    let Some((descriptor, byte_length)) = duplicate else {
        return -1;
    };
    // SAFETY: The caller passes a valid out parameter.
    unsafe { out_byte_length.write(byte_length) };
    into_raw_descriptor(descriptor)
}

/// [[ArrayBufferByteLength]], which is 0 for a detached buffer. Main thread only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_byte_length(buffer: *mut JSObject) -> usize {
    // SAFETY: The caller passes a live buffer.
    unsafe { array_buffer_from_abi(buffer) }.byte_length()
}

/// [[ArrayBufferMaxByteLength]], which only a buffer that is not fixed-length has. Main thread only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer that is not fixed-length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_max_byte_length(buffer: *mut JSObject) -> usize {
    // SAFETY: The caller passes a live buffer.
    unsafe { array_buffer_from_abi(buffer) }.max_byte_length()
}

/// Gives the buffer an [[ArrayBufferMaxByteLength]], which makes it resizable, or growable if it is shared, and its
/// typed arrays stop caching where its bytes are. Main thread only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_set_max_byte_length(buffer: *mut JSObject, max_byte_length: usize) {
    // SAFETY: The caller passes a live buffer.
    unsafe { array_buffer_from_abi(buffer) }.set_max_byte_length(max_byte_length);
}

/// IsFixedLengthArrayBuffer. Main thread only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_is_fixed_length(buffer: *mut JSObject) -> bool {
    // SAFETY: The caller passes a live buffer.
    unsafe { array_buffer_from_abi(buffer) }.is_fixed_length()
}

/// IsDetachedBuffer. Main thread only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_is_detached(buffer: *mut JSObject) -> bool {
    // SAFETY: The caller passes a live buffer.
    unsafe { array_buffer_from_abi(buffer) }.is_detached()
}

/// IsSharedArrayBuffer, which is false for a detached buffer. Main thread only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_is_shared_array_buffer(buffer: *mut JSObject) -> bool {
    // SAFETY: The caller passes a live buffer.
    unsafe { array_buffer_from_abi(buffer) }.is_shared_array_buffer()
}

/// The first byte of the buffer's data, with the byte length in `out_byte_length`, or null with a length of 0 for a
/// detached buffer and one without bytes. The pointer stays valid until the buffer is detached, resized or given new
/// storage, or its owner moves external storage. Bytes of a SharedArrayBuffer may change at any time. Main thread
/// only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer, and `out_byte_length` valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_data(buffer: *mut JSObject, out_byte_length: *mut usize) -> *mut u8 {
    assert!(!out_byte_length.is_null(), "the embedder passes an out parameter");
    // SAFETY: The caller passes a live buffer.
    let buffer = unsafe { array_buffer_from_abi(buffer) };
    let (data, byte_length) = buffer.with_data_block(|block| match block.byte_buffer {
        DataBlockStorage::Empty => (core::ptr::null_mut(), 0),
        _ if block.size() == 0 => (core::ptr::null_mut(), 0),
        _ => (block.data_at(0), block.size()),
    });
    // SAFETY: The caller passes a valid out parameter.
    unsafe { out_byte_length.write(byte_length) };
    data
}

/// DetachArrayBuffer(buffer, key), with undefined as the key of a caller that has none. Throws a TypeError if the key
/// is not the buffer's [[ArrayBufferDetachKey]]. Main thread only.
///
/// # Safety
///
/// `vm` must be live, and `buffer` a live ArrayBuffer that is not a SharedArrayBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_detach(vm: *mut JSVM, buffer: *mut JSObject, key: JSValue) -> JSCompletion {
    // SAFETY: The caller passes a live VM and buffer.
    let (vm, buffer) = unsafe { (vm_from_abi(vm), array_buffer_from_abi(buffer)) };
    completion_into_abi(detach_array_buffer(vm, buffer, Some(Value(key))))
}

/// ArrayBuffer::detach_and_take_data_block(): detaches `buffer`, as DetachArrayBuffer(buffer) does, and describes the
/// storage of the data block it had in `out`, which the caller takes over. The caller owns the storage of a block of
/// kind JS_ARRAY_BUFFER_STORAGE_OWNED, and frees it with gc_primitive_storage_free() or gives it to
/// js_array_buffer_create_adopting_storage(), and keeps the owner of JS_ARRAY_BUFFER_STORAGE_EXTERNAL storage alive
/// itself. Throws a TypeError, and leaves the buffer as it was, if the buffer has a detach key. Main thread only.
///
/// # Safety
///
/// `vm` must be live, `buffer` a live ArrayBuffer that is not a SharedArrayBuffer, and `out` valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_detach_and_take_storage(
    vm: *mut JSVM,
    buffer: *mut JSObject,
    out: *mut JSArrayBufferStorage,
) -> JSCompletion {
    // SAFETY: The caller passes a live VM and buffer.
    let (vm, buffer) = unsafe { (vm_from_abi(vm), array_buffer_from_abi(buffer)) };
    let taken_storage = buffer.detach_and_take_data_block(vm).map(|block| {
        let mut storage = JSArrayBufferStorage {
            kind: JS_ARRAY_BUFFER_STORAGE_DETACHED,
            handle: GC_PRIMITIVE_STORAGE_NULL_HANDLE,
            external: JSExternalPrimitiveStorage {
                owner: core::ptr::null_mut(),
                handle: GC_PRIMITIVE_STORAGE_NULL_HANDLE,
                fixed_byte_length: 0,
                has_fixed_byte_length: false,
            },
            shared_memory_object_id: 0,
        };
        match block.byte_buffer {
            DataBlockStorage::Empty => {}
            DataBlockStorage::Owned(owned) => {
                storage.kind = JS_ARRAY_BUFFER_STORAGE_OWNED;
                storage.handle = owned.into_handle();
            }
            DataBlockStorage::External(external) => {
                storage.kind = JS_ARRAY_BUFFER_STORAGE_EXTERNAL;
                storage.handle = external.handle();
                storage.external = JSExternalPrimitiveStorage {
                    owner: external.owner().as_ptr(),
                    handle: external.handle(),
                    fixed_byte_length: external.fixed_byte_length().unwrap_or(0),
                    has_fixed_byte_length: external.fixed_byte_length().is_some(),
                };
            }
            DataBlockStorage::Shared(_) => unreachable!("a SharedArrayBuffer is never detached"),
        }
        storage
    });
    // SAFETY: The caller passes a valid out parameter.
    unsafe { completion_writing_result_to(taken_storage, out) }
}

/// ArrayBuffer::create(Realm&, DataBlock) over storage that the embedder owns and gives up: a fixed-length buffer of
/// the bytes `handle` names, or of none for GC_PRIMITIVE_STORAGE_NULL_HANDLE, and a SharedArrayBuffer if `shared`.
/// The buffer frees the storage when it no longer needs it. Main thread only.
///
/// # Safety
///
/// `vm` and `realm` must be live, and `handle` the null handle or one that names live storage that nothing else
/// resizes or frees from now on.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_create_adopting_storage(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    handle: u64,
    shared: bool,
) -> *mut JSObject {
    // SAFETY: The caller passes a live VM and realm.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi(realm)) };
    // SAFETY: The caller gives up the storage.
    let owned = unsafe { OwnedBackingStore::adopt_handle(handle) };
    let block = DataBlock::new(owned, shared_from_abi(shared));
    object_into_abi(ArrayBuffer::create_from_data_block(vm, realm, block))
}

/// CloneArrayBuffer(srcBuffer, srcByteOffset, srcLength, %ArrayBuffer%), whose new buffer of the current realm is the
/// payload of the normal completion. Main thread only.
///
/// # Safety
///
/// `vm` must be live with a realm on its execution context stack, and `buffer` a live ArrayBuffer that is not detached
/// and has `length` bytes from `byte_offset` on.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_clone(
    vm: *mut JSVM,
    buffer: *mut JSObject,
    byte_offset: usize,
    length: usize,
) -> JSCompletion {
    // SAFETY: The caller passes a live VM and buffer.
    let (vm, buffer) = unsafe { (vm_from_abi(vm), array_buffer_from_abi(buffer)) };
    completion_into_abi(clone_array_buffer(vm, buffer, byte_offset, length))
}

/// [[ArrayBufferDetachKey]], which is undefined unless the embedder set one. Main thread only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_detach_key(buffer: *mut JSObject) -> JSValue {
    // SAFETY: The caller passes a live buffer.
    unsafe { array_buffer_from_abi(buffer) }.detach_key().0
}

/// Sets [[ArrayBufferDetachKey]], which the buffer keeps alive. Main thread only.
///
/// # Safety
///
/// `buffer` must be a live ArrayBuffer, and `key` a value of the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_set_detach_key(buffer: *mut JSObject, key: JSValue) {
    // SAFETY: The caller passes a live buffer.
    unsafe { array_buffer_from_abi(buffer) }.set_detach_key(Value(key));
}

/// DataView::create(): a view of `buffer` from `byte_offset` on, of `byte_length` bytes, or of the rest of a resizable
/// buffer for a byte length of kind JS_BYTE_LENGTH_AUTO. The caller has checked that the view fits in the buffer. Main
/// thread only.
///
/// # Safety
///
/// `vm` and `realm` must be live, and `buffer` a live ArrayBuffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_create_data_view(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    buffer: *mut JSObject,
    byte_length: JSByteLength,
    byte_offset: usize,
) -> *mut JSObject {
    // SAFETY: The caller passes a live VM, realm and buffer.
    let (vm, realm, buffer) = unsafe { (vm_from_abi(vm), cell_from_abi(realm), array_buffer_from_abi(buffer)) };
    object_into_abi(DataView::create(
        vm,
        realm,
        buffer,
        byte_length_from_abi(byte_length),
        byte_offset,
    ))
}

/// A DataView's [[ViewedArrayBuffer]]. Main thread only.
///
/// # Safety
///
/// `data_view` must be a live DataView.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_data_view_viewed_buffer(data_view: *mut JSObject) -> *mut JSObject {
    // SAFETY: The caller passes a live DataView.
    object_into_abi(unsafe { data_view_from_abi(data_view) }.viewed_array_buffer())
}

/// A DataView's [[ByteOffset]], truncated to the 32 bits that DataView::byte_offset() returns. Main thread only.
///
/// # Safety
///
/// `data_view` must be a live DataView.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_data_view_byte_offset(data_view: *mut JSObject) -> u32 {
    // SAFETY: The caller passes a live DataView.
    unsafe { data_view_from_abi(data_view) }.byte_offset()
}

/// A DataView's [[ByteLength]] slot, which is auto for a view that tracks a resizable buffer. Main thread only.
///
/// # Safety
///
/// `data_view` must be a live DataView.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_data_view_byte_length(data_view: *mut JSObject) -> JSByteLength {
    // SAFETY: The caller passes a live DataView.
    byte_length_into_abi(unsafe { data_view_from_abi(data_view) }.byte_length())
}

/// MakeDataViewWithBufferWitnessRecord(view, order), with an order of JS_ARRAY_BUFFER_ORDER_*. Main thread only.
///
/// # Safety
///
/// `data_view` must be a live DataView.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_make_data_view_witness_record(
    data_view: *mut JSObject,
    order: u8,
) -> JSDataViewWithBufferWitness {
    // SAFETY: The caller passes a live DataView.
    let record =
        make_data_view_with_buffer_witness_record(unsafe { data_view_from_abi(data_view) }, order_from_abi(order));
    JSDataViewWithBufferWitness {
        data_view: object_into_abi(record.object),
        cached_buffer_byte_length: byte_length_into_abi(record.cached_buffer_byte_length),
    }
}

/// # Safety
///
/// `record` must point to a record of a live DataView.
unsafe fn data_view_witness_record_from_abi(record: *const JSDataViewWithBufferWitness) -> DataViewWithBufferWitness {
    assert!(!record.is_null(), "the embedder passes a witness record");
    // SAFETY: The caller passes a valid record.
    let record = unsafe { &*record };
    DataViewWithBufferWitness {
        // SAFETY: The caller passes the record of a live DataView.
        object: unsafe { data_view_from_abi(record.data_view) },
        cached_buffer_byte_length: byte_length_from_abi(record.cached_buffer_byte_length),
    }
}

/// IsViewOutOfBounds(viewRecord). Main thread only.
///
/// # Safety
///
/// `record` must point to a record of a live DataView.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_is_data_view_out_of_bounds(
    record: *const JSDataViewWithBufferWitness,
) -> bool {
    // SAFETY: The caller passes a valid record.
    is_view_out_of_bounds(&unsafe { data_view_witness_record_from_abi(record) })
}

/// GetViewByteLength(viewRecord), of a view that is not out of bounds. Main thread only.
///
/// # Safety
///
/// `record` must point to a record of a live DataView that is not out of bounds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_buffer_data_view_view_byte_length(record: *const JSDataViewWithBufferWitness) -> u32 {
    // SAFETY: The caller passes a valid record.
    get_view_byte_length(&unsafe { data_view_witness_record_from_abi(record) })
}
