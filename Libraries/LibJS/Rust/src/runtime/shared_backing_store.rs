/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The bytes of the fixed-length SharedArrayBuffers of a process, and how a message finds them in another agent of the
//! process.
//!
//! A SharedArrayBuffer never leaves its agent cluster, and an agent cluster never leaves its process, so these bytes are
//! ordinary memory of the process: there is one mapping, at one address, for every agent. A message names a store by its
//! id. The id gives nothing to the process of a receiver that is not this one, which is where a message goes that
//! deserialization then rejects.
//!
//! A message can wait for a long time, and every agent may drop its buffer meanwhile. So a message holds a pin. While the
//! message stays in memory of this process, that is just a count on the stores. A message that goes through a socket can
//! still come back here: BroadcastChannel reaches agents of this process through the browser process, and the ends of a
//! MessagePort can both be in this process with a socket between them. So such a message holds a lifeline instead: the
//! read end of a pipe that the sender's process keeps the stores for. The kernel keeps track of where that read end is,
//! as it does for any descriptor in flight, so it does not matter whether the message is delivered to one receiver, to
//! many, to a process that has no use for it, or to nobody.

use core::ffi::c_int;
use std::collections::HashMap;
use std::sync::{Arc, LazyLock, Mutex, PoisonError, Weak};

use crate::gc::capi::GCPrimitiveStorageHandle;
use crate::gc::primitive_storage::{OutOfMemory, OwnedPrimitiveStorage, ZeroFillNewBytes};
use crate::random::get_random_u64;

/// DataBlock::SharedBackingStore: the bytes of a fixed-length Shared Data Block, which every agent of the process that
/// has the block shares. The last agent to drop it frees the bytes.
pub struct SharedBackingStore {
    storage: OwnedPrimitiveStorage,
    id: u64,
}

// SAFETY: LibGC's primitive storage takes a lock, the storage is never resized, and what agents do to the bytes goes
//         through atomics.
unsafe impl Send for SharedBackingStore {}
// SAFETY: As above.
unsafe impl Sync for SharedBackingStore {}

type StoresById = HashMap<u64, Weak<SharedBackingStore>>;

fn stores_by_id() -> std::sync::MutexGuard<'static, StoresById> {
    static STORES_BY_ID: LazyLock<Mutex<StoresById>> = LazyLock::new(Mutex::default);
    STORES_BY_ID.lock().unwrap_or_else(PoisonError::into_inner)
}

impl SharedBackingStore {
    /// A new zero-filled store of `size` bytes.
    pub fn create(size: usize) -> Result<Arc<Self>, OutOfMemory> {
        let storage = OwnedPrimitiveStorage::allocate(size, ZeroFillNewBytes::Yes)?;
        let mut stores = stores_by_id();
        // NB: An id that a message carries should not be guessable, as a message can come from another process.
        let mut id = get_random_u64();
        while id == 0 || stores.contains_key(&id) {
            id = get_random_u64();
        }
        let store = Arc::new(Self { storage, id });
        stores.insert(id, Arc::downgrade(&store));
        Ok(store)
    }

    /// The store of this process that `id` names, if there is one.
    pub fn with_id(id: u64) -> Option<Arc<Self>> {
        stores_by_id().get(&id)?.upgrade()
    }

    pub fn id(&self) -> u64 {
        self.id
    }

    pub fn handle(&self) -> GCPrimitiveStorageHandle {
        self.storage.handle()
    }

    pub fn size(&self) -> usize {
        self.storage.size()
    }

    pub(crate) fn data(&self) -> *mut u8 {
        self.storage.data()
    }

    pub(crate) fn offset(&self) -> usize {
        self.storage.offset()
    }
}

impl Drop for SharedBackingStore {
    fn drop(&mut self) {
        stores_by_id().remove(&self.id);
    }
}

/// The read end of the pipe of a message.
#[cfg(unix)]
pub type SharedStoreLifeline = std::os::fd::OwnedFd;
#[cfg(windows)]
pub type SharedStoreLifeline = std::os::windows::io::OwnedHandle;

/// The lifeline as the descriptor that LibIPC and C++ pass around, which the caller then owns.
pub fn lifeline_into_raw_descriptor(lifeline: SharedStoreLifeline) -> c_int {
    #[cfg(unix)]
    {
        std::os::fd::IntoRawFd::into_raw_fd(lifeline)
    }
    #[cfg(windows)]
    {
        // AK's to_fd() keeps the low 32 bits, which hold every value a kernel handle can have.
        std::os::windows::io::IntoRawHandle::into_raw_handle(lifeline) as isize as c_int
    }
}

/// The lifeline behind a descriptor of LibIPC.
///
/// # Safety
///
/// `descriptor` must be open, and owned by nothing else.
pub unsafe fn lifeline_from_raw_descriptor(descriptor: c_int) -> SharedStoreLifeline {
    // SAFETY: As the caller guarantees.
    unsafe { crate::gc::shared_memory::owned_from_raw_descriptor(descriptor) }
}

