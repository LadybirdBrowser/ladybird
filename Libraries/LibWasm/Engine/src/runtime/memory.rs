/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Linear memories. A memory reserves the address range its accesses can form up front and only commits what it has
//! grown to, so that its base never moves and accesses past its end fault into the guard region instead of needing a
//! bounds check. A store with a LibGC heap keeps its memories in LibGC's primitive storage, inside the cage that
//! array buffers live in, so that buffers can view them; other stores map their own.
//!
//! A shared memory belongs to the process. Each agent that has it has a memory of its own over the same reservation,
//! which is released with the last of them. The agents see one size, which any of them may grow.

use super::ranges::RangeSet;
use crate::abi::MemoryData;
use crate::types::{AddressType, MemoryType};
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, LazyLock, Mutex, MutexGuard, PoisonError, Weak};

pub const PAGE_SIZE: u64 = 64 * 1024;
const MEMORY32_MAX_PAGES: u64 = 1 << 16;

/// The most pages a memory64 reserves, and so can grow to: 16 GiB. Its accesses are bounds checked, so it needs no
/// guard region.
const MEMORY64_MAX_PAGES: u64 = 1 << 18;

/// The bytes a memory32 memory reserves: every address (u32) plus every offset (u32), plus one page for the width of
/// the access.
pub const GUARDED_RESERVATION_SIZE: u64 = 2 * (MEMORY32_MAX_PAGES * PAGE_SIZE) + PAGE_SIZE;

/// The address ranges all memories reserve, for the fault handler to recognize accesses that ran off a memory.
static RESERVATIONS: RangeSet = RangeSet::new();

/// Whether `address` is in the reservation of a memory. Async-signal-safe, for the fault handler.
pub fn is_in_any_reservation(address: usize) -> bool {
    RESERVATIONS.contains(address)
}

/// The address range of a memory, of which the pages that the memory has grown to are committed.
struct Reservation {
    base: *mut u8,
    size: usize,
    maximum_pages: u64,
    backing: Backing,
}

enum Backing {
    Mapping,
    PrimitiveStorage {
        handle: u64,
        resize: unsafe extern "C" fn(
            handle: u64,
            new_size: usize,
            zero_fill: bool,
            out_layout: *mut super::heap::PrimitiveStorageLayout,
        ) -> bool,
        free: unsafe extern "C" fn(handle: u64),
    },
}

impl Reservation {
    /// A reservation for a memory of the type, with nothing committed.
    fn new(ty: MemoryType, heap: &super::heap::Heap) -> Option<Self> {
        let (maximum_pages, capacity, guard_size) = match ty.limits.address_type {
            AddressType::I32 => {
                let maximum_pages = ty.limits.max.unwrap_or(MEMORY32_MAX_PAGES).min(MEMORY32_MAX_PAGES);
                let capacity = maximum_pages * PAGE_SIZE;
                (maximum_pages, capacity, GUARDED_RESERVATION_SIZE - capacity)
            }
            AddressType::I64 => {
                let maximum_pages = ty.limits.max.unwrap_or(MEMORY64_MAX_PAGES).min(MEMORY64_MAX_PAGES);
                // A memory that cannot grow still reserves a page, so that its base is a valid address.
                (maximum_pages, maximum_pages.max(1) * PAGE_SIZE, 0)
            }
        };
        if ty.limits.min > maximum_pages {
            return None;
        }
        let size = (capacity + guard_size) as usize;
        let (base, backing) = match heap.gc() {
            Some(gc) => {
                let interface = gc.interface;
                let mut handle = 0;
                let mut layout = super::heap::PrimitiveStorageLayout::default();
                let reserved = unsafe {
                    (interface.gc_primitive_storage_reserve)(
                        0,
                        capacity as usize,
                        true,
                        guard_size as usize,
                        &raw mut handle,
                        &raw mut layout,
                    )
                };
                if !reserved {
                    return None;
                }
                let data = unsafe { (interface.gc_primitive_storage_data)(handle) };
                (
                    data,
                    // NB: Primitive storage belongs to the process, not to the heap of the interface.
                    Backing::PrimitiveStorage {
                        handle,
                        resize: interface.gc_primitive_storage_resize,
                        free: interface.gc_primitive_storage_free,
                    },
                )
            }
            None => {
                let base = unsafe { platform::reserve(size) };
                if base.is_null() {
                    return None;
                }
                (base, Backing::Mapping)
            }
        };
        let reservation = Self {
            base,
            size,
            maximum_pages,
            backing,
        };
        // NB: Dropping the reservation releases it if this fails.
        RESERVATIONS.add(base as usize, size).then_some(reservation)
    }

