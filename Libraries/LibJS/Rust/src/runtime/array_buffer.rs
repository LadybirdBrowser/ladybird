/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ffi::c_void;
use core::num::NonZeroU64;
use core::ops::Deref;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicU8, AtomicU16, AtomicU32, AtomicU64, Ordering};
use core::time::Duration;

use libjs_runtime_macros::Trace;
use num_bigint::BigInt as NumBigInt;

use crate::futex::{self, AtomicWaitResult};
use crate::gc::capi::{GC_PRIMITIVE_STORAGE_NULL_HANDLE, GCPrimitiveStorageHandle, gc_cell_type_info};
use crate::gc::class::{ExternalMemorySize, Finalize, GcCell, define_cell};
use crate::gc::foreign::ForeignCellSlot;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::heap::Heap;
use crate::gc::primitive_storage::{ForeignPrimitiveStorage, OwnedPrimitiveStorage, create_shared_memory};
use crate::gc::shared_memory::{AsSharedMemory, BorrowedSharedMemory, OwnedSharedMemory, SharedMemoryViewOutsideCage};
use crate::gc::visitor::{Trace, Visitor};
use crate::gc::weak::GcWeak;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::random::get_random_u64;
use crate::runtime::abstract_operations::ordinary_create_from_constructor_of;
use crate::runtime::big_int::BigInt;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::math_object::round_to_binary16;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;
use crate::runtime::typed_array::TypedArrayBase;
use crate::runtime::value::same_value;
use crate::runtime::value_conversions::MAX_ARRAY_LIKE_INDEX;

pub use crate::gc::primitive_storage::cage_base as primitive_storage_cage_base;
pub use crate::gc::primitive_storage::{OutOfMemory, ZeroFillNewBytes};

/// GC::PrimitiveStorage::invalid_offset, the offset of a data block without bytes in the cage.
pub const INVALID_DATA_OFFSET: usize = crate::gc::primitive_storage::INVALID_OFFSET;

/// Table 71: The element types.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ElementType {
    Uint8,
    Uint8Clamped,
    Uint16,
    Uint32,
    BigUint64,
    Int8,
    Int16,
    Int32,
    BigInt64,
    Float16,
    Float32,
    Float64,
}

impl ElementType {
    /// The Element Size value specified in Table 71 for the Element Type.
    pub const fn size(self) -> usize {
        match self {
            Self::Uint8 | Self::Uint8Clamped | Self::Int8 => 1,
            Self::Uint16 | Self::Int16 | Self::Float16 => 2,
            Self::Uint32 | Self::Int32 | Self::Float32 => 4,
            Self::BigUint64 | Self::BigInt64 | Self::Float64 => 8,
        }
    }

    fn is_bigint(self) -> bool {
        matches!(self, Self::BigUint64 | Self::BigInt64)
    }
}

/// 25.1.1 Notation (read-modify-write modification function), https://tc39.es/ecma262/#sec-arraybuffer-notation
/// NB: The spec's modification function is an abstract closure. The Atomics functions only ever pass these operations,
///     which the buffer performs itself so that the atomic access to its memory stays here.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReadWriteModifyOperation {
    Add,
    And,
    Exchange,
    Or,
    Sub,
    Xor,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PreserveResizability {
    FixedLength,
    PreserveResizability,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Order {
    SeqCst,
    Unordered,
}

/// DataBlock::Shared.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Shared {
    No,
    Yes,
}

/// DataBlock::OwnedBackingStore: storage in the primitive storage cage, or none at all for a data block of zero
/// bytes, like an invalid handle.
pub struct OwnedBackingStore {
    storage: Option<OwnedPrimitiveStorage>,
}

impl OwnedBackingStore {
    fn empty() -> Self {
        Self { storage: None }
    }

    fn allocate(size: usize, zero_fill_new_bytes: ZeroFillNewBytes) -> Result<Self, OutOfMemory> {
        if size == 0 {
            return Ok(Self::empty());
        }
        Ok(Self {
            storage: Some(OwnedPrimitiveStorage::allocate(size, zero_fill_new_bytes)?),
        })
    }

    pub fn create_zeroed(size: usize) -> Result<Self, OutOfMemory> {
        Self::allocate(size, ZeroFillNewBytes::Yes)
    }

    pub fn create_uninitialized(size: usize) -> Result<Self, OutOfMemory> {
        Self::allocate(size, ZeroFillNewBytes::No)
    }

    pub fn create_zeroed_with_capacity(size: usize, capacity: usize) -> Result<Self, OutOfMemory> {
        if capacity == 0 {
            return Ok(Self::empty());
        }
        Ok(Self {
            storage: Some(OwnedPrimitiveStorage::reserve(size, capacity, ZeroFillNewBytes::Yes)?),
        })
    }

    /// Takes over storage that an embedder gives up, or none for the null handle.
    ///
    /// # Safety
    ///
    /// A handle other than the null handle must name live storage that nothing else resizes or frees from now on.
    pub unsafe fn adopt_handle(handle: GCPrimitiveStorageHandle) -> Self {
        Self {
            // SAFETY: The caller gives up the storage.
            storage: NonZeroU64::new(handle).map(|handle| unsafe { OwnedPrimitiveStorage::adopt(handle) }),
        }
    }

    /// Gives up the storage without freeing it, for an embedder that takes it over, and returns its handle, or the null
    /// handle if there are no bytes.
    pub fn into_handle(self) -> GCPrimitiveStorageHandle {
        self.storage
            .map_or(GC_PRIMITIVE_STORAGE_NULL_HANDLE, |storage| storage.into_handle().get())
    }

    /// The handle of the storage, or the null handle while there are no bytes.
    pub fn handle(&self) -> GCPrimitiveStorageHandle {
        self.storage
            .as_ref()
            .map_or(GC_PRIMITIVE_STORAGE_NULL_HANDLE, OwnedPrimitiveStorage::handle)
    }

    #[inline]
    pub fn size(&self) -> usize {
        self.storage.as_ref().map_or(0, OwnedPrimitiveStorage::size)
    }

    #[inline]
    pub fn capacity(&self) -> usize {
        self.storage.as_ref().map_or(0, OwnedPrimitiveStorage::capacity)
    }

    #[inline]
    fn data(&self) -> *mut u8 {
        self.storage
            .as_ref()
            .map_or(core::ptr::null_mut(), OwnedPrimitiveStorage::data)
    }

    #[inline]
    pub fn offset(&self) -> usize {
        self.storage
            .as_ref()
            .map_or(INVALID_DATA_OFFSET, OwnedPrimitiveStorage::offset)
    }

    pub fn set_size(&mut self, new_size: usize, zero_fill_new_bytes: ZeroFillNewBytes) {
        assert!(new_size <= self.capacity());
        self.try_resize(new_size, zero_fill_new_bytes)
            .expect("resizing within the capacity succeeds");
    }

    pub fn try_resize(&mut self, new_size: usize, zero_fill_new_bytes: ZeroFillNewBytes) -> Result<(), OutOfMemory> {
        match &mut self.storage {
            Some(storage) => storage.resize(new_size, zero_fill_new_bytes),
            None => {
                *self = Self::allocate(new_size, zero_fill_new_bytes)?;
                Ok(())
            }
        }
    }

    pub fn try_ensure_capacity(&mut self, new_capacity: usize) -> Result<(), OutOfMemory> {
        if new_capacity <= self.capacity() {
            return Ok(());
        }
        match &mut self.storage {
            Some(storage) => storage.reserve_capacity(new_capacity),
            None => {
                self.storage = Some(OwnedPrimitiveStorage::reserve(0, new_capacity, ZeroFillNewBytes::No)?);
                Ok(())
            }
        }
    }
}

/// DataBlock::ExternalPrimitiveStorage: storage that a cell of the embedder owns, such as a wasm memory or the channel
/// of an AudioBuffer, which the block keeps alive. The byte length is either fixed when the block is made, or the size
/// of the storage, which follows the owner's resizing.
pub struct ExternalPrimitiveStorage {
    owner: ForeignCellSlot,
    storage: ForeignPrimitiveStorage,
    fixed_byte_length: Option<usize>,
}

impl ExternalPrimitiveStorage {
    /// `owner` holds the cell that owns the storage.
    pub fn new(owner: ForeignCellSlot, handle: GCPrimitiveStorageHandle, fixed_byte_length: Option<usize>) -> Self {
        assert!(owner.get().is_some(), "external storage has an owner");
        Self {
            owner,
            storage: ForeignPrimitiveStorage::new(handle),
            fixed_byte_length,
        }
    }

    pub fn owner(&self) -> NonNull<c_void> {
        self.owner.get().expect("external storage has an owner")
    }

    pub fn handle(&self) -> GCPrimitiveStorageHandle {
        self.storage.handle()
    }

    pub fn fixed_byte_length(&self) -> Option<usize> {
        self.fixed_byte_length
    }

    fn byte_length(&self) -> usize {
        self.fixed_byte_length.unwrap_or_else(|| self.storage.size())
    }

    /// The owner if it is an ArrayBuffer of the runtime that owns the storage, as when structured deserialization
    /// makes a growable SharedArrayBuffer that shares the storage of another one. Resizing such storage has to go
    /// through the owner, which keeps the layout of storage it owns at hand.
    fn array_buffer_owning_the_storage(&self) -> Option<Gc<ArrayBuffer>> {
        let owner = self.owner();
        // NB: The owner may be a C++ cell, so its type is looked up through LibGC instead of read from the cell.
        // SAFETY: The block keeps its owner, a cell of the VM's heap, alive.
        let type_info = unsafe { gc_cell_type_info(owner.as_ptr()) };
        if !core::ptr::eq(type_info, &raw const ArrayBuffer::CLASS.type_info) {
            return None;
        }
        // SAFETY: LibGC allocated the owner as an ArrayBuffer.
        let owner = unsafe { Gc::from_non_null(owner.cast::<ArrayBuffer>()) };
        let owns_the_storage = owner.with_data_block(
            |block| matches!(&block.byte_buffer, DataBlockStorage::Owned(buffer) if buffer.handle() == self.handle()),
        );
        owns_the_storage.then_some(owner)
    }
}

