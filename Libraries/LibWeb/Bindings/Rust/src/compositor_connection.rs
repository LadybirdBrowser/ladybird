/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::mem::MaybeUninit;
use core::ptr::NonNull;

use libcompositing_rust::display_list::canvas_2d_recorder::{AlphaType, ColorSpace};
use libcompositing_rust::ipc_types::Core_AnonymousBuffer;
use libgfx_rust::image_frame::ImageFrameHandle;
use libipc_rust::cpp_connection::{CppConnection, FfiConnection, FfiThreadSafeMessagePoster, ThreadSafeMessagePoster};
use libipc_rust::wire::{EndpointMessage, File, SyncMessage};

unsafe extern "C" {
    fn web_canvas_2d_connection_unref(connection: *mut c_void);
    fn web_canvas_2d_connection_poster(connection: *mut c_void, out_poster: *mut FfiThreadSafeMessagePoster) -> bool;
    fn web_canvas_2d_connection_did_fail_to_post(connection: *mut c_void);
    fn web_compositor_connection_for_sync_requests(connection: *mut c_void, out_connection: *mut FfiConnection)
    -> bool;
    fn web_compositor_connection_register_image_frames(
        connection: *mut c_void,
        image_frames: *const *const c_void,
        image_frame_count: usize,
    ) -> bool;
    fn web_compositor_create_shared_buffer(size: usize, out: &mut FfiSharedBuffer) -> bool;
    fn web_compositor_shared_buffer_release(owner: *mut c_void);
    fn web_compositor_shared_buffer_clone_descriptor(owner: *mut c_void) -> i32;
    fn web_compositor_shared_buffer_image_frame(
        owner: *mut c_void,
        width: i32,
        height: i32,
        alpha_type: AlphaType,
        color_space: ColorSpace,
    ) -> *mut c_void;
    fn web_compositor_connection_serial(connection: *mut c_void) -> u64;
}

#[derive(Clone, Copy)]
pub(crate) struct CompositorConnection(NonNull<c_void>);

// SAFETY (of every call below): The connection is live, and what is passed to it is lent for the call, in which C++
// takes references of its own.
impl CompositorConnection {
    /// # Safety
    ///
    /// `connection` must be a CompositorConnection that outlives the value.
    pub(crate) unsafe fn lent(connection: NonNull<c_void>) -> Self {
        Self(connection)
    }