/// A duplicate of the lifeline behind a descriptor of LibIPC, if the system has a descriptor left.
///
/// # Safety
///
/// `descriptor` must stay open for the call.
pub unsafe fn duplicate_lifeline_from_raw_descriptor(descriptor: c_int) -> std::io::Result<SharedStoreLifeline> {
    #[cfg(unix)]
    {
        // SAFETY: As the caller guarantees.
        unsafe { std::os::fd::BorrowedFd::borrow_raw(descriptor) }.try_clone_to_owned()
    }
    #[cfg(windows)]
    {
        // SAFETY: As the caller guarantees. AK's to_handle() sign-extends the int.
        unsafe {
            std::os::windows::io::BorrowedHandle::borrow_raw(descriptor as isize as std::os::windows::io::RawHandle)
        }
        .try_clone_to_owned()
    }
}

/// What keeps the stores that a message names.
pub enum SharedStorePin {
    /// For a message in memory of this process.
    Counted(Vec<Arc<SharedBackingStore>>),
    /// For a message that has been through a socket, or is about to go through one.
    Lifeline(SharedStoreLifeline),
}

impl SharedStorePin {
    /// A lifeline that keeps the same stores, if the system has a descriptor left.
    pub fn duplicate_lifeline(&self) -> Option<SharedStoreLifeline> {
        match self {
            Self::Counted(stores) => pin_stores_for_a_message(stores.clone()).ok(),
            Self::Lifeline(lifeline) => lifeline.try_clone().ok(),
        }
    }

    /// This pin as a lifeline, if the system has a descriptor left. Without one, the stores may go before the message
    /// arrives, which deserializing throws for.
    pub fn into_lifeline(self) -> Option<SharedStoreLifeline> {
        match self {
            Self::Counted(stores) => pin_stores_for_a_message(stores).ok(),
            Self::Lifeline(lifeline) => Some(lifeline),
        }
    }

    /// Lets go of the stores, for a message that is destroyed.
    pub fn release(self) {
        if let Self::Lifeline(lifeline) = self {
            drop(lifeline);
            release_stores_of_lost_messages();
        }
    }
}

/// Stores that a message keeps alive, until the last duplicate of its lifeline is closed.
struct PinnedStores {
    /// The write end of the pipe, which only ever reports that the read end is gone.
    // NB: Windows does not poll it yet, see take_pins_nothing_holds().
    #[cfg_attr(windows, allow(dead_code))]
    write_end: std::io::PipeWriter,
    /// NB: Only dropped for what it keeps alive.
    _stores: Vec<Arc<SharedBackingStore>>,
}

fn pinned_stores() -> std::sync::MutexGuard<'static, Vec<PinnedStores>> {
    static PINNED_STORES: Mutex<Vec<PinnedStores>> = Mutex::new(Vec::new());
    PINNED_STORES.lock().unwrap_or_else(PoisonError::into_inner)
}

#[cfg(unix)]
fn take_pins_nothing_holds(pins: &mut Vec<PinnedStores>) -> Vec<PinnedStores> {
    use std::os::fd::AsRawFd;

    let mut poll_descriptors: Vec<libc::pollfd> = pins
        .iter()
        .map(|pin| libc::pollfd {
            fd: pin.write_end.as_raw_fd(),
            events: 0,
            revents: 0,
        })
        .collect();
    // SAFETY: The array is valid for its length, and the call does not block.
    let result = unsafe { libc::poll(poll_descriptors.as_mut_ptr(), poll_descriptors.len() as libc::nfds_t, 0) };
    if result <= 0 {
        return Vec::new();
    }
    let mut poll_descriptors = poll_descriptors.into_iter();
    pins.extract_if(.., |_| {
        poll_descriptors
            .next()
            .is_some_and(|descriptor| descriptor.revents & (libc::POLLERR | libc::POLLHUP) != 0)
    })
    .collect()
}

// FIXME: Find out on Windows when a pipe has lost its reader. Until then, the stores of a message stay for as long as
//        the process does.
#[cfg(windows)]
fn take_pins_nothing_holds(_: &mut Vec<PinnedStores>) -> Vec<PinnedStores> {
    Vec::new()
}

/// Lets go of the stores of the messages that are gone. A message that is destroyed in this process calls this. One that
/// another process destroys gets noticed the next time a message goes out or is destroyed here.
pub fn release_stores_of_lost_messages() {
    let released = take_pins_nothing_holds(&mut pinned_stores());
    // NB: A store that goes takes other locks.
    drop(released);
}

/// A new lifeline that keeps `stores` for as long as it, or a duplicate of it, is open anywhere.
fn pin_stores_for_a_message(stores: Vec<Arc<SharedBackingStore>>) -> std::io::Result<SharedStoreLifeline> {
    release_stores_of_lost_messages();
    let (read_end, write_end) = std::io::pipe()?;
    pinned_stores().push(PinnedStores {
        write_end,
        _stores: stores,
    });
    Ok(read_end.into())
}