/// The id of a new shared memory object is drawn at random, as it has to stay unique across processes that never
/// coordinate. Zero means that a block has no id.
fn mint_shared_object_id() -> u64 {
    loop {
        let id = get_random_u64();
        if id != 0 {
            return id;
        }
    }
}

/// DataBlock::SharedBackingStore: the bytes of a fixed-length Shared Data Block in a shared memory object, which agents
/// in other processes map too. The block keeps a descriptor of the object to hand it on, and the id that names the
/// object in every agent, since one object that is mapped twice has two addresses.
pub struct SharedBackingStore {
    shared_memory: OwnedSharedMemory,
    object_id: u64,
    mapping: SharedMemoryMapping,
}

/// Like every other backing store, shared memory is mapped into the cage, so that an out-of-bounds access through the
/// buffer is masked back into it. When the cage cannot take the mapping, as on Windows, which cannot place one inside
/// it, the object is mapped outside of it instead, where the views of the buffer reach the bytes through the slow path.
enum SharedMemoryMapping {
    InCage(OwnedPrimitiveStorage),
    OutsideCage(SharedMemoryViewOutsideCage),
}

impl SharedMemoryMapping {
    fn map(shared_memory: BorrowedSharedMemory<'_>, size: usize) -> Result<Self, OutOfMemory> {
        if let Ok(storage) = OwnedPrimitiveStorage::adopt_shared_memory(shared_memory, size) {
            return Ok(Self::InCage(storage));
        }
        let view = SharedMemoryViewOutsideCage::map(shared_memory, size)?;
        Ok(Self::OutsideCage(view))
    }
}

/// Whether the shared memory object has at least `size` bytes, as accessing a mapping beyond the end of its object
/// raises SIGBUS. A Windows section is not a file, so it has no size to ask for, but MapViewOfFile() refuses a view
/// that extends past its end.
fn shared_memory_object_has_at_least(shared_memory: &std::fs::File, size: usize) -> bool {
    #[cfg(unix)]
    {
        shared_memory
            .metadata()
            .is_ok_and(|metadata| metadata.len() >= size as u64)
    }
    #[cfg(windows)]
    {
        let _ = (shared_memory, size);
        true
    }
}

impl SharedBackingStore {
    /// A new zero-filled shared memory object of `size` bytes, named by a new id.
    pub fn create(size: usize) -> Result<Self, OutOfMemory> {
        let shared_memory = create_shared_memory(size)?;
        let mapping = SharedMemoryMapping::map(shared_memory.as_shared_memory(), size)?;
        Ok(Self {
            shared_memory,
            object_id: mint_shared_object_id(),
            mapping,
        })
    }

    /// The first `size` bytes of a shared memory object that another agent made and `object_id` names. The store keeps
    /// a duplicate of the descriptor. Fails if `size` is 0 or the object is smaller than that.
    pub fn adopt(shared_memory: BorrowedSharedMemory<'_>, size: usize, object_id: u64) -> Result<Self, OutOfMemory> {
        let shared_memory = std::fs::File::from(shared_memory.try_clone_to_owned().map_err(|_| OutOfMemory)?);
        if !shared_memory_object_has_at_least(&shared_memory, size) {
            return Err(OutOfMemory);
        }
        let mapping = SharedMemoryMapping::map(shared_memory.as_shared_memory(), size)?;
        Ok(Self {
            shared_memory: shared_memory.into(),
            object_id,
            mapping,
        })
    }

    pub fn shared_memory(&self) -> BorrowedSharedMemory<'_> {
        self.shared_memory.as_shared_memory()
    }

    pub fn object_id(&self) -> u64 {
        self.object_id
    }

    /// The handle of the storage in the cage, or the null handle for a mapping outside of it.
    pub fn handle(&self) -> GCPrimitiveStorageHandle {
        match &self.mapping {
            SharedMemoryMapping::InCage(storage) => storage.handle(),
            SharedMemoryMapping::OutsideCage(_) => GC_PRIMITIVE_STORAGE_NULL_HANDLE,
        }
    }

    pub fn size(&self) -> usize {
        match &self.mapping {
            SharedMemoryMapping::InCage(storage) => storage.size(),
            SharedMemoryMapping::OutsideCage(view) => view.size(),
        }
    }

    fn data(&self) -> *mut u8 {
        match &self.mapping {
            SharedMemoryMapping::InCage(storage) => storage.data(),
            SharedMemoryMapping::OutsideCage(view) => view.data(),
        }
    }

    fn offset(&self) -> usize {
        match &self.mapping {
            SharedMemoryMapping::InCage(storage) => storage.offset(),
            SharedMemoryMapping::OutsideCage(_) => INVALID_DATA_OFFSET,
        }
    }

    fn is_caged(&self) -> bool {
        matches!(self.mapping, SharedMemoryMapping::InCage(_))
    }
}

/// Copies `count` bytes; the ranges may overlap.
///
/// # Safety
///
/// Both pointers must have `count` accessible bytes.
unsafe fn move_bytes(destination: *mut u8, source: *const u8, count: usize) {
    if count == 0 {
        return;
    }
    assert!(!destination.is_null() && !source.is_null());
    // SAFETY: The caller guarantees that both ranges are accessible.
    unsafe { core::ptr::copy(source, destination, count) };
}

/// The storage of a data block.
pub enum DataBlockStorage {
    Empty,
    Owned(OwnedBackingStore),
    External(ExternalPrimitiveStorage),
    Shared(SharedBackingStore),
}

// 6.2.9 Data Blocks, https://tc39.es/ecma262/#sec-data-blocks
pub struct DataBlock {
    pub byte_buffer: DataBlockStorage,
    pub is_shared: Shared,
}

// SAFETY: The owner of external storage is the only cell a data block refers to.
unsafe impl Trace for DataBlock {
    fn trace(&self, visitor: &mut Visitor) {
        if let DataBlockStorage::External(storage) = &self.byte_buffer {
            storage.owner.trace(visitor);
        }
    }
}

impl DataBlock {
    pub fn new(buffer: OwnedBackingStore, is_shared: Shared) -> Self {
        Self {
            byte_buffer: DataBlockStorage::Owned(buffer),
            is_shared,
        }
    }

    pub fn external(storage: ExternalPrimitiveStorage, is_shared: Shared) -> Self {
        Self {
            byte_buffer: DataBlockStorage::External(storage),
            is_shared,
        }
    }

    /// A Shared Data Block in shared memory.
    pub fn shared_memory(store: SharedBackingStore) -> Self {
        Self {
            byte_buffer: DataBlockStorage::Shared(store),
            is_shared: Shared::Yes,
        }
    }

    pub fn empty(is_shared: Shared) -> Self {
        Self {
            byte_buffer: DataBlockStorage::Empty,
            is_shared,
        }
    }

    /// Only owned storage can be resized: an embedder resizes the storage it owns, and shared memory keeps its size.
    fn owned_mut(&mut self) -> &mut OwnedBackingStore {
        match &mut self.byte_buffer {
            DataBlockStorage::Empty => unreachable!("the data block is detached"),
            DataBlockStorage::Owned(buffer) => buffer,
            DataBlockStorage::External(_) | DataBlockStorage::Shared(_) => {
                unreachable!("only the runtime's own storage is resized")
            }
        }
    }

    /// The first byte, which is null for an owned block of zero bytes and for external storage that the embedder freed.
    /// Blocks the runtime owns take the inline path; the others are rare enough to take a call.
    #[inline]
    fn data(&self) -> *mut u8 {
        match &self.byte_buffer {
            DataBlockStorage::Owned(buffer) => buffer.data(),
            _ => self.data_of_storage_the_block_does_not_own(),
        }
    }

    #[cold]
    #[inline(never)]
    fn data_of_storage_the_block_does_not_own(&self) -> *mut u8 {
        match &self.byte_buffer {
            DataBlockStorage::Empty => unreachable!("the data block is detached"),
            DataBlockStorage::Owned(buffer) => buffer.data(),
            DataBlockStorage::External(storage) => storage.storage.data(),
            DataBlockStorage::Shared(store) => store.data(),
        }
    }

    #[inline]
    pub fn data_at(&self, byte_offset: usize) -> *mut u8 {
        let data = self.data();
        if data.is_null() {
            assert!(byte_offset == 0);
            return core::ptr::null_mut();
        }
        // SAFETY: Callers only address bytes within the block.
        unsafe { data.add(byte_offset) }
    }

    #[inline]
    pub fn copy_to(&self, offset: usize, destination: &mut [u8]) {
        let size = self.size();
        assert!(offset <= size);
        assert!(destination.len() <= size - offset);
        // SAFETY: The block has the bytes, and the destination is a distinct Rust buffer.
        unsafe {
            move_bytes(
                destination.as_mut_ptr(),
                self.data_at_or_null(offset),
                destination.len(),
            );
        };
    }

    #[inline]
    pub fn copy_to_block(
        &self,
        destination: &DataBlock,
        source_offset: usize,
        destination_offset: usize,
        count: usize,
    ) {
        let (source_size, destination_size) = (self.size(), destination.size());
        assert!(source_offset <= source_size);
        assert!(count <= source_size - source_offset);
        assert!(destination_offset <= destination_size);
        assert!(count <= destination_size - destination_offset);
        // SAFETY: Both blocks have the bytes.
        unsafe {
            move_bytes(
                destination.data_at_or_null(destination_offset),
                self.data_at_or_null(source_offset),
                count,
            );
        };
    }

    pub fn copy_to_byte_buffer(&self, offset: usize, count: usize) -> Vec<u8> {
        let size = self.size();
        assert!(offset <= size);
        assert!(count <= size - offset);
        let mut destination = vec![0; count];
        self.copy_to(offset, &mut destination);
        destination
    }

    #[inline]
    pub fn overwrite(&self, offset: usize, source: &[u8]) {
        let size = self.size();
        assert!(offset <= size);
        assert!(source.len() <= size - offset);
        // SAFETY: The block has the bytes, and the source is a distinct Rust buffer.
        unsafe { move_bytes(self.data_at_or_null(offset), source.as_ptr(), source.len()) };
    }