    /// Commits the pages from `pages` up to `new_pages`, zero-filled. The pages below `pages` stay where they are, and
    /// as they are.
    fn commit(&self, pages: u64, new_pages: u64) -> bool {
        if new_pages > self.maximum_pages {
            return false;
        }
        if new_pages == pages {
            return true;
        }
        match &self.backing {
            Backing::Mapping => {
                let start = unsafe { self.base.add((pages * PAGE_SIZE) as usize) };
                unsafe { platform::commit(start, ((new_pages - pages) * PAGE_SIZE) as usize) }
            }
            // Within the capacity, the storage stays where it is.
            Backing::PrimitiveStorage { handle, resize, .. } => unsafe {
                resize(*handle, (new_pages * PAGE_SIZE) as usize, true, core::ptr::null_mut())
            },
        }
    }

    fn primitive_storage_handle(&self) -> u64 {
        match &self.backing {
            Backing::Mapping => 0,
            Backing::PrimitiveStorage { handle, .. } => *handle,
        }
    }
}

impl Drop for Reservation {
    fn drop(&mut self) {
        RESERVATIONS.remove(self.base as usize);
        match &self.backing {
            Backing::Mapping => unsafe { platform::release(self.base, self.size) },
            Backing::PrimitiveStorage { handle, free, .. } => unsafe { free(*handle) },
        }
    }
}

/// A shared memory of the process. Its data is its first field, and the memories of all agents that have it point
/// there, so the interpreter of each agent sees the size that the memory has now.
#[repr(C)]
pub struct SharedMemory {
    data: MemoryData,
    ty: MemoryType,
    reservation: Reservation,
    /// Held to grow the memory, so that the size never names pages that are not committed yet.
    grow_lock: Mutex<()>,
    id: u64,
}

// SAFETY: The reservation never moves, growing it takes the lock, and the size is an atomic. What agents do to the bytes
//         is up to their code, as the bytes of shared memory are.
unsafe impl Send for SharedMemory {}
// SAFETY: As above.
unsafe impl Sync for SharedMemory {}

/// A shared memory as the process knows it. The handle is beside the weak reference so that a search by handle upgrades
/// only the memory it looks for. Upgrading another one could leave the search with its last reference, and dropping
/// that takes the lock that the search holds.
struct KnownSharedMemory {
    primitive_storage_handle: u64,
    memory: Weak<SharedMemory>,
}

type SharedMemoriesById = HashMap<u64, KnownSharedMemory>;

fn shared_memories_by_id() -> MutexGuard<'static, SharedMemoriesById> {
    static SHARED_MEMORIES_BY_ID: LazyLock<Mutex<SharedMemoriesById>> = LazyLock::new(Mutex::default);
    SHARED_MEMORIES_BY_ID.lock().unwrap_or_else(PoisonError::into_inner)
}

impl SharedMemory {
    fn new(ty: MemoryType, heap: &super::heap::Heap) -> Option<Arc<Self>> {
        let reservation = Reservation::new(ty, heap)?;
        if !reservation.commit(0, ty.limits.min) {
            return None;
        }
        let data = MemoryData {
            base: reservation.base,
            size: AtomicU64::new(ty.limits.min * PAGE_SIZE),
            address64: u64::from(ty.limits.address_type == AddressType::I64),
            shared: 1,
        };
        let mut memories = shared_memories_by_id();
        // NB: An id that a message carries must not be guessable, as a message can come from another process.
        let random_id = || {
            let mut bytes = [0u8; 8];
            getrandom::fill(&mut bytes).expect("the platform CSPRNG fills buffers");
            u64::from_ne_bytes(bytes)
        };
        let mut id = random_id();
        while id == 0 || memories.contains_key(&id) {
            id = random_id();
        }
        let primitive_storage_handle = reservation.primitive_storage_handle();
        let memory = Arc::new(Self {
            data,
            ty,
            reservation,
            grow_lock: Mutex::default(),
            id,
        });
        memories.insert(
            id,
            KnownSharedMemory {
                primitive_storage_handle,
                memory: Arc::downgrade(&memory),
            },
        );
        Some(memory)
    }

    /// The shared memory of this process that `id` names, if there is one.
    pub fn with_id(id: u64) -> Option<Arc<Self>> {
        shared_memories_by_id().get(&id)?.memory.upgrade()
    }