    pub(crate) fn as_ptr(self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub(crate) fn serial(self) -> u64 {
        unsafe { web_compositor_connection_serial(self.0.as_ptr()) }
    }

    /// # Safety
    ///
    /// The caller owns a reference to the connection, which it gives up.
    pub(crate) unsafe fn release(self) {
        unsafe { web_canvas_2d_connection_unref(self.0.as_ptr()) };
    }

    pub(crate) fn poster_while_compositor_is_reachable(self) -> Option<ThreadSafeMessagePoster> {
        let mut poster = MaybeUninit::<FfiThreadSafeMessagePoster>::uninit();
        let reachable = unsafe { web_canvas_2d_connection_poster(self.0.as_ptr(), poster.as_mut_ptr()) };
        // SAFETY: C++ sets the poster to one reference to the poster of the connection where it answers true.
        reachable.then(|| unsafe { ThreadSafeMessagePoster::adopt(poster.assume_init()) })
    }

    pub(crate) fn lose_compositor(self) {
        unsafe { web_canvas_2d_connection_did_fail_to_post(self.0.as_ptr()) };
    }

    pub(crate) fn post_or_lose_compositor<M: EndpointMessage>(
        self,
        poster: &ThreadSafeMessagePoster,
        message: M,
    ) -> bool {
        let posted = poster.try_post(message).expect("a message to the compositor encodes");
        if !posted {
            self.lose_compositor();
        }
        posted
    }

    pub(crate) fn post<M: EndpointMessage>(self, message: M) -> bool {
        self.poster_while_compositor_is_reachable()
            .is_some_and(|poster| poster.try_post(message).expect("a message to the compositor encodes"))
    }

    pub(crate) fn send_sync<M: SyncMessage>(self, request: M) -> Option<M::Response> {
        let mut connection = MaybeUninit::<FfiConnection>::uninit();
        if !unsafe { web_compositor_connection_for_sync_requests(self.0.as_ptr(), connection.as_mut_ptr()) } {
            return None;
        }
        // SAFETY: C++ set the connection to one reference to the connection of this thread.
        let connection = unsafe { CppConnection::adopt(connection.assume_init()) };
        let response = connection.send_sync(request);
        if response.is_none() {
            self.lose_compositor();
        }
        response
    }

    pub(crate) fn register_image_frames(self, image_frames: &[&ImageFrameHandle]) -> bool {
        let image_frames: Vec<*const c_void> = image_frames.iter().map(|frame| frame.as_raw()).collect();
        unsafe {
            web_compositor_connection_register_image_frames(self.0.as_ptr(), image_frames.as_ptr(), image_frames.len())
        }
    }
}

/// Mirrors `Web::Compositor::FfiSharedBuffer`.
#[repr(C)]
pub(crate) struct FfiSharedBuffer {
    data: *mut u8,
    size: usize,
    descriptor: i32,
    owner: *mut c_void,
}

pub(crate) struct SharedBuffer {
    data: NonNull<u8>,
    size: usize,
    owner: NonNull<c_void>,
}

impl SharedBuffer {
    pub(crate) fn create(size: usize) -> Option<Self> {
        let mut buffer = FfiSharedBuffer {
            data: core::ptr::null_mut(),
            size: 0,
            descriptor: -1,
            owner: core::ptr::null_mut(),
        };
        // SAFETY: C++ fills `buffer` where it answers true.
        if size == 0 || !unsafe { web_compositor_create_shared_buffer(size, &mut buffer) } {
            return None;
        }
        drop(descriptor_into_file(buffer.descriptor));
        Some(Self {
            data: NonNull::new(buffer.data).expect("C++ maps the memory"),
            size: buffer.size,
            owner: NonNull::new(buffer.owner).expect("C++ passes the owner of the memory"),
        })
    }

    pub(crate) fn as_ptr(&self) -> *mut u8 {
        self.data.as_ptr()
    }

    pub(crate) fn len(&self) -> usize {
        self.size
    }

    pub(crate) fn for_message(&self) -> Core_AnonymousBuffer {
        // SAFETY: The value owns the memory.
        let descriptor = unsafe { web_compositor_shared_buffer_clone_descriptor(self.owner.as_ptr()) };
        assert!(descriptor >= 0, "the descriptor of shared memory can be cloned");
        Core_AnonymousBuffer {
            shared_memory: Some((self.size as u64, descriptor_into_file(descriptor))),
        }
    }

    pub(crate) fn into_image_frame(
        self,
        width: i32,
        height: i32,
        alpha_type: AlphaType,
        color_space: ColorSpace,
    ) -> Option<ImageFrameHandle> {
        // SAFETY: The value owns the memory, which the frame shares, and LibWeb hands over a frame it allocated with new,
        //         or null.
        let frame = unsafe {
            web_compositor_shared_buffer_image_frame(self.owner.as_ptr(), width, height, alpha_type, color_space)
        };
        (!frame.is_null()).then(|| unsafe { ImageFrameHandle::adopt(frame) })
    }
}

impl Drop for SharedBuffer {
    fn drop(&mut self) {
        // SAFETY: The value owns the memory, which nothing uses after this.
        unsafe { web_compositor_shared_buffer_release(self.owner.as_ptr()) };
    }
}

fn descriptor_into_file(descriptor: i32) -> File {
    #[cfg(unix)]
    // SAFETY: C++ hands over a descriptor of its own.
    let descriptor = unsafe { <std::os::fd::OwnedFd as std::os::fd::FromRawFd>::from_raw_fd(descriptor) };
    #[cfg(windows)]
    // SAFETY: C++ hands over a descriptor of its own.
    let descriptor = unsafe { libipc_rust::transport_windows::OwnedDescriptor::from_raw(descriptor) };
    File(descriptor)
}