    #[inline]
    pub fn move_data(&self, destination_offset: usize, source_offset: usize, count: usize) {
        let size = self.size();
        assert!(destination_offset <= size);
        assert!(count <= size - destination_offset);
        assert!(source_offset <= size);
        assert!(count <= size - source_offset);

        if count == 0 || destination_offset == source_offset {
            return;
        }

        let data = self.data_or_null();
        assert!(!data.is_null(), "a block with bytes to move has storage");
        // SAFETY: The block has both ranges, which may overlap.
        unsafe { core::ptr::copy(data.add(source_offset), data.add(destination_offset), count) };
    }

    /// The first byte, or null for a detached block, whose bytes are never addressed when there are none to copy.
    #[inline]
    fn data_or_null(&self) -> *mut u8 {
        match &self.byte_buffer {
            DataBlockStorage::Owned(buffer) => buffer.data(),
            DataBlockStorage::Empty => core::ptr::null_mut(),
            _ => self.data_of_storage_the_block_does_not_own(),
        }
    }

    /// data_at() for the start of a range of bytes, which may be empty, even in a detached block.
    #[inline]
    fn data_at_or_null(&self, byte_offset: usize) -> *mut u8 {
        let data = self.data_or_null();
        if data.is_null() {
            return data;
        }
        // SAFETY: Callers only address bytes within the block.
        unsafe { data.add(byte_offset) }
    }

    pub fn set_size(&mut self, new_size: usize, zero_fill_new_bytes: ZeroFillNewBytes) {
        self.owned_mut().set_size(new_size, zero_fill_new_bytes);
    }

    pub fn try_resize(&mut self, new_size: usize, zero_fill_new_bytes: ZeroFillNewBytes) -> Result<(), OutOfMemory> {
        self.owned_mut().try_resize(new_size, zero_fill_new_bytes)
    }

    pub fn try_ensure_capacity(&mut self, new_capacity: usize) -> Result<(), OutOfMemory> {
        self.owned_mut().try_ensure_capacity(new_capacity)
    }

    #[inline]
    pub fn size(&self) -> usize {
        match &self.byte_buffer {
            DataBlockStorage::Owned(buffer) => buffer.size(),
            DataBlockStorage::Empty => 0,
            _ => self.size_of_storage_the_block_does_not_own(),
        }
    }

    #[cold]
    #[inline(never)]
    fn size_of_storage_the_block_does_not_own(&self) -> usize {
        match &self.byte_buffer {
            DataBlockStorage::Empty => 0,
            DataBlockStorage::Owned(buffer) => buffer.size(),
            DataBlockStorage::External(storage) => storage.byte_length(),
            DataBlockStorage::Shared(store) => store.size(),
        }
    }

    pub fn capacity(&self) -> usize {
        match &self.byte_buffer {
            DataBlockStorage::Empty => 0,
            DataBlockStorage::Owned(buffer) => buffer.capacity(),
            DataBlockStorage::External(storage) => storage.storage.capacity(),
            DataBlockStorage::Shared(store) => store.size(),
        }
    }

    #[inline]
    pub fn offset(&self) -> usize {
        match &self.byte_buffer {
            DataBlockStorage::Empty => INVALID_DATA_OFFSET,
            DataBlockStorage::Owned(buffer) => buffer.offset(),
            DataBlockStorage::External(storage) => storage.storage.offset(),
            DataBlockStorage::Shared(store) => store.offset(),
        }
    }

    /// Whether the bytes are in the cage. Owned storage always is, as the runtime fails to create a block rather than
    /// fall back to memory outside of it, shared memory is unless the platform cannot map it there, and external
    /// storage is while its handle names storage.
    pub fn is_caged(&self) -> bool {
        match &self.byte_buffer {
            DataBlockStorage::Empty => false,
            DataBlockStorage::Owned(_) => true,
            DataBlockStorage::Shared(store) => store.is_caged(),
            DataBlockStorage::External(storage) => storage.storage.is_valid(),
        }
    }

    /// The memory that the block makes the heap responsible for. External storage is the owner's.
    pub fn external_memory_size(&self) -> usize {
        match &self.byte_buffer {
            DataBlockStorage::Empty | DataBlockStorage::External(_) => 0,
            DataBlockStorage::Owned(buffer) => buffer.capacity(),
            DataBlockStorage::Shared(store) => store.size(),
        }
    }

    pub fn is_external(&self) -> bool {
        matches!(self.byte_buffer, DataBlockStorage::External(_))
    }

    /// Whether the bytes are in shared memory, which an agent in another process may write at any time, so that a
    /// typed array must not cache their offset for the interpreter's plain loads and stores.
    pub fn is_cross_process_shared(&self) -> bool {
        matches!(self.byte_buffer, DataBlockStorage::Shared(_))
    }

    /// The id of the shared memory object, or 0 for a block that is not in shared memory.
    pub fn shared_object_id(&self) -> u64 {
        match &self.byte_buffer {
            DataBlockStorage::Shared(store) => store.object_id(),
            _ => 0,
        }
    }

    pub fn shares_storage_with(&self, other: &DataBlock) -> bool {
        if matches!(self.byte_buffer, DataBlockStorage::Empty) || matches!(other.byte_buffer, DataBlockStorage::Empty) {
            return false;
        }
        // NB: One shared memory object can be mapped at several addresses, so blocks in shared memory are compared by
        //     the id of the object, and one without an id matches nothing.
        if let DataBlockStorage::Shared(store) = &self.byte_buffer {
            return matches!(&other.byte_buffer, DataBlockStorage::Shared(other_store)
                if store.object_id() != 0 && store.object_id() == other_store.object_id());
        }
        if matches!(other.byte_buffer, DataBlockStorage::Shared(_)) {
            return false;
        }
        // NB: Two blocks of zero bytes share the storage they do not have.
        core::ptr::eq(self.data(), other.data())
    }

    fn atomic_load(&self, byte_index: usize, size: usize, ordering: Ordering) -> u64 {
        let address = self.data_at(byte_index);
        assert!(address.addr().is_multiple_of(size));
        // SAFETY: The block has the aligned bytes, which are only ever accessed atomically while they are shared.
        unsafe {
            match size {
                1 => u64::from(AtomicU8::from_ptr(address).load(ordering)),
                2 => u64::from(AtomicU16::from_ptr(address.cast()).load(ordering)),
                4 => u64::from(AtomicU32::from_ptr(address.cast()).load(ordering)),
                8 => AtomicU64::from_ptr(address.cast()).load(ordering),
                _ => unreachable!("atomic accesses are 1, 2, 4 or 8 bytes wide"),
            }
        }
    }

    fn atomic_store(&self, byte_index: usize, size: usize, value: u64, ordering: Ordering) {
        let address = self.data_at(byte_index);
        assert!(address.addr().is_multiple_of(size));
        // SAFETY: As for atomic_load(). The value is truncated to the access size.
        unsafe {
            match size {
                1 => AtomicU8::from_ptr(address).store(value as u8, ordering),
                2 => AtomicU16::from_ptr(address.cast()).store(value as u16, ordering),
                4 => AtomicU32::from_ptr(address.cast()).store(value as u32, ordering),
                8 => AtomicU64::from_ptr(address.cast()).store(value, ordering),
                _ => unreachable!("atomic accesses are 1, 2, 4 or 8 bytes wide"),
            }
        }
    }

    fn atomic_read_modify_write(
        &self,
        byte_index: usize,
        size: usize,
        operation: ReadWriteModifyOperation,
        operand: u64,
    ) -> u64 {
        macro_rules! perform {
            ($atomic:ty, $address:expr, $operand:expr) => {{
                // SAFETY: As for atomic_load().
                let atomic = unsafe { <$atomic>::from_ptr($address) };
                let ordering = Ordering::SeqCst;
                u64::from(match operation {
                    ReadWriteModifyOperation::Add => atomic.fetch_add($operand, ordering),
                    ReadWriteModifyOperation::And => atomic.fetch_and($operand, ordering),
                    ReadWriteModifyOperation::Exchange => atomic.swap($operand, ordering),
                    ReadWriteModifyOperation::Or => atomic.fetch_or($operand, ordering),
                    ReadWriteModifyOperation::Sub => atomic.fetch_sub($operand, ordering),
                    ReadWriteModifyOperation::Xor => atomic.fetch_xor($operand, ordering),
                })
            }};
        }

        let address = self.data_at(byte_index);
        assert!(address.addr().is_multiple_of(size));
        match size {
            1 => perform!(AtomicU8, address, operand as u8),
            2 => perform!(AtomicU16, address.cast(), operand as u16),
            4 => perform!(AtomicU32, address.cast(), operand as u32),
            8 => perform!(AtomicU64, address.cast(), operand),
            _ => unreachable!("atomic accesses are 1, 2, 4 or 8 bytes wide"),
        }
    }

    fn atomic_compare_exchange(&self, byte_index: usize, size: usize, expected: u64, replacement: u64) -> u64 {
        macro_rules! perform {
            ($atomic:ty, $type:ty, $address:expr) => {{
                // SAFETY: As for atomic_load().
                let atomic = unsafe { <$atomic>::from_ptr($address) };
                let observed = atomic.compare_exchange(
                    expected as $type,
                    replacement as $type,
                    Ordering::SeqCst,
                    Ordering::SeqCst,
                );
                u64::from(observed.unwrap_or_else(|observed| observed))
            }};
        }

        let address = self.data_at(byte_index);
        assert!(address.addr().is_multiple_of(size));
        match size {
            1 => perform!(AtomicU8, u8, address),
            2 => perform!(AtomicU16, u16, address.cast()),
            4 => perform!(AtomicU32, u32, address.cast()),
            8 => perform!(AtomicU64, u64, address.cast()),
            _ => unreachable!("atomic accesses are 1, 2, 4 or 8 bytes wide"),
        }
    }
}