    /// The shared memory of this process that lives in the primitive storage of `handle`, if there is one.
    pub fn with_primitive_storage_handle(handle: u64) -> Option<Arc<Self>> {
        if handle == 0 {
            return None;
        }
        shared_memories_by_id()
            .values()
            .find(|known| known.primitive_storage_handle == handle)?
            .memory
            .upgrade()
    }

    /// What names the memory in a message to another agent of the process.
    pub fn id(&self) -> u64 {
        self.id
    }

    /// The shared memory whose data `data` is.
    ///
    /// # Safety
    ///
    /// `data` must be the data of a shared memory that lives.
    pub unsafe fn from_data<'a>(data: *const MemoryData) -> &'a Self {
        const { assert!(core::mem::offset_of!(SharedMemory, data) == 0) };
        unsafe { &*data.cast() }
    }

    /// The type of the memory. Its minimum is the size the memory was created with.
    pub fn ty(&self) -> MemoryType {
        self.ty
    }

    pub fn pages(&self) -> u64 {
        self.data.size() / PAGE_SIZE
    }

    /// Grow by `delta` pages, returning the previous size in pages. Every agent that has the memory sees the new size.
    pub fn grow(&self, delta: u64) -> Option<u64> {
        let _guard = self.grow_lock.lock().unwrap_or_else(PoisonError::into_inner);
        let previous = self.pages();
        let new_pages = previous.checked_add(delta)?;
        self.set_pages(previous, new_pages).then_some(previous)
    }

    /// Grow to exactly `new_pages` pages, and return whether the memory has that many then. This is for an agent that
    /// asks for a size, as another agent may grow the memory at the same time. It fails if the memory has more pages.
    pub fn grow_to(&self, new_pages: u64) -> bool {
        let _guard = self.grow_lock.lock().unwrap_or_else(PoisonError::into_inner);
        let previous = self.pages();
        new_pages == previous || (new_pages > previous && self.set_pages(previous, new_pages))
    }

    /// Only called with the grow lock held.
    fn set_pages(&self, previous: u64, new_pages: u64) -> bool {
        if !self.reservation.commit(previous, new_pages) {
            return false;
        }
        // NB: Only now, with the pages committed, may another agent find out that it can access them.
        self.data.size.store(new_pages * PAGE_SIZE, Ordering::SeqCst);
        true
    }
}

impl Drop for SharedMemory {
    fn drop(&mut self) {
        shared_memories_by_id().remove(&self.id);
    }
}

/// A memory of a store. The interpreter sees the data of a memory that is not shared, which is its first field and so
/// at its handle, and the data of a shared memory in its SharedMemory.
#[repr(C)]
pub struct MemoryInstance {
    data: MemoryData,
    pub ty: MemoryType,
    storage: Storage,
}

enum Storage {
    Unshared { reservation: Reservation, pages: u64 },
    Shared(Arc<SharedMemory>),
}

// The reservation is only accessed through the memory that owns it, or is that of a shared memory.
unsafe impl Send for MemoryInstance {}

#[cfg(unix)]
mod platform {
    pub unsafe fn reserve(size: usize) -> *mut u8 {
        let pointer = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_NONE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE,
                -1,
                0,
            )
        };
        if pointer == libc::MAP_FAILED {
            std::ptr::null_mut()
        } else {
            pointer.cast()
        }
    }

    pub unsafe fn commit(address: *mut u8, size: usize) -> bool {
        unsafe { libc::mprotect(address.cast(), size, libc::PROT_READ | libc::PROT_WRITE) == 0 }
    }

    pub unsafe fn release(address: *mut u8, size: usize) {
        unsafe {
            libc::munmap(address.cast(), size);
        }
    }
}

#[cfg(windows)]
mod platform {
    use std::ffi::c_void;

    const MEM_COMMIT: u32 = 0x1000;
    const MEM_RESERVE: u32 = 0x2000;
    const MEM_RELEASE: u32 = 0x8000;
    const PAGE_NOACCESS: u32 = 0x01;
    const PAGE_READWRITE: u32 = 0x04;

    unsafe extern "system" {
        fn VirtualAlloc(address: *mut c_void, size: usize, allocation_type: u32, protect: u32) -> *mut c_void;
        fn VirtualFree(address: *mut c_void, size: usize, free_type: u32) -> i32;
    }

    pub unsafe fn reserve(size: usize) -> *mut u8 {
        unsafe { VirtualAlloc(std::ptr::null_mut(), size, MEM_RESERVE, PAGE_NOACCESS).cast() }
    }