/// A large allocation that fails may succeed once dead buffers pinning address space are collected, so it is retried
/// once after a collection before giving up, as WebKit does.
fn allocate_or_retry_after_gc<T>(heap: &Heap, allocate: impl Fn() -> Result<T, OutOfMemory>) -> Result<T, OutOfMemory> {
    let result = allocate();
    if result.is_err() {
        heap.collect_garbage();
        return allocate();
    }
    result
}

#[repr(C)]
#[derive(Trace)]
pub struct ArrayBuffer {
    base: Object,
    data_block: GcRefCell<DataBlock>,
    max_byte_length: Cell<Option<usize>>,
    // The various detach related members of ArrayBuffer are not used by any ECMA262 functionality,
    // but are required to be available for the use of various harnesses like the Test262 test runner.
    detach_key: Cell<Value>,
    /// The views that cache the offset of this buffer's data in the cage, which has to be invalidated once the data
    /// moves or goes away.
    cached_views: GcRefCell<Vec<GcWeak<TypedArrayBase>>>,
}

define_cell!(
    ArrayBuffer,
    Object,
    extends: [Object],
    finalize: finalize,
    external_memory_size: external_memory_size
);

impl Deref for ArrayBuffer {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for ArrayBuffer {
    fn finalize(&self) {
        let is_shared = self.data_block.borrow().is_shared;
        drop(self.data_block.replace(DataBlock::empty(is_shared)));
    }
}

impl ExternalMemorySize for ArrayBuffer {
    fn external_memory_size(&self) -> usize {
        self.data_block.borrow().external_memory_size()
    }
}

fn prototype_for_shared_state(vm: &Vm, realm: Gc<Realm>, is_shared: Shared) -> Gc<Object> {
    match is_shared {
        Shared::No => realm.intrinsics().array_buffer_prototype(vm),
        Shared::Yes => realm.intrinsics().shared_array_buffer_prototype(vm),
    }
}

impl ArrayBuffer {
    fn new_with_buffer(vm: &Vm, buffer: OwnedBackingStore, is_shared: Shared, prototype: Gc<Object>) -> Self {
        Self {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            data_block: GcRefCell::new(DataBlock::new(buffer, is_shared)),
            max_byte_length: Cell::new(None),
            detach_key: Cell::new(Value::UNDEFINED),
            cached_views: GcRefCell::new(Vec::new()),
        }
    }

    pub fn new(vm: &Vm, is_shared: Shared, prototype: Gc<Object>) -> Self {
        Self {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            data_block: GcRefCell::new(DataBlock::empty(is_shared)),
            max_byte_length: Cell::new(None),
            detach_key: Cell::new(Value::UNDEFINED),
            cached_views: GcRefCell::new(Vec::new()),
        }
    }

    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        byte_length: usize,
        is_shared: Shared,
    ) -> ThrowCompletionOr<Gc<ArrayBuffer>> {
        let Ok(buffer) = allocate_or_retry_after_gc(vm.heap(), || OwnedBackingStore::create_zeroed(byte_length)) else {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::NotEnoughMemoryToAllocate,
                &[&byte_length],
            );
        };

        let prototype = prototype_for_shared_state(vm, realm, is_shared);
        Ok(realm.create_object(vm, ArrayBuffer::new_with_buffer(vm, buffer, is_shared, prototype)))
    }

    /// ArrayBuffer::create(Realm&, ByteBuffer, DataBlock::Shared)
    pub fn create_from_bytes(vm: &Vm, realm: Gc<Realm>, bytes: &[u8], is_shared: Shared) -> Gc<ArrayBuffer> {
        let owned_buffer =
            OwnedBackingStore::create_uninitialized(bytes.len()).expect("allocating the data block succeeds");
        let prototype = prototype_for_shared_state(vm, realm, is_shared);
        let array_buffer =
            realm.create_object(vm, ArrayBuffer::new_with_buffer(vm, owned_buffer, is_shared, prototype));
        array_buffer.overwrite(0, bytes);
        vm.heap()
            .did_allocate_external_memory(array_buffer.external_memory_size());
        array_buffer
    }

    /// ArrayBuffer::create(Realm&, DataBlock)
    pub fn create_from_data_block(vm: &Vm, realm: Gc<Realm>, block: DataBlock) -> Gc<ArrayBuffer> {
        let is_shared = block.is_shared;
        let prototype = prototype_for_shared_state(vm, realm, is_shared);
        let array_buffer = realm.create_object(vm, ArrayBuffer::new(vm, is_shared, prototype));
        array_buffer.set_data_block(vm, block);
        array_buffer
    }

    /// ArrayBuffer::create(Realm&, Core::AnonymousBuffer, u64 shared_object_id): a fixed-length SharedArrayBuffer over
    /// `size` bytes of a shared memory object that another agent made, as structured deserialization creates it.
    pub fn create_from_shared_memory(
        vm: &Vm,
        realm: Gc<Realm>,
        shared_memory: BorrowedSharedMemory<'_>,
        size: usize,
        object_id: u64,
    ) -> Result<Gc<ArrayBuffer>, OutOfMemory> {
        let store = SharedBackingStore::adopt(shared_memory, size, object_id)?;
        Ok(Self::create_from_data_block(vm, realm, DataBlock::shared_memory(store)))
    }

    pub fn byte_length(&self) -> usize {
        self.data_block.borrow().size()
    }

    /// The storage of the data block, for code that has to look into its kind, like StructuredSerialize, which shares
    /// it with other blocks.
    pub fn with_data_block<R>(&self, callback: impl FnOnce(&DataBlock) -> R) -> R {
        callback(&self.data_block.borrow())
    }

    pub fn copy_to(&self, offset: usize, destination: &mut [u8]) {
        self.data_block.borrow().copy_to(offset, destination);
    }

    pub fn copy_to_byte_buffer(&self, offset: usize, count: usize) -> Vec<u8> {
        self.data_block.borrow().copy_to_byte_buffer(offset, count)
    }

    pub fn copy_all_to_byte_buffer(&self) -> Vec<u8> {
        let data_block = self.data_block.borrow();
        data_block.copy_to_byte_buffer(0, data_block.size())
    }

    pub fn copy_data_to(
        &self,
        destination: &ArrayBuffer,
        source_offset: usize,
        destination_offset: usize,
        count: usize,
    ) {
        let source_block = self.data_block.borrow();
        let destination_block = destination.data_block.borrow();
        source_block.copy_to_block(&destination_block, source_offset, destination_offset, count);
    }

    pub fn copy_data_to_block(
        &self,
        destination: &DataBlock,
        source_offset: usize,
        destination_offset: usize,
        count: usize,
    ) {
        self.data_block
            .borrow()
            .copy_to_block(destination, source_offset, destination_offset, count);
    }

    pub fn overwrite(&self, offset: usize, source: &[u8]) {
        self.data_block.borrow().overwrite(offset, source);
    }

    pub fn move_data(&self, destination_offset: usize, source_offset: usize, count: usize) {
        self.data_block
            .borrow()
            .move_data(destination_offset, source_offset, count);
    }

    pub fn is_external(&self) -> bool {
        self.data_block.borrow().is_external()
    }

    pub fn is_caged(&self) -> bool {
        self.data_block.borrow().is_caged()
    }

    pub fn shares_storage_with(&self, other: &ArrayBuffer) -> bool {
        self.data_block.borrow().shares_storage_with(&other.data_block.borrow())
    }

    pub fn data_offset(&self) -> usize {
        self.data_block.borrow().offset()
    }

    // Detaches this ArrayBuffer and returns its underlying DataBlock for use in a TransferArrayBuffer-like operation.
    // If detach fails, the underlying storage is left untouched.
    pub fn detach_and_take_data_block(&self, vm: &Vm) -> ThrowCompletionOr<DataBlock> {
        assert!(!self.is_shared_array_buffer());

        if !same_value(self.detach_key(), Value::UNDEFINED) {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::DetachKeyMismatch,
                &[&Value::UNDEFINED, &self.detach_key()],
            );
        }

        let old_external_memory_size = self.external_memory_size();
        let is_shared = self.data_block.borrow().is_shared;
        let block = self.data_block.replace(DataBlock::empty(is_shared));
        self.invalidate_cached_typed_array_view_offsets();
        self.account_external_memory_change(vm, old_external_memory_size, 0);
        Ok(block)
    }

    // [[ArrayBufferMaxByteLength]]
    pub fn max_byte_length(&self) -> usize {
        self.max_byte_length
            .get()
            .expect("the ArrayBuffer has an [[ArrayBufferMaxByteLength]]")
    }

    /// Makes the buffer resizable, or growable if it is shared. Only views of fixed-length buffers cache the offset of
    /// the data, so the views that did stop doing so.
    pub fn set_max_byte_length(&self, max_byte_length: usize) {
        self.max_byte_length.set(Some(max_byte_length));
        self.invalidate_cached_typed_array_view_offsets();
    }

    fn account_external_memory_change(
        &self,
        vm: &Vm,
        old_external_memory_size: usize,
        new_external_memory_size: usize,
    ) {
        if new_external_memory_size > old_external_memory_size {
            vm.heap()
                .did_allocate_external_memory(new_external_memory_size - old_external_memory_size);
            return;
        }

        vm.heap()
            .did_free_external_memory(old_external_memory_size - new_external_memory_size);
    }

    // Used by allocate_array_buffer() to attach the data block after construction
    pub fn set_data_block(&self, vm: &Vm, block: DataBlock) {
        let old_external_memory_size = self.external_memory_size();
        let old_data_offset = self.data_offset();
        drop(self.data_block.replace(block));
        if self.data_offset() != old_data_offset {
            self.invalidate_cached_typed_array_view_offsets();
        }
        self.account_external_memory_change(vm, old_external_memory_size, self.external_memory_size());
    }

    pub fn did_change_data_block_capacity(&self, vm: &Vm, old_external_memory_size: usize) {
        self.account_external_memory_change(vm, old_external_memory_size, self.external_memory_size());
    }

    pub fn try_resize(
        &self,
        vm: &Vm,
        new_size: usize,
        zero_fill_new_bytes: ZeroFillNewBytes,
    ) -> Result<(), OutOfMemory> {
        // NB: A block over the storage of another buffer is not resized itself. The runtime resizes the other buffer,
        //     whose new size such a block has as long as it has no fixed byte length.
        let owner_of_aliased_storage = match &self.data_block.borrow().byte_buffer {
            DataBlockStorage::External(storage) => storage.array_buffer_owning_the_storage(),
            _ => None,
        };
        if let Some(owner) = owner_of_aliased_storage {
            return owner.try_resize(vm, new_size, zero_fill_new_bytes);
        }

        let old_external_memory_size = self.external_memory_size();
        let old_data_offset = self.data_offset();
        self.data_block.borrow_mut().try_resize(new_size, zero_fill_new_bytes)?;
        if self.data_offset() != old_data_offset {
            self.invalidate_cached_typed_array_view_offsets();
        }
        self.did_change_data_block_capacity(vm, old_external_memory_size);
        Ok(())
    }

    pub fn try_ensure_capacity(&self, vm: &Vm, new_capacity: usize) -> Result<(), OutOfMemory> {
        let old_external_memory_size = self.external_memory_size();
        let old_data_offset = self.data_offset();
        self.data_block.borrow_mut().try_ensure_capacity(new_capacity)?;
        if self.data_offset() != old_data_offset {
            self.invalidate_cached_typed_array_view_offsets();
        }
        self.did_change_data_block_capacity(vm, old_external_memory_size);
        Ok(())
    }

    pub fn detach_key(&self) -> Value {
        self.detach_key.get()
    }

    pub fn set_detach_key(&self, detach_key: Value) {
        self.detach_key.set(detach_key);
    }

    pub fn detach_buffer(&self, vm: &Vm) {
        let old_external_memory_size = self.external_memory_size();
        self.invalidate_cached_typed_array_view_offsets();
        let is_shared = self.data_block.borrow().is_shared;
        drop(self.data_block.replace(DataBlock::empty(is_shared)));
        self.account_external_memory_change(vm, old_external_memory_size, 0);
    }

    // 25.1.3.4 IsDetachedBuffer ( arrayBuffer ), https://tc39.es/ecma262/#sec-isdetachedbuffer
    pub fn is_detached(&self) -> bool {
        // 1. If arrayBuffer.[[ArrayBufferData]] is null, return true.
        // 2. Return false.
        matches!(self.data_block.borrow().byte_buffer, DataBlockStorage::Empty)
    }

    // 25.1.3.9 IsFixedLengthArrayBuffer ( arrayBuffer ), https://tc39.es/ecma262/#sec-isfixedlengtharraybuffer
    pub fn is_fixed_length(&self) -> bool {
        // 1. If arrayBuffer has an [[ArrayBufferMaxByteLength]] internal slot, return false.
        // 2. Return true.
        self.max_byte_length.get().is_none()
    }

    pub fn register_cached_typed_array_view(&self, vm: &Vm, view: Gc<TypedArrayBase>) {
        let view = GcWeak::new(vm.heap(), view);
        let mut cached_views = self.cached_views.borrow_mut();
        // NB: Views that died stay in the list until it is full, which keeps registering amortized constant time.
        if cached_views.len() == cached_views.capacity() {
            cached_views.retain(|view| view.get().is_some());
        }
        cached_views.push(view);
    }

    fn invalidate_cached_typed_array_view_offsets(&self) {
        let cached_views = self.cached_views.replace(Vec::new());
        for cached_view in &cached_views {
            if let Some(view) = cached_view.get()
                && core::ptr::eq(view.viewed_array_buffer().as_ptr(), self)
            {
                view.invalidate_cached_data_offset();
            }
        }
    }

    pub fn can_cache_typed_array_view_data_offset(&self) -> bool {
        let data_block = self.data_block.borrow();
        // NB: The views of a fixed-length Shared Data Block backed by shared memory stay on the atomic slow path.
        //     Unless the VM was asked for shared memory, the runtime owns such a block instead, and its views take the
        //     same path.
        let stands_in_for_shared_memory =
            matches!(data_block.byte_buffer, DataBlockStorage::Owned(_)) && data_block.is_shared == Shared::Yes;
        !matches!(data_block.byte_buffer, DataBlockStorage::Empty)
            && self.is_fixed_length()
            && data_block.is_caged()
            && !data_block.is_cross_process_shared()
            && !stands_in_for_shared_memory
    }

    // 25.2.2.2 IsSharedArrayBuffer ( obj ), https://tc39.es/ecma262/#sec-issharedarraybuffer
    pub fn is_shared_array_buffer(&self) -> bool {
        let data_block = self.data_block.borrow();
        // 1. Let bufferData be obj.[[ArrayBufferData]].
        // 2. If bufferData is null, return false.
        if matches!(data_block.byte_buffer, DataBlockStorage::Empty) {
            return false;
        }
        // 3. If bufferData is a Data Block, return false.
        // 4. Assert: bufferData is a Shared Data Block.
        // 5. Return true.
        data_block.is_shared == Shared::Yes
    }

    // 25.1.3.16 GetValueFromBuffer ( arrayBuffer, byteIndex, type, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-getvaluefrombuffer
    pub fn get_value(
        &self,
        vm: &Vm,
        byte_index: usize,
        element_type: ElementType,
        is_typed_array: bool,
        order: Order,
        is_little_endian: bool,
    ) -> Value {
        let mut raw_value = [0u8; 8];
        {
            let data_block = self.data_block.borrow();

            // 1. Assert: IsDetachedBuffer(arrayBuffer) is false.
            assert!(!matches!(data_block.byte_buffer, DataBlockStorage::Empty));

            // 2. Assert: There are sufficient bytes in arrayBuffer starting at byteIndex to represent a value of type.
            assert!(byte_index <= data_block.size());
            assert!(element_type.size() <= data_block.size() - byte_index);

            // 3. Let block be arrayBuffer.[[ArrayBufferData]].
            // 4. Let elementSize be the Element Size value specified in Table 70 for Element Type type.
            let element_size = element_type.size();

            // 5. If IsSharedArrayBuffer(arrayBuffer) is true, then
            if data_block.is_shared == Shared::Yes {
                // a. Let execution be the [[CandidateExecution]] field of the surrounding agent's Agent Record.
                // b. Let eventsRecord be the Agent Events Record of execution.[[EventsRecords]] whose [[AgentSignifier]] is AgentSignifier().
                // c. If isTypedArray is true and IsNoTearConfiguration(type, order) is true, let noTear be true; otherwise let noTear be false.
                // d. Let rawValue be a List of length elementSize whose elements are nondeterministically chosen byte values.
                // e. NOTE: In implementations, rawValue is the result of a non-atomic or atomic read instruction on the underlying hardware. The nondeterminism is a semantic prescription of the memory model to describe observable behaviour of hardware with weak consistency.
                // f. Let readEvent be ReadSharedMemory { [[Order]]: order, [[NoTear]]: noTear, [[Block]]: block, [[ByteIndex]]: byteIndex, [[ElementSize]]: elementSize }.
                // g. Append readEvent to eventsRecord.[[EventList]].
                // h. Append Chosen Value Record { [[Event]]: readEvent, [[ChosenValue]]: rawValue } to execution.[[ChosenValues]].
                // AD-HOC: We don't model the abstract memory model (candidate execution, event list, chosen values). Instead,
                //         we realize the read directly on the shared block with an atomic load: sequentially consistent for
                //         SeqCst order (Atomics.load), and relaxed for a typed-array element read — which the memory model
                //         requires to be tear-free (NoTear) for integer element types, and which a relaxed atomic provides
                //         without the UB of a non-atomic read racing another agent's write. A DataView read may be unaligned,
                //         and the model permits it to tear — so it takes the plain read of the non-shared case.
                if order == Order::SeqCst || is_typed_array {
                    let ordering = if order == Order::SeqCst {
                        Ordering::SeqCst
                    } else {
                        Ordering::Relaxed
                    };
                    let atomic_value = data_block.atomic_load(byte_index, element_size, ordering);
                    raw_value = atomic_value.to_le_bytes();
                } else {
                    data_block.copy_to(byte_index, &mut raw_value[..element_size]);
                }
            }
            // 6. Else,
            else {
                // a. Let rawValue be a List whose elements are bytes from block at indices in the interval from byteIndex (inclusive) to byteIndex + elementSize (exclusive).
                data_block.copy_to(byte_index, &mut raw_value[..element_size]);
            }
        }

        // 7. Assert: The number of elements in rawValue is elementSize.

        // 8. If isLittleEndian is not present, set isLittleEndian to the value of the [[LittleEndian]] field of the surrounding agent's Agent Record.
        //    NOTE: Done by default parameter at declaration of this function.

        // 9. Return RawBytesToNumeric(type, rawValue, isLittleEndian).
        raw_bytes_to_numeric(
            vm,
            element_type,
            &mut raw_value[..element_type.size()],
            is_little_endian,
        )
    }

    // 25.1.3.18 SetValueInBuffer ( arrayBuffer, byteIndex, type, value, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-setvalueinbuffer
    #[allow(clippy::too_many_arguments, reason = "the operation takes the spec's arguments")]
    pub fn set_value(
        &self,
        vm: &Vm,
        byte_index: usize,
        element_type: ElementType,
        value: Value,
        is_typed_array: bool,
        order: Order,
        is_little_endian: bool,
    ) {
        // 1. Assert: IsDetachedBuffer(arrayBuffer) is false.
        assert!(!self.is_detached());

        // 2. Assert: There are sufficient bytes in arrayBuffer starting at byteIndex to represent a value of type.
        assert!(byte_index <= self.byte_length());
        assert!(element_type.size() <= self.byte_length() - byte_index);

        // 3. Assert: value is a BigInt if IsBigIntElementType(type) is true; otherwise, value is a Number.
        if element_type.is_bigint() {
            assert!(value.is_bigint());
        } else {
            assert!(value.is_number());
        }

        // FIXME: 5. Let elementSize be the Element Size value specified in Table 70 for Element Type type.

        // 6. If isLittleEndian is not present, set isLittleEndian to the value of the [[LittleEndian]] field of the surrounding agent's Agent Record.
        //    NOTE: Done by default parameter at declaration of this function.

        // 7. Let rawBytes be NumericToRawBytes(type, value, isLittleEndian).
        let raw_bytes = numeric_to_raw_bytes(vm, element_type, value, is_little_endian);
        let raw_bytes = &raw_bytes[..element_type.size()];

        let data_block = self.data_block.borrow();

        // 8. If IsSharedArrayBuffer(arrayBuffer) is true, then
        if data_block.is_shared == Shared::Yes {
            // a. Let execution be the [[CandidateExecution]] field of the surrounding agent's Agent Record.
            // b. Let eventsRecord be the Agent Events Record of execution.[[EventsRecords]] whose [[AgentSignifier]] is AgentSignifier().
            // c. If isTypedArray is true and IsNoTearConfiguration(type, order) is true, let noTear be true; otherwise let noTear be false.
            // d. Append WriteSharedMemory { [[Order]]: order, [[NoTear]]: noTear, [[Block]]: block, [[ByteIndex]]: byteIndex, [[ElementSize]]: elementSize, [[Payload]]: rawBytes } to eventsRecord.[[EventList]].
            // AD-HOC: We don't model the abstract memory model (candidate execution, event list). Instead, we realize the
            //         write directly on the shared block with an atomic store: sequentially consistent for SeqCst order
            //         (Atomics.store), and relaxed for a typed-array element write — which the memory model requires to be
            //         tear-free (NoTear) for integer element types, and which a relaxed atomic provides without the UB of a
            //         non-atomic write racing another agent. A DataView write may be unaligned, and the model permits it to
            //         tear — so it takes the plain write of the non-shared case.
            if order == Order::SeqCst || is_typed_array {
                let ordering = if order == Order::SeqCst {
                    Ordering::SeqCst
                } else {
                    Ordering::Relaxed
                };
                let mut atomic_value = [0u8; 8];
                atomic_value[..raw_bytes.len()].copy_from_slice(raw_bytes);
                data_block.atomic_store(byte_index, raw_bytes.len(), u64::from_le_bytes(atomic_value), ordering);
            } else {
                data_block.overwrite(byte_index, raw_bytes);
            }
        }
        // 9. Else,
        else {
            // a. Store the individual bytes of rawBytes into block, starting at block[byteIndex].
            data_block.overwrite(byte_index, raw_bytes);
        }

        // 10. Return unused.
    }

    // 25.1.3.19 GetModifySetValueInBuffer ( arrayBuffer, byteIndex, type, value, op [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-getmodifysetvalueinbuffer
    pub fn get_modify_set_value(
        &self,
        vm: &Vm,
        byte_index: usize,
        element_type: ElementType,
        value: Value,
        operation: ReadWriteModifyOperation,
        is_little_endian: bool,
    ) -> Value {
        let raw_bytes = numeric_to_raw_bytes(vm, element_type, value, is_little_endian);

        // The operation performs the read-modify-write atomically on the live buffer bytes (which may be shared cross-agent
        // memory), and returns the bytes that were read.
        let raw_bytes_read = self.data_block.borrow().atomic_read_modify_write(
            byte_index,
            element_type.size(),
            operation,
            u64::from_le_bytes(raw_bytes),
        );

        let mut raw_bytes_read = raw_bytes_read.to_le_bytes();
        raw_bytes_to_numeric(
            vm,
            element_type,
            &mut raw_bytes_read[..element_type.size()],
            is_little_endian,
        )
    }

    /// A sequentially consistent compare-exchange on the live block, which returns the bytes it read: the
    /// AtomicCompareExchangeInSharedBlock of Atomics.compareExchange, and its plain read-compare-store for a block
    /// that is not shared.
    pub fn atomic_compare_exchange(
        &self,
        byte_index: usize,
        element_size: usize,
        expected_bytes: [u8; 8],
        replacement_bytes: [u8; 8],
    ) -> [u8; 8] {
        self.data_block
            .borrow()
            .atomic_compare_exchange(
                byte_index,
                element_size,
                u64::from_le_bytes(expected_bytes),
                u64::from_le_bytes(replacement_bytes),
            )
            .to_le_bytes()
    }

    /// Core::atomic_wait() on the word at `byte_index`.
    pub fn atomic_wait(
        &self,
        byte_index: usize,
        expected: u64,
        size: usize,
        timeout: Option<Duration>,
    ) -> AtomicWaitResult {
        let address = self.data_block.borrow().data_at(byte_index);
        // NB: The data block stays in place while the agent waits, since the agent is the only one that could change it.
        futex::atomic_wait(address, expected, size, timeout)
    }

    /// Core::atomic_notify() on the word at `byte_index`.
    pub fn atomic_notify(&self, byte_index: usize, size: usize, max_count: usize) -> usize {
        let address = self.data_block.borrow().data_at(byte_index);
        futex::atomic_notify(address, size, max_count)
    }
}