    pub unsafe fn commit(address: *mut u8, size: usize) -> bool {
        unsafe { !VirtualAlloc(address.cast(), size, MEM_COMMIT, PAGE_READWRITE).is_null() }
    }

    pub unsafe fn release(address: *mut u8, _size: usize) {
        unsafe {
            VirtualFree(address.cast(), 0, MEM_RELEASE);
        }
    }
}

impl MemoryInstance {
    pub fn new(ty: MemoryType, heap: &super::heap::Heap) -> Option<Self> {
        if ty.limits.shared {
            return Some(Self::sharing(SharedMemory::new(ty, heap)?));
        }
        let reservation = Reservation::new(ty, heap)?;
        let mut memory = Self {
            data: MemoryData {
                base: reservation.base,
                size: AtomicU64::new(0),
                address64: u64::from(ty.limits.address_type == AddressType::I64),
                shared: 0,
            },
            ty,
            storage: Storage::Unshared { reservation, pages: 0 },
        };
        memory.grow(ty.limits.min)?;
        Some(memory)
    }

    /// A memory over a shared memory, which may be that of another agent.
    pub fn sharing(shared: Arc<SharedMemory>) -> Self {
        Self {
            // NB: Nothing sees this data. A stray use of it finds a memory of no bytes.
            data: MemoryData {
                base: shared.data.base,
                size: AtomicU64::new(0),
                address64: shared.data.address64,
                shared: 1,
            },
            ty: shared.ty,
            storage: Storage::Shared(shared),
        }
    }

    /// The shared memory that this is a memory over, if the memory is shared.
    pub fn shared(&self) -> Option<&Arc<SharedMemory>> {
        match &self.storage {
            Storage::Unshared { .. } => None,
            Storage::Shared(shared) => Some(shared),
        }
    }

    /// The memory at `address` as the interpreter sees it.
    // NB: The pointer is that of the whole memory, not of its first field: the helpers get from the data back to the
    //     memory.
    pub fn data(address: super::MemoryAddress) -> *const MemoryData {
        const { assert!(core::mem::offset_of!(MemoryInstance, data) == 0) };
        // SAFETY: The handles of an instance are those of live memories.
        match unsafe { &address.get().storage } {
            Storage::Unshared { .. } => address.as_ptr().cast(),
            Storage::Shared(shared) => Arc::as_ptr(shared).cast(),
        }
    }

    pub fn pages(&self) -> u64 {
        match &self.storage {
            Storage::Unshared { pages, .. } => *pages,
            Storage::Shared(shared) => shared.pages(),
        }
    }

    pub fn size(&self) -> usize {
        (self.pages() * PAGE_SIZE) as usize
    }

    fn reservation(&self) -> &Reservation {
        match &self.storage {
            Storage::Unshared { reservation, .. } => reservation,
            Storage::Shared(shared) => &shared.reservation,
        }
    }

    /// The handle of the primitive storage the memory lives in, or zero if it is a plain mapping.
    pub fn primitive_storage_handle(&self) -> u64 {
        self.reservation().primitive_storage_handle()
    }

    /// Grow by `delta` pages, returning the previous size in pages.
    pub fn grow(&mut self, delta: u64) -> Option<u64> {
        match &mut self.storage {
            Storage::Unshared { reservation, pages } => {
                let previous = *pages;
                let new_pages = previous.checked_add(delta)?;
                if !reservation.commit(previous, new_pages) {
                    return None;
                }
                *pages = new_pages;
                self.data.size.store(new_pages * PAGE_SIZE, Ordering::SeqCst);
                Some(previous)
            }
            Storage::Shared(shared) => shared.grow(delta),
        }
    }

    /// Copies the bytes to `offset`. The copy takes no slice of the memory, as the bytes of a shared memory are those of
    /// other agents too, whose code may run meanwhile.
    pub fn write(&mut self, offset: usize, bytes: &[u8]) {
        assert!(offset.checked_add(bytes.len()).is_some_and(|end| end <= self.size()));
        // SAFETY: The range is within the committed pages of the memory, which only grows.
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), self.reservation().base.add(offset), bytes.len()) };
    }

    // FIXME: The bytes of a shared memory are those of other agents too, which a slice does not allow for. Only WASI
    //        still takes slices of a memory.
    pub fn bytes(&self) -> &[u8] {
        unsafe { ak::slice_from_raw_parts(self.reservation().base, self.size()) }
    }

    pub fn bytes_mut(&mut self) -> &mut [u8] {
        unsafe { ak::slice_from_raw_parts_mut(self.reservation().base, self.size()) }
    }
}