// 6.2.9.1 CreateByteDataBlock ( size ), https://tc39.es/ecma262/#sec-createbytedatablock
pub fn create_byte_data_block(vm: &Vm, size: usize, capacity: Option<usize>) -> ThrowCompletionOr<DataBlock> {
    // 1. If size > 2^53 - 1, throw a RangeError exception.
    if size as f64 > MAX_ARRAY_LIKE_INDEX {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"array buffer"]);
    }
    if let Some(capacity) = capacity
        && capacity as f64 > MAX_ARRAY_LIKE_INDEX
    {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"array buffer"]);
    }

    // 2. Let db be a new Data Block value consisting of size bytes. If it is impossible to create such a Data Block, throw a RangeError exception.
    // 3. Set all of the bytes of db to 0.
    let data_block = allocate_or_retry_after_gc(vm.heap(), || match capacity {
        Some(capacity) => OwnedBackingStore::create_zeroed_with_capacity(size, capacity),
        None => OwnedBackingStore::create_zeroed(size),
    });
    let Ok(data_block) = data_block else {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::NotEnoughMemoryToAllocate,
            &[&capacity.unwrap_or(size)],
        );
    };

    // 4. Return db.
    Ok(DataBlock::new(data_block, Shared::No))
}

// FIXME: The returned DataBlock is not shared in the sense that the standard specifies it.
// 6.2.9.2 CreateSharedByteDataBlock ( size ), https://tc39.es/ecma262/#sec-createsharedbytedatablock
fn create_shared_byte_data_block(vm: &Vm, size: usize, capacity: Option<usize>) -> ThrowCompletionOr<DataBlock> {
    let capacity = capacity.unwrap_or(size);

    // AD-HOC: A fixed-length shared Data Block is backed by cross-process shared memory (Core::AnonymousBuffer) — so
    //         that the SharedArrayBuffer is genuinely shared across agents in different processes, rather than copied.
    //         Fresh anonymous shared memory is zero-filled by the OS. A growable shared Data Block (capacity > size)
    //         uses process-local storage, so a growable SharedArrayBuffer crosses a process boundary as a copy.
    // NB: Only a VM whose embedder has agents in other processes asks for shared memory. Otherwise, the fixed-length
    //     block is owned by the runtime, like a growable one.
    if capacity == size && size > 0 {
        // AD-HOC: Cap the shared allocation — so one SharedArrayBuffer can't reserve an absurd amount of address space
        //         in every agent that maps it. Chromium caps its shared-memory regions at INT_MAX; match that (the fd
        //         transport also carries the size as a u32).
        if size > i32::MAX as usize {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::InvalidLength,
                &[&"shared array buffer"],
            );
        }

        let block = allocate_or_retry_after_gc(vm.heap(), || {
            if vm.options().shared_memory_shared_array_buffers {
                // Mint the id that will name this object for as long as it exists — in this agent and every agent it
                // reaches.
                return Ok(DataBlock::shared_memory(SharedBackingStore::create(size)?));
            }
            Ok(DataBlock::new(OwnedBackingStore::create_zeroed(size)?, Shared::Yes))
        });
        let Ok(block) = block else {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::NotEnoughMemoryToAllocate, &[&size]);
        };
        return Ok(block);
    }

    // 1. Let db be a new Shared Data Block value consisting of size bytes. If it is impossible to create such a Shared Data Block, throw a RangeError exception.
    let data_block = allocate_or_retry_after_gc(vm.heap(), || {
        OwnedBackingStore::create_zeroed_with_capacity(size, capacity)
    });
    let Ok(data_block) = data_block else {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::NotEnoughMemoryToAllocate,
            &[&capacity],
        );
    };

    // 2. Let execution be the [[CandidateExecution]] field of the surrounding agent's Agent Record.
    // 3. Let eventsRecord be the Agent Events Record of execution.[[EventsRecords]] whose [[AgentSignifier]] is AgentSignifier().
    // 4. Let zero be « 0 ».
    // 5. For each index i of db, do
    // a. Append WriteSharedMemory { [[Order]]: init, [[NoTear]]: true, [[Block]]: db, [[ByteIndex]]: i, [[ElementSize]]: 1, [[Payload]]: zero } to eventsRecord.[[EventList]].
    // 6. Return db.
    Ok(DataBlock::new(data_block, Shared::Yes))
}

// 6.2.9.3 CopyDataBlockBytes ( toBlock, toIndex, fromBlock, fromIndex, count ), https://tc39.es/ecma262/#sec-copydatablockbytes

// 25.1.3.1 AllocateArrayBuffer ( constructor, byteLength [ , maxByteLength ] ), https://tc39.es/ecma262/#sec-allocatearraybuffer
pub fn allocate_array_buffer(
    vm: &Vm,
    constructor: Gc<FunctionObject>,
    byte_length: usize,
    max_byte_length: Option<usize>,
) -> ThrowCompletionOr<Gc<ArrayBuffer>> {
    let realm = vm.current_realm().expect("AllocateArrayBuffer runs in a realm");

    // 1. Let slots be « [[ArrayBufferData]], [[ArrayBufferByteLength]], [[ArrayBufferDetachKey]] ».

    // 2. If maxByteLength is present and maxByteLength is not empty, let allocatingResizableBuffer be true; otherwise let allocatingResizableBuffer be false.

    // 3. If allocatingResizableBuffer is true, then
    if let Some(max_byte_length) = max_byte_length {
        // a. If byteLength > maxByteLength, throw a RangeError exception.
        if byte_length > max_byte_length {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::ByteLengthExceedsMaxByteLength,
                &[&byte_length, &max_byte_length],
            );
        }

        // b. Append [[ArrayBufferMaxByteLength]] to slots.
    }

    // 4. Let obj be ? OrdinaryCreateFromConstructor(constructor, "%ArrayBuffer.prototype%", slots).
    let object = ordinary_create_from_constructor_of(
        vm,
        realm,
        constructor,
        Intrinsics::array_buffer_prototype,
        |prototype| ArrayBuffer::new(vm, Shared::No, prototype),
    )?;

    // 5. Let block be ? CreateByteDataBlock(byteLength).
    let block = create_byte_data_block(vm, byte_length, max_byte_length)?;

    // 6. Set obj.[[ArrayBufferData]] to block.
    object.set_data_block(vm, block);

    // 7. Set obj.[[ArrayBufferByteLength]] to byteLength.

    // 8. If allocatingResizableBuffer is true, then
    if let Some(max_byte_length) = max_byte_length {
        // a. If it is not possible to create a Data Block block consisting of maxByteLength bytes, throw a RangeError exception.
        // b. NOTE: Resizable ArrayBuffers are designed to be implementable with in-place growth. Implementations may throw if, for example, virtual memory cannot be reserved up front.

        // c. Set obj.[[ArrayBufferMaxByteLength]] to maxByteLength.
        object.set_max_byte_length(max_byte_length);
    }

    // 9. Return obj.
    Ok(object)
}

// 25.1.3.3 ArrayBufferCopyAndDetach ( arrayBuffer, newLength, preserveResizability ), https://tc39.es/ecma262/#sec-arraybuffercopyanddetach
pub fn array_buffer_copy_and_detach(
    vm: &Vm,
    array_buffer: Gc<ArrayBuffer>,
    new_length: Value,
    preserve_resizability: PreserveResizability,
) -> ThrowCompletionOr<Gc<ArrayBuffer>> {
    let realm = vm.current_realm().expect("ArrayBufferCopyAndDetach runs in a realm");

    // 1. Perform ? RequireInternalSlot(arrayBuffer, [[ArrayBufferData]]).

    // 2. If IsSharedArrayBuffer(arrayBuffer) is true, throw a TypeError exception.
    if array_buffer.is_shared_array_buffer() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
    }

    // 3. If newLength is undefined, then
    //     a. Let newByteLength be arrayBuffer.[[ArrayBufferByteLength]].
    // 4. Else,
    //     a. Let newByteLength be ? ToIndex(newLength).
    let new_byte_length = if new_length.is_undefined() {
        array_buffer.byte_length()
    } else {
        new_length.to_index(vm)? as usize
    };

    // 5. If IsDetachedBuffer(arrayBuffer) is true, throw a TypeError exception.
    if array_buffer.is_detached() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::DetachedArrayBuffer, &[]);
    }

    // 6. If preserveResizability is PRESERVE-RESIZABILITY and IsFixedLengthArrayBuffer(arrayBuffer) is false, then
    let new_max_byte_length =
        if preserve_resizability == PreserveResizability::PreserveResizability && !array_buffer.is_fixed_length() {
            // a. Let newMaxByteLength be arrayBuffer.[[ArrayBufferMaxByteLength]].
            Some(array_buffer.max_byte_length())
        }
        // 7. Else,
        else {
            // a. Let newMaxByteLength be EMPTY.
            None
        };

    // 8. If arrayBuffer.[[ArrayBufferDetachKey]] is not undefined, throw a TypeError exception.
    if !array_buffer.detach_key().is_undefined() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::DetachKeyMismatch,
            &[&array_buffer.detach_key(), &Value::UNDEFINED],
        );
    }

    // 9. Let newBuffer be ? AllocateArrayBuffer(%ArrayBuffer%, newByteLength, newMaxByteLength).
    let new_buffer = allocate_array_buffer(
        vm,
        realm.intrinsics().array_buffer_constructor(vm),
        new_byte_length,
        new_max_byte_length,
    )?;

    // 10. Let copyLength be min(newByteLength, arrayBuffer.[[ArrayBufferByteLength]]).
    let copy_length = new_byte_length.min(array_buffer.byte_length());

    // 11. Let fromBlock be arrayBuffer.[[ArrayBufferData]].
    // 12. Let toBlock be newBuffer.[[ArrayBufferData]].
    // 13. Perform CopyDataBlockBytes(toBlock, 0, fromBlock, 0, copyLength).
    // 14. NOTE: Neither creation of the new Data Block nor copying from the old Data Block are observable. Implementations may implement this method as a zero-copy move or a realloc.
    array_buffer.copy_data_to(&new_buffer, 0, 0, copy_length);

    // 15. Perform ! DetachArrayBuffer(arrayBuffer).
    detach_array_buffer(vm, array_buffer, None).must();

    // 16. Return newBuffer.
    Ok(new_buffer)
}

// 25.1.3.5 DetachArrayBuffer ( arrayBuffer [ , key ] ), https://tc39.es/ecma262/#sec-detacharraybuffer
pub fn detach_array_buffer(vm: &Vm, array_buffer: Gc<ArrayBuffer>, key: Option<Value>) -> ThrowCompletionOr<()> {
    // 1. Assert: IsSharedArrayBuffer(arrayBuffer) is false.
    assert!(!array_buffer.is_shared_array_buffer());

    // 2. If key is not present, set key to undefined.
    let key = key.unwrap_or(Value::UNDEFINED);

    // 3. If SameValue(arrayBuffer.[[ArrayBufferDetachKey]], key) is false, throw a TypeError exception.
    if !same_value(array_buffer.detach_key(), key) {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::DetachKeyMismatch,
            &[&key, &array_buffer.detach_key()],
        );
    }

    // 4. Set arrayBuffer.[[ArrayBufferData]] to null.
    // 5. Set arrayBuffer.[[ArrayBufferByteLength]] to 0.
    array_buffer.detach_buffer(vm);

    // 6. Return unused.
    Ok(())
}

// 25.1.3.6 CloneArrayBuffer ( srcBuffer, srcByteOffset, srcLength, cloneConstructor ), https://tc39.es/ecma262/#sec-clonearraybuffer
pub fn clone_array_buffer(
    vm: &Vm,
    source_buffer: Gc<ArrayBuffer>,
    source_byte_offset: usize,
    source_length: usize,
) -> ThrowCompletionOr<Gc<ArrayBuffer>> {
    let realm = vm.current_realm().expect("CloneArrayBuffer runs in a realm");

    // 1. Assert: IsDetachedBuffer(srcBuffer) is false.
    assert!(!source_buffer.is_detached());

    // 2. Let targetBuffer be ? AllocateArrayBuffer(%ArrayBuffer%, srcLength).
    let target_buffer =
        allocate_array_buffer(vm, realm.intrinsics().array_buffer_constructor(vm), source_length, None)?;

    // 3. Let srcBlock be srcBuffer.[[ArrayBufferData]].
    // 4. Let targetBlock be targetBuffer.[[ArrayBufferData]].
    // 5. Perform CopyDataBlockBytes(targetBlock, 0, srcBlock, srcByteOffset, srcLength).
    source_buffer.copy_data_to(&target_buffer, source_byte_offset, 0, source_length);

    // 6. Return targetBuffer.
    Ok(target_buffer)
}

// 25.1.3.7 GetArrayBufferMaxByteLengthOption ( options ), https://tc39.es/ecma262/#sec-getarraybuffermaxbytelengthoption
pub fn get_array_buffer_max_byte_length_option(vm: &Vm, options: Value) -> ThrowCompletionOr<Option<usize>> {
    // 1. If options is not an Object, return empty.
    if !options.is_object() {
        return Ok(None);
    }

    // 2. Let maxByteLength be ? Get(options, "maxByteLength").
    let max_byte_length = options.as_object().get(vm, &vm.names.maxByteLength)?;

    // 3. If maxByteLength is undefined, return empty.
    if max_byte_length.is_undefined() {
        return Ok(None);
    }

    // 4. Return ? ToIndex(maxByteLength).
    Ok(Some(max_byte_length.to_index(vm)? as usize))
}

// 25.2.2.1 AllocateSharedArrayBuffer ( constructor, byteLength [ , maxByteLength ] ), https://tc39.es/ecma262/#sec-allocatesharedarraybuffer
pub fn allocate_shared_array_buffer(
    vm: &Vm,
    constructor: Gc<FunctionObject>,
    byte_length: usize,
    max_byte_length: Option<usize>,
) -> ThrowCompletionOr<Gc<ArrayBuffer>> {
    let realm = vm.current_realm().expect("AllocateSharedArrayBuffer runs in a realm");

    // 1. Let slots be « [[ArrayBufferData]] ».

    // 2. If maxByteLength is present and maxByteLength is not empty, let allocatingGrowableBuffer be true; otherwise let allocatingGrowableBuffer be false.

    // 3. If allocatingGrowableBuffer is true, then
    if let Some(max_byte_length) = max_byte_length {
        // a. If byteLength > maxByteLength, throw a RangeError exception.
        if byte_length > max_byte_length {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::ByteLengthExceedsMaxByteLength,
                &[&byte_length, &max_byte_length],
            );
        }

        // b. Append [[ArrayBufferByteLengthData]] and [[ArrayBufferMaxByteLength]] to slots.
    }

    // 4. Else,
    //        a. Append [[ArrayBufferByteLength]] to slots.

    // 5. Let obj be ? OrdinaryCreateFromConstructor(constructor, "%SharedArrayBuffer.prototype%", slots).
    let object = ordinary_create_from_constructor_of(
        vm,
        realm,
        constructor,
        Intrinsics::shared_array_buffer_prototype,
        |prototype| ArrayBuffer::new(vm, Shared::Yes, prototype),
    )?;

    // 6. If allocatingGrowableBuffer is true, let allocLength be maxByteLength; otherwise let allocLength be byteLength.
    let alloc_length = max_byte_length.unwrap_or(byte_length);

    // 7. Let block be ? CreateSharedByteDataBlock(allocLength).
    // AD-HOC: We track [[ArrayBufferByteLength(Data)]] via the length of the Data Block, so reserve allocLength
    //         up front and expose byteLength as the current length.
    let block = create_shared_byte_data_block(vm, byte_length, Some(alloc_length))?;

    // 8. Set obj.[[ArrayBufferData]] to block.
    object.set_data_block(vm, block);

    // 9. If allocatingGrowableBuffer is true, then
    if let Some(max_byte_length) = max_byte_length {
        // a. Assert: byteLength ≤ maxByteLength.
        assert!(byte_length <= max_byte_length);

        // FIXME: b. Let byteLengthBlock be ? CreateSharedByteDataBlock(8).
        // FIXME: c. Perform SetValueInBuffer(byteLengthBlock, 0, biguint64, ℤ(byteLength), true, seq-cst).
        // FIXME: d. Set obj.[[ArrayBufferByteLengthData]] to byteLengthBlock.

        // e. Set obj.[[ArrayBufferMaxByteLength]] to maxByteLength.
        object.set_max_byte_length(max_byte_length);
    }

    // 10. Else,
    //         a. Set obj.[[ArrayBufferByteLength]] to byteLength.

    // 11. Return obj.
    Ok(object)
}

// 25.1.3.2 ArrayBufferByteLength ( arrayBuffer, order ), https://tc39.es/ecma262/#sec-arraybufferbytelength
pub fn array_buffer_byte_length(array_buffer: &ArrayBuffer, _order: Order) -> usize {
    // FIXME: 1. If IsSharedArrayBuffer(arrayBuffer) is true and arrayBuffer has an [[ArrayBufferByteLengthData]] internal slot, then
    // FIXME:     a. Let bufferByteLengthBlock be arrayBuffer.[[ArrayBufferByteLengthData]].
    // FIXME:     b. Let rawLength be GetRawBytesFromSharedBlock(bufferByteLengthBlock, 0, biguint64, true, order).
    // FIXME:     c. Let isLittleEndian be the value of the [[LittleEndian]] field of the surrounding agent's Agent Record.
    // FIXME:     d. Return ℝ(RawBytesToNumeric(biguint64, rawLength, isLittleEndian)).

    // 2. Assert: IsDetachedBuffer(arrayBuffer) is false.
    assert!(!array_buffer.is_detached());

    // 3. Return arrayBuffer.[[ArrayBufferByteLength]].
    array_buffer.byte_length()
}

/// static_cast<f16>(double): the bits of the binary16 value nearest the double, ties to the even significand. A NaN
/// becomes the quiet NaN that keeps the sign and the top bits of the payload, as the hardware conversion does.
pub fn f64_to_binary16_bits(value: f64) -> u16 {
    let bits = value.to_bits();
    let sign = ((bits >> 48) & 0x8000) as u16;
    if value.is_nan() {
        return sign | 0x7e00 | ((bits >> 42) & 0x1ff) as u16;
    }
    if value == 0.0 {
        return sign;
    }
    if value.is_infinite() {
        return sign | 0x7c00;
    }
    let rounded = round_to_binary16(value).abs();
    if rounded.is_infinite() {
        return sign | 0x7c00;
    }
    // The rounded value is a binary16 value, so the conversions below are exact.
    if rounded < 2f64.powi(-14) {
        return sign | (rounded * 2f64.powi(24)) as u16;
    }
    let rounded_bits = rounded.to_bits();
    let exponent = ((rounded_bits >> 52) & 0x7ff) as i32 - 1023;
    let significand = ((rounded_bits >> 42) & 0x3ff) as u16;
    sign | (((exponent + 15) as u16) << 10) | significand
}

pub fn binary16_bits_to_f64(bits: u16) -> f64 {
    let sign = if bits & 0x8000 != 0 { -1.0 } else { 1.0 };
    let exponent = i32::from((bits >> 10) & 0x1f);
    let significand = f64::from(bits & 0x3ff);
    let magnitude = match exponent {
        0 => significand * 2f64.powi(-24),
        0x1f if significand == 0.0 => f64::INFINITY,
        0x1f => f64::NAN,
        _ => (1.0 + significand / 1024.0) * 2f64.powi(exponent - 15),
    };
    sign * magnitude
}

// 25.1.3.14 RawBytesToNumeric ( type, rawBytes, isLittleEndian ), https://tc39.es/ecma262/#sec-rawbytestonumeric
pub fn raw_bytes_to_numeric(vm: &Vm, element_type: ElementType, raw_value: &mut [u8], is_little_endian: bool) -> Value {
    // 1. Let elementSize be the Element Size value specified in Table 70 for Element Type type.
    //    NOTE: Used in step 7, but not needed with our implementation of that step.

    // 2. If isLittleEndian is false, reverse the order of the elements of rawBytes.
    if !is_little_endian {
        assert!(raw_value.len().is_multiple_of(2));
        raw_value.reverse();
    }

    let mut bytes = [0u8; 8];
    bytes[..raw_value.len()].copy_from_slice(raw_value);
    let little_endian_value = u64::from_le_bytes(bytes);

    match element_type {
        // 3. If type is Float16, then
        ElementType::Float16 => {
            // a. Let value be the byte elements of rawBytes concatenated and interpreted as a little-endian bit string encoding of an IEEE 754-2019 binary16 value.
            let value = binary16_bits_to_f64(little_endian_value as u16);

            // b. If value is an IEEE 754-2019 binary16 NaN value, return the NaN Number value.
            if value.is_nan() {
                return Value::from_f64(f64::NAN);
            }

            // c. Return the Number value that corresponds to value.
            Value::from_f64(value)
        }
        // 4. If type is Float32, then
        ElementType::Float32 => {
            // a. Let value be the byte elements of rawBytes concatenated and interpreted as a little-endian bit string encoding of an IEEE 754-2019 binary32 value.
            let value = f32::from_bits(little_endian_value as u32);

            // b. If value is an IEEE 754-2019 binary32 NaN value, return the NaN Number value.
            if value.is_nan() {
                return Value::from_f64(f64::NAN);
            }

            // c. Return the Number value that corresponds to value.
            Value::from_f64(f64::from(value))
        }
        // 5. If type is Float64, then
        ElementType::Float64 => {
            // a. Let value be the byte elements of rawBytes concatenated and interpreted as a little-endian bit string encoding of an IEEE 754-2019 binary64 value.
            let value = f64::from_bits(little_endian_value);

            // b. If value is an IEEE 754-2019 binary64 NaN value, return the NaN Number value.
            if value.is_nan() {
                return Value::from_f64(f64::NAN);
            }

            // c. Return the Number value that corresponds to value.
            Value::from_f64(value)
        }
        // 6. If IsUnsignedElementType(type) is true, then
        //     a. Let intValue be the byte elements of rawBytes concatenated and interpreted as a bit string encoding of an unsigned little-endian binary number.
        // 7. Else,
        //     a. Let intValue be the byte elements of rawBytes concatenated and interpreted as a bit string encoding of a binary little-endian two's complement number of bit length elementSize × 8.
        //
        // 8. If IsBigIntElementType(type) is true, return the BigInt value that corresponds to intValue.
        ElementType::BigInt64 => Value::from_bigint(BigInt::create(vm, NumBigInt::from(little_endian_value as i64))),
        ElementType::BigUint64 => Value::from_bigint(BigInt::create(vm, NumBigInt::from(little_endian_value))),
        // 9. Otherwise, return the Number value that corresponds to intValue.
        ElementType::Uint8 | ElementType::Uint8Clamped => Value::from_i32(i32::from(little_endian_value as u8)),
        ElementType::Uint16 => Value::from_i32(i32::from(little_endian_value as u16)),
        ElementType::Uint32 => Value::from_f64(f64::from(little_endian_value as u32)),
        ElementType::Int8 => Value::from_i32(i32::from(little_endian_value as u8 as i8)),
        ElementType::Int16 => Value::from_i32(i32::from(little_endian_value as u16 as i16)),
        ElementType::Int32 => Value::from_i32(little_endian_value as u32 as i32),
    }
}

// 25.1.3.17 NumericToRawBytes ( type, value, isLittleEndian ), https://tc39.es/ecma262/#sec-numerictorawbytes
/// The raw bytes in the first Element Size bytes of the result.
pub fn numeric_to_raw_bytes(vm: &Vm, element_type: ElementType, value: Value, is_little_endian: bool) -> [u8; 8] {
    assert!(value.is_number() || value.is_bigint());
    let little_endian_bytes: u64 = match element_type {
        ElementType::Float16 => u64::from(f64_to_binary16_bits(value.to_double(vm).must())),
        ElementType::Float32 => u64::from((value.to_double(vm).must() as f32).to_bits()),
        ElementType::Float64 => value.to_double(vm).must().to_bits(),
        ElementType::BigInt64 => value.to_bigint_int64(vm).must() as u64,
        ElementType::BigUint64 => value.to_bigint_uint64(vm).must(),
        ElementType::Int32 => u64::from(value.to_i32(vm).must() as u32),
        ElementType::Int16 => u64::from(value.to_i16(vm).must() as u16),
        ElementType::Int8 => u64::from(value.to_i8(vm).must() as u8),
        ElementType::Uint32 => u64::from(value.to_u32(vm).must()),
        ElementType::Uint16 => u64::from(value.to_u16(vm).must()),
        ElementType::Uint8 => u64::from(value.to_u8(vm).must()),
        ElementType::Uint8Clamped => u64::from(value.to_u8_clamp(vm).must()),
    };
    let mut raw_bytes = little_endian_bytes.to_le_bytes();
    let element_size = element_type.size();
    if !is_little_endian && element_size >= 2 {
        raw_bytes[..element_size].reverse();
    }
    raw_bytes
}
