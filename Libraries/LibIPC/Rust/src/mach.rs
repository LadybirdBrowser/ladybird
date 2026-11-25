/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Mach interface that the transport uses: port rights, messages and file ports.
//!
//! This is one of the crate's two islands of unsafe code. Everything outside it handles ports through `MachPort`,
//! which owns one right and gives it back to the kernel when dropped.

#![allow(unsafe_code)]

use std::ffi::{CStr, c_char, c_int, c_void};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

pub use crate::receive_queue::PayloadBytes;
use crate::transport::{OwnedPayload, clock_monotonic_nanoseconds};

pub type PortName = u32;
pub type KernReturn = i32;

pub const MACH_PORT_NULL: PortName = 0;
pub const MACH_PORT_DEAD: PortName = !0;

pub const KERN_SUCCESS: KernReturn = 0;

pub const MACH_MSG_TYPE_MOVE_RECEIVE: u32 = 16;
pub const MACH_MSG_TYPE_MOVE_SEND: u32 = 17;
pub const MACH_MSG_TYPE_MOVE_SEND_ONCE: u32 = 18;
pub const MACH_MSG_TYPE_COPY_SEND: u32 = 19;
pub const MACH_MSG_TYPE_MAKE_SEND: u32 = 20;
pub const MACH_MSG_TYPE_MAKE_SEND_ONCE: u32 = 21;

pub const MACH_MSGH_BITS_COMPLEX: u32 = 0x8000_0000;

pub const MACH_MSG_PORT_DESCRIPTOR: u8 = 0;
pub const MACH_MSG_OOL_DESCRIPTOR: u8 = 1;
pub const MACH_MSG_VIRTUAL_COPY: u8 = 1;

pub const MACH_SEND_MSG: i32 = 0x1;
pub const MACH_RCV_MSG: i32 = 0x2;
pub const MACH_RCV_LARGE: i32 = 0x4;
// NB: With MACH_RCV_LARGE, a message too large for the buffer names the port it waits on, which a receive on a port set
//     needs to take that message rather than another port's.
const MACH_RCV_LARGE_IDENTITY: i32 = 0x8;
pub const MACH_SEND_TIMEOUT: i32 = 0x10;
pub const MACH_RCV_TIMEOUT: i32 = 0x100;

pub const MACH_MSG_SUCCESS: KernReturn = 0;
pub(crate) const MACH_MSG_MASK: KernReturn = 0x0000_3e00;
const MACH_SEND_INVALID_DATA: KernReturn = 0x1000_0002;
pub const MACH_SEND_INVALID_DEST: KernReturn = 0x1000_0003;
pub const MACH_SEND_TIMED_OUT: KernReturn = 0x1000_0004;
const MACH_SEND_INVALID_VOUCHER: KernReturn = 0x1000_0005;
const MACH_SEND_MSG_TOO_SMALL: KernReturn = 0x1000_0008;
const MACH_SEND_INVALID_REPLY: KernReturn = 0x1000_0009;
pub const MACH_SEND_INVALID_RIGHT: KernReturn = 0x1000_000a;
const MACH_SEND_INVALID_NOTIFY: KernReturn = 0x1000_000b;
pub const MACH_SEND_INVALID_MEMORY: KernReturn = 0x1000_000c;
pub const MACH_SEND_NO_BUFFER: KernReturn = 0x1000_000d;
pub const MACH_SEND_TOO_LARGE: KernReturn = 0x1000_000e;
pub const MACH_SEND_INVALID_TYPE: KernReturn = 0x1000_000f;
const MACH_SEND_INVALID_HEADER: KernReturn = 0x1000_0010;
const MACH_SEND_INVALID_TRAILER: KernReturn = 0x1000_0011;
const MACH_SEND_INVALID_CONTEXT: KernReturn = 0x1000_0012;
const MACH_SEND_INVALID_OPTIONS: KernReturn = 0x1000_0013;
const MACH_SEND_NO_GRANT_DEST: KernReturn = 0x1000_0016;
const MACH_SEND_MSG_FILTERED: KernReturn = 0x1000_0017;
const MACH_RCV_TIMED_OUT: KernReturn = 0x1000_4003;
pub const MACH_RCV_TOO_LARGE: KernReturn = 0x1000_4004;
pub const MACH_RCV_HEADER_ERROR: KernReturn = 0x1000_400b;
pub const MACH_RCV_BODY_ERROR: KernReturn = 0x1000_400c;

pub const MACH_MSG_TIMEOUT_NONE: u32 = 0;

pub const MACH_NOTIFY_SEND_POSSIBLE: i32 = 0o102;
pub const MACH_NOTIFY_NO_SENDERS: i32 = 0o106;
pub const MACH_NOTIFY_SEND_ONCE: i32 = 0o107;
pub const MACH_NOTIFY_DEAD_NAME: i32 = 0o110;

const MACH_PORT_RIGHT_RECEIVE: u32 = 1;
const MACH_PORT_RIGHT_PORT_SET: u32 = 3;

pub const MACH_PORT_TYPE_DNREQUEST: u32 = 0x8000_0000;
pub const MACH_PORT_TYPE_SPREQUEST: u32 = 0x4000_0000;

const MACH_PORT_LIMITS_INFO: i32 = 1;
const MACH_PORT_LIMITS_INFO_COUNT: u32 = 1;
pub(crate) const MACH_PORT_QLIMIT_MAX: i32 = 1024;

pub fn msgh_bits(remote: u32, local: u32) -> u32 {
    remote | (local << 8)
}

pub fn is_valid(name: PortName) -> bool {
    name != MACH_PORT_NULL && name != MACH_PORT_DEAD
}

/// `mach_msg_trailer_t`, which the kernel appends to every received message.
pub const TRAILER_SIZE: usize = 8;
pub const DEFAULT_RECEIVE_BUFFER_SIZE: usize = 65536;

unsafe extern "C" {
    static mach_task_self_: PortName;
    static bootstrap_port: PortName;

    fn mach_msg(
        message: *mut c_void,
        option: i32,
        send_size: u32,
        receive_size: u32,
        receive_name: PortName,
        timeout: u32,
        notify: PortName,
    ) -> KernReturn;
    fn mach_msg_destroy(message: *mut c_void);
    fn mach_port_allocate(task: PortName, right: u32, name: *mut PortName) -> KernReturn;
    fn mach_port_insert_right(task: PortName, name: PortName, right: PortName, right_type: u32) -> KernReturn;
    fn mach_port_deallocate(task: PortName, name: PortName) -> KernReturn;
    fn mach_port_mod_refs(task: PortName, name: PortName, right: u32, delta: i32) -> KernReturn;
    fn mach_port_insert_member(task: PortName, member: PortName, set: PortName) -> KernReturn;
    fn mach_port_extract_member(task: PortName, member: PortName, set: PortName) -> KernReturn;
    fn mach_port_request_notification(
        task: PortName,
        name: PortName,
        message_id: i32,
        sync: u32,
        notify: PortName,
        notify_type: u32,
        previous: *mut PortName,
    ) -> KernReturn;
    fn mach_port_set_attributes(task: PortName, name: PortName, flavor: i32, info: *mut i32, count: u32) -> KernReturn;
    fn mach_port_extract_right(
        task: PortName,
        name: PortName,
        right_type: u32,
        right: *mut PortName,
        acquired_type: *mut u32,
    ) -> KernReturn;
    fn mach_port_type(task: PortName, name: PortName, port_type: *mut u32) -> KernReturn;
    fn vm_deallocate(task: PortName, address: usize, size: usize) -> KernReturn;
    fn mach_error_string(error: KernReturn) -> *const c_char;
    fn fileport_makeport(fd: c_int, port: *mut PortName) -> c_int;
    fn fileport_makefd(port: PortName) -> c_int;
    fn task_get_special_port(task: PortName, which_port: c_int, special_port: *mut PortName) -> KernReturn;
    fn bootstrap_look_up(bootstrap: PortName, service_name: *const c_char, service_port: *mut PortName) -> KernReturn;
    fn bootstrap_register(bootstrap: PortName, service_name: *mut c_char, service_port: PortName) -> KernReturn;
}

fn task_self() -> PortName {
    // NB: The kernel sets up mach_task_self_ before any code runs, and it does not change afterwards.
    unsafe { mach_task_self_ }
}

pub fn error_string(error: KernReturn) -> String {
    // NB: mach_error_string() returns a pointer to a static, NUL-terminated string for any value.
    unsafe { CStr::from_ptr(mach_error_string(error)) }
        .to_string_lossy()
        .into_owned()
}

/// A Mach call's failure, for an `std::io::Error` to carry.
#[derive(Debug)]
pub struct KernelError(pub KernReturn);

impl std::fmt::Display for KernelError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&error_string(self.0))
    }
}

impl std::error::Error for KernelError {}

impl From<KernelError> for std::io::Error {
    fn from(error: KernelError) -> Self {
        Self::other(error)
    }
}

/// Runs the calling thread at the user-interactive quality of service, as `Threading::Thread::set_qos()` does.
pub fn set_current_thread_qos_to_user_interactive() {
    // NB: The call only changes how the calling thread is scheduled. A thread that keeps its default quality of
    //     service still works, so a failure is not an error.
    unsafe {
        libc::pthread_set_qos_class_self_np(libc::qos_class_t::QOS_CLASS_USER_INTERACTIVE, 0);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PortRight {
    Send,
    Receive,
    SendOnce,
    PortSet,
}

/// One right to a Mach port, given back to the kernel when dropped.
#[derive(Debug)]
pub struct MachPort {
    name: PortName,
    right: PortRight,
}

impl MachPort {
    pub fn allocate_receive_right() -> Result<Self, KernReturn> {
        Self::allocate(MACH_PORT_RIGHT_RECEIVE, PortRight::Receive)
    }

    pub fn allocate_port_set() -> Result<Self, KernReturn> {
        Self::allocate(MACH_PORT_RIGHT_PORT_SET, PortRight::PortSet)
    }

    fn allocate(kernel_right: u32, right: PortRight) -> Result<Self, KernReturn> {
        let mut name = MACH_PORT_NULL;
        // NB: name points to a local the call writes the new port's name to.
        let result = unsafe { mach_port_allocate(task_self(), kernel_right, &raw mut name) };
        if result != KERN_SUCCESS {
            return Err(result);
        }
        Ok(Self { name, right })
    }

    /// Takes over a right that this task holds under a name, such as one received in a message.
    pub fn adopt(name: PortName, right: PortRight) -> Self {
        Self { name, right }
    }

    pub fn name(&self) -> PortName {
        self.name
    }

    pub fn right(&self) -> PortRight {
        self.right
    }

    /// Makes a send right to the port this receive right is for.
    pub fn make_send_right(&self) -> Result<Self, KernReturn> {
        assert_eq!(self.right, PortRight::Receive);
        // NB: The call only adds a send right under the receive right's name.
        let result = unsafe { mach_port_insert_right(task_self(), self.name, self.name, MACH_MSG_TYPE_MAKE_SEND) };
        if result != KERN_SUCCESS {
            return Err(result);
        }
        Ok(Self {
            name: self.name,
            right: PortRight::Send,
        })
    }

    pub fn insert_member(&self, member: &MachPort) -> Result<(), KernReturn> {
        assert_eq!(self.right, PortRight::PortSet);
        // NB: Both names are rights this task holds.
        let result = unsafe { mach_port_insert_member(task_self(), member.name, self.name) };
        if result != KERN_SUCCESS {
            return Err(result);
        }
        Ok(())
    }

    pub fn extract_member(&self, member: &MachPort) -> Result<(), KernReturn> {
        assert_eq!(self.right, PortRight::PortSet);
        // NB: Both names are rights this task holds.
        let result = unsafe { mach_port_extract_member(task_self(), member.name, self.name) };
        if result != KERN_SUCCESS {
            return Err(result);
        }
        Ok(())
    }

    /// Takes a member out of this port set by its name, for an owner that no longer holds the member's right here.
    pub fn extract_member_named(&self, member: PortName) -> Result<(), KernReturn> {
        assert_eq!(self.right, PortRight::PortSet);
        // NB: Both are names in this task; a name that is not a member fails without effect.
        let result = unsafe { mach_port_extract_member(task_self(), member, self.name) };
        if result != KERN_SUCCESS {
            return Err(result);
        }
        Ok(())
    }

    /// Raises the number of messages the port queues, so that a peer can send a burst before this side reads it.
    pub fn raise_queue_limit(&self) {
        assert_eq!(self.right, PortRight::Receive);
        let mut limit = MACH_PORT_QLIMIT_MAX;
        // NB: limit is the one integer of mach_port_limits_t that MACH_PORT_LIMITS_INFO reads.
        unsafe {
            mach_port_set_attributes(
                task_self(),
                self.name,
                MACH_PORT_LIMITS_INFO,
                &raw mut limit,
                MACH_PORT_LIMITS_INFO_COUNT,
            );
        }
    }

    /// Has the kernel send `MACH_NOTIFY_NO_SENDERS` to this receive right once no send right to it is left.
    pub fn request_no_senders_notification(&self) -> Result<(), KernReturn> {
        assert_eq!(self.right, PortRight::Receive);
        let mut previous = MACH_PORT_NULL;
        // NB: previous points to a local that the call writes to.
        let result = unsafe {
            mach_port_request_notification(
                task_self(),
                self.name,
                MACH_NOTIFY_NO_SENDERS,
                0,
                self.name,
                MACH_MSG_TYPE_MAKE_SEND_ONCE,
                &raw mut previous,
            )
        };
        if result != KERN_SUCCESS {
            return Err(result);
        }
        // NB: A registration this replaced gives back its send-once right.
        if is_valid(previous) {
            drop(Self::adopt(previous, PortRight::SendOnce));
        }
        Ok(())
    }

    /// Takes back the request for `MACH_NOTIFY_NO_SENDERS`, before the receive right goes to another owner.
    pub fn cancel_no_senders_notification(&self) -> Result<(), KernReturn> {
        assert_eq!(self.right, PortRight::Receive);
        let previous =
            self.request_notification(MACH_NOTIFY_NO_SENDERS, 0, MACH_PORT_NULL, MACH_MSG_TYPE_MAKE_SEND_ONCE)?;
        drop(previous);
        Ok(())
    }

    /// Makes a send-once right to the port this receive right is for. A message sent through it bypasses the queue
    /// limit, and doesn't count as a sender for `MACH_NOTIFY_NO_SENDERS`.
    pub fn make_send_once_right(&self) -> Result<Self, KernReturn> {
        assert_eq!(self.right, PortRight::Receive);
        make_send_once_right_from_receive_right_named(self.name)
    }
}

/// As `MachPort::make_send_once_right()`, for a receive right that something else owns, and keeps while this runs.
pub fn make_send_once_right_from_receive_right_named(receive_right: PortName) -> Result<MachPort, KernReturn> {
    let mut name = MACH_PORT_NULL;
    let mut acquired_type = 0;
    // NB: name and acquired_type point to locals that the call writes to.
    let result = unsafe {
        mach_port_extract_right(
            task_self(),
            receive_right,
            MACH_MSG_TYPE_MAKE_SEND_ONCE,
            &raw mut name,
            &raw mut acquired_type,
        )
    };
    if result != KERN_SUCCESS {
        return Err(result);
    }
    Ok(MachPort::adopt(name, PortRight::SendOnce))
}

impl MachPort {
    /// The `MACH_PORT_TYPE_*` bits of this name in this task, which include the notification requests made on it.
    pub fn port_type(&self) -> Result<u32, KernReturn> {
        let mut port_type = 0;
        // NB: port_type points to a local that the call writes to.
        let result = unsafe { mach_port_type(task_self(), self.name, &raw mut port_type) };
        if result != KERN_SUCCESS {
            return Err(result);
        }
        Ok(port_type)
    }

    /// Asks for `MACH_NOTIFY_SEND_POSSIBLE` on `notify_port` once the queue of the port this send right is for has
    /// room, or at once if it has room already. A port that dies answers with `MACH_NOTIFY_DEAD_NAME` instead.
    ///
    /// Gives back the send-once right of a request on this name that this one replaced.
    pub fn request_send_possible_notification(&self, notify_port: &MachPort) -> Result<Option<Self>, KernReturn> {
        assert_eq!(notify_port.right, PortRight::Receive);
        self.request_notification(
            MACH_NOTIFY_SEND_POSSIBLE,
            1,
            notify_port.name,
            MACH_MSG_TYPE_MAKE_SEND_ONCE,
        )
    }

    /// Takes back a request for `MACH_NOTIFY_SEND_POSSIBLE`, and answers whether it was still armed. One that was not
    /// has fired or is firing, so its notification is on its way.
    pub fn cancel_send_possible_notification(&self) -> bool {
        matches!(
            self.request_notification(
                MACH_NOTIFY_SEND_POSSIBLE,
                0,
                MACH_PORT_NULL,
                MACH_MSG_TYPE_MAKE_SEND_ONCE
            ),
            Ok(Some(_))
        )
    }

    /// Puts back a dead-name request that a send-possible request replaced, with the send-once right it gave back.
    pub fn restore_dead_name_notification(&self, notify_right: Self) {
        assert_eq!(notify_right.right, PortRight::SendOnce);
        let notify_right = std::mem::ManuallyDrop::new(notify_right);
        match self.request_notification(
            MACH_NOTIFY_DEAD_NAME,
            1,
            notify_right.name,
            MACH_MSG_TYPE_MOVE_SEND_ONCE,
        ) {
            // NB: The kernel took the send-once right, and gave back the one this request replaced, if any.
            Ok(replaced) => drop(replaced),
            Err(_) => drop(std::mem::ManuallyDrop::into_inner(notify_right)),
        }
    }

    fn request_notification(
        &self,
        message_id: i32,
        sync: u32,
        notify: PortName,
        notify_type: u32,
    ) -> Result<Option<Self>, KernReturn> {
        let mut previous = MACH_PORT_NULL;
        // NB: previous points to a local that the call writes to.
        let result = unsafe {
            mach_port_request_notification(
                task_self(),
                self.name,
                message_id,
                sync,
                notify,
                notify_type,
                &raw mut previous,
            )
        };
        if result != KERN_SUCCESS {
            return Err(result);
        }
        Ok(is_valid(previous).then(|| Self::adopt(previous, PortRight::SendOnce)))
    }
}

impl Drop for MachPort {
    fn drop(&mut self) {
        if !is_valid(self.name) {
            return;
        }
        // NB: This MachPort owns one right of its kind under its name.
        unsafe {
            match self.right {
                PortRight::Send | PortRight::SendOnce => {
                    mach_port_deallocate(task_self(), self.name);
                }
                PortRight::Receive => {
                    mach_port_mod_refs(task_self(), self.name, MACH_PORT_RIGHT_RECEIVE, -1);
                }
                PortRight::PortSet => {
                    mach_port_mod_refs(task_self(), self.name, MACH_PORT_RIGHT_PORT_SET, -1);
                }
            }
        }
    }
}

/// Wraps a file descriptor in a send right to a file port, closing the descriptor.
pub fn file_port_from_fd(fd: OwnedFd) -> Result<MachPort, std::io::Error> {
    let mut name = MACH_PORT_NULL;
    // NB: fd is open and name points to a local the call writes to.
    let result = unsafe { fileport_makeport(fd.as_raw_fd(), &raw mut name) };
    if result != 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(MachPort::adopt(name, PortRight::Send))
}

/// Opens a file descriptor for the file a file port refers to, giving the send right back.
pub fn fd_from_file_port(port: MachPort) -> Result<OwnedFd, std::io::Error> {
    // NB: The port is a send right this function owns.
    let fd = unsafe { fileport_makefd(port.name()) };
    if fd < 0 {
        return Err(std::io::Error::last_os_error());
    }
    // NB: fileport_makefd() returned a new descriptor that nothing else owns.
    let descriptor = unsafe { OwnedFd::from_raw_fd(fd) };
    // NB: IPC::File::decode() makes its descriptors close-on-exec too.
    if unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) } < 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(descriptor)
}

/// Has writes to a pipe or socket whose reader is gone fail with `EPIPE` instead of raising `SIGPIPE`, which the
/// process does not ignore.
pub fn disable_sigpipe_on_writes(fd: &OwnedFd) -> Result<(), std::io::Error> {
    // NB: As <sys/fcntl.h> numbers it.
    const F_SETNOSIGPIPE: c_int = 73;
    // NB: fd is open, and the call only sets a flag on it.
    if unsafe { libc::fcntl(fd.as_raw_fd(), F_SETNOSIGPIPE, 1) } < 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(())
}

pub const IPC_DATA_MESSAGE_ID: i32 = 0x4950_C001;
pub const IPC_INLINE_DATA_MESSAGE_ID: i32 = 0x4950_C002;
pub const IPC_WAKEUP_MESSAGE_ID: i32 = 0x4950_C003;
pub const IPC_RECEIVE_BARRIER_MESSAGE_ID: i32 = 0x4950_C004;
/// What a transport sends to its own receive right once it finds the peer unreachable, so that the peer's departure
/// shows after every message that the peer sent before.
pub const IPC_PEER_UNREACHABLE_MESSAGE_ID: i32 = 0x4950_C005;

// NB: The largest message that carries its payload inline; a larger payload goes out of line.
const INLINE_MESSAGE_MAX_SIZE: usize = 4096;

const HEADER_SIZE: usize = 24;
const BODY_SIZE: usize = 4;
const PORT_DESCRIPTOR_SIZE: usize = 12;
const OUT_OF_LINE_DESCRIPTOR_SIZE: usize = 16;
const INLINE_PAYLOAD_HEADER_SIZE: usize = size_of::<u32>();

fn round_message_size(size: usize) -> usize {
    size.next_multiple_of(size_of::<u32>())
}

/// A buffer for a Mach message, aligned for its header.
pub struct MessageBuffer {
    words: Vec<u64>,
}

impl MessageBuffer {
    pub fn new(size: usize) -> Self {
        Self {
            words: vec![0; size.div_ceil(8)],
        }
    }

    fn ensure_size(&mut self, size: usize) {
        if self.words.len() * 8 < size {
            self.words.resize(size.div_ceil(8), 0);
        }
    }

    fn size(&self) -> usize {
        self.words.len() * 8
    }

    // NB: A receive buffer lives as long as its thread, so it does not keep the size of the largest message a peer
    //     sent.
    fn shrink_after_large_message(&mut self) {
        if self.size() > DEFAULT_RECEIVE_BUFFER_SIZE {
            *self = Self::new(DEFAULT_RECEIVE_BUFFER_SIZE);
        }
    }

    fn bytes(&self) -> &[u8] {
        // NB: Every initialized word can be read as bytes, and u8 alignment covers the entire slice.
        unsafe { self.words.align_to::<u8>().1 }
    }

    fn bytes_mut(&mut self) -> &mut [u8] {
        // NB: Every byte pattern is a valid u64, and u8 alignment covers the entire slice.
        unsafe { self.words.align_to_mut::<u8>().1 }
    }

    fn header_pointer(&mut self) -> *mut c_void {
        self.words.as_mut_ptr().cast::<c_void>()
    }
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_ne_bytes());
}

fn read_u32(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_ne_bytes(bytes.get(offset..offset + 4)?.try_into().ok()?))
}

fn write_header(bytes: &mut [u8], bits: u32, size: usize, remote_port: PortName, id: i32) {
    write_u32(bytes, 0, bits);
    write_u32(bytes, 4, size as u32);
    write_u32(bytes, 8, remote_port);
    write_u32(bytes, 12, MACH_PORT_NULL);
    write_u32(bytes, 16, MACH_PORT_NULL);
    write_u32(bytes, 20, id as u32);
}

/// Sends a header-only message through a send right this task keeps, as to wake a thread waiting on its port.
pub fn send_header_only_message(destination: &MachPort, id: i32, timeout: Option<u32>) -> KernReturn {
    let mut buffer = [0u32; HEADER_SIZE / size_of::<u32>()];
    // NB: All words are initialized and u8 alignment covers the entire slice.
    let bytes = unsafe { buffer.align_to_mut::<u8>().1 };
    write_header(
        bytes,
        msgh_bits(MACH_MSG_TYPE_COPY_SEND, 0),
        HEADER_SIZE,
        destination.name(),
        id,
    );
    let options = if timeout.is_some() {
        MACH_SEND_MSG | MACH_SEND_TIMEOUT
    } else {
        MACH_SEND_MSG
    };
    // NB: The buffer holds a header-only message to a send right this task keeps, and outlives the call.
    let result = unsafe {
        mach_msg(
            buffer.as_mut_ptr().cast(),
            options,
            HEADER_SIZE as u32,
            0,
            MACH_PORT_NULL,
            timeout.unwrap_or(MACH_MSG_TIMEOUT_NONE),
            MACH_PORT_NULL,
        )
    };
    if result & !MACH_MSG_MASK == MACH_SEND_TIMED_OUT {
        // NB: A timeout returns the copied destination right in this header.
        unsafe { mach_msg_destroy(buffer.as_mut_ptr().cast()) };
    }
    result
}

/// Sends a header-only message through a send-once right, which the message uses up. It waits for nothing, as a
/// send-once right bypasses the queue limit.
pub fn send_header_only_message_through_send_once_right(destination: MachPort, id: i32) -> KernReturn {
    assert_eq!(destination.right(), PortRight::SendOnce);
    let mut buffer = [0u32; HEADER_SIZE / size_of::<u32>()];
    // NB: All words are initialized and u8 alignment covers the entire slice.
    let bytes = unsafe { buffer.align_to_mut::<u8>().1 };
    write_header(
        bytes,
        msgh_bits(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0),
        HEADER_SIZE,
        destination.name(),
        id,
    );
    // NB: The buffer holds a header-only message that moves the send-once right, and outlives the call.
    let result = unsafe {
        mach_msg(
            buffer.as_mut_ptr().cast(),
            MACH_SEND_MSG,
            HEADER_SIZE as u32,
            0,
            MACH_PORT_NULL,
            MACH_MSG_TIMEOUT_NONE,
            MACH_PORT_NULL,
        )
    };
    if result == MACH_MSG_SUCCESS {
        // NB: The message took the right.
        std::mem::forget(destination);
    }
    result
}

/// What became of a data message that was given to the kernel without waiting for room in its destination's queue.
pub enum SendOutcome {
    Sent,
    /// The destination's queue is full. The message keeps what it carries, to be sent again.
    WouldBlock(PendingMessage),
    /// The message is gone with what it carried, for the reason the kernel gave.
    Failed(KernReturn),
}

/// A data message that waits for room in its destination's queue, to be sent in its turn.
pub struct PendingMessage(PendingMessageContents);

enum PendingMessageContents {
    /// Posted behind other messages that wait, and not given to the kernel yet.
    Unsent {
        payload: OwnedPayload,
        ports: Vec<(MachPort, u32)>,
    },
    /// Given to the kernel once already, and handed back.
    Returned(ReturnedMessage),
}

impl PendingMessage {
    pub fn unsent(payload: OwnedPayload, ports: Vec<(MachPort, u32)>) -> Self {
        Self(PendingMessageContents::Unsent { payload, ports })
    }

    /// Tries to send the message without waiting for room, with the buffer as scratch space as `send_data_message()`
    /// takes it.
    pub fn try_send(self, buffer: &mut MessageBuffer, destination: &MachPort) -> SendOutcome {
        match self.0 {
            PendingMessageContents::Unsent { payload, ports } => {
                send_data_message(buffer, destination, (*payload).as_ref(), ports)
            }
            PendingMessageContents::Returned(message) => message.try_send_with(send_without_waiting),
        }
    }
}

/// A data message whose destination's queue was full, as the kernel handed it back: pseudo-received, with the rights
/// it carries under the names they have now, a send right of its own on the destination, and a mapping of its own of
/// an out-of-line payload. It is sent again as it is, and dropping it gives all of that back.
struct ReturnedMessage {
    buffer: MessageBuffer,
    // NB: Zero once the kernel has taken the message, or taken it apart.
    size: usize,
}

impl ReturnedMessage {
    fn try_send_with(mut self, send: impl FnOnce(&mut MessageBuffer, usize) -> KernReturn) -> SendOutcome {
        let result = send(&mut self.buffer, self.size);
        match classify_send_attempt(result) {
            SendAttempt::Sent => {
                self.size = 0;
                SendOutcome::Sent
            }
            SendAttempt::QueueFull => SendOutcome::WouldBlock(PendingMessage(PendingMessageContents::Returned(self))),
            SendAttempt::FailedWithMessageBack => SendOutcome::Failed(result),
            SendAttempt::FailedWithMessageConsumed => {
                self.size = 0;
                SendOutcome::Failed(result)
            }
        }
    }
}

impl Drop for ReturnedMessage {
    fn drop(&mut self) {
        if self.size == 0 {
            return;
        }
        // NB: This message owns everything it carries, including the mapping of an out-of-line payload.
        unsafe { mach_msg_destroy(self.buffer.header_pointer()) };
    }
}

enum SendAttempt {
    Sent,
    QueueFull,
    /// The kernel handed the message back, maybe only in part, and whatever came back is ours to release.
    FailedWithMessageBack,
    /// The kernel took the message apart, and nothing in it is ours any more.
    FailedWithMessageConsumed,
}

fn classify_send_attempt(result: KernReturn) -> SendAttempt {
    if result == MACH_MSG_SUCCESS {
        return SendAttempt::Sent;
    }
    // NB: A send that times out comes back pseudo-received. When the kernel can't hand all of it back, that shows as
    //     extra bits on the timeout, or as a body error, and the message can't go out whole any more.
    if result == MACH_SEND_TIMED_OUT {
        return SendAttempt::QueueFull;
    }
    let error = result & !MACH_MSG_MASK;
    if error == MACH_SEND_TIMED_OUT || error == MACH_RCV_BODY_ERROR || send_error_leaves_descriptors_untouched(result) {
        return SendAttempt::FailedWithMessageBack;
    }
    // NB: Body copyin errors can destroy a prefix of the descriptors without saying how long, so none of the names
    //     in the message may be released here.
    SendAttempt::FailedWithMessageConsumed
}

fn send_without_waiting(buffer: &mut MessageBuffer, size: usize) -> KernReturn {
    // NB: The buffer holds a message of size bytes that owns what it moves, and outlives the call.
    unsafe {
        mach_msg(
            buffer.header_pointer(),
            MACH_SEND_MSG | MACH_SEND_TIMEOUT,
            size as u32,
            0,
            MACH_PORT_NULL,
            0,
            MACH_PORT_NULL,
        )
    }
}

/// Gives LibIPC's data message to the kernel through a send right this task keeps, moving the rights of the
/// attachments with it, without waiting for room in the destination's queue.
///
/// A payload that fits goes inline after a 32-bit size; a larger one goes out of line, borrowed for the call, as
/// `TransportMachPort` sends it. The buffer is scratch space that a message which has to wait is copied out of.
pub fn send_data_message(
    buffer: &mut MessageBuffer,
    destination: &MachPort,
    payload: &[u8],
    ports: Vec<(MachPort, u32)>,
) -> SendOutcome {
    send_data_message_with(buffer, destination, payload, ports, send_without_waiting)
}

fn send_data_message_with(
    buffer: &mut MessageBuffer,
    destination: &MachPort,
    payload: &[u8],
    mut ports: Vec<(MachPort, u32)>,
    send: impl FnOnce(&mut MessageBuffer, usize) -> KernReturn,
) -> SendOutcome {
    if payload.len() > crate::MAX_MESSAGE_PAYLOAD_SIZE {
        return SendOutcome::Failed(MACH_SEND_TOO_LARGE);
    }
    if ports.iter().any(|(port, disposition)| {
        !matches!(
            (port.right(), *disposition),
            (PortRight::Send, MACH_MSG_TYPE_MOVE_SEND | MACH_MSG_TYPE_COPY_SEND)
                | (
                    PortRight::Receive,
                    MACH_MSG_TYPE_MOVE_RECEIVE | MACH_MSG_TYPE_MAKE_SEND | MACH_MSG_TYPE_MAKE_SEND_ONCE
                )
                | (PortRight::SendOnce, MACH_MSG_TYPE_MOVE_SEND_ONCE)
        )
    }) {
        return SendOutcome::Failed(MACH_SEND_INVALID_RIGHT);
    }
    let port_count = ports.len();
    let mut inline_size = HEADER_SIZE + INLINE_PAYLOAD_HEADER_SIZE + payload.len();
    if port_count > 0 {
        inline_size += BODY_SIZE + port_count * PORT_DESCRIPTOR_SIZE;
    }
    let inline_size = round_message_size(inline_size);
    let send_payload_inline = inline_size <= INLINE_MESSAGE_MAX_SIZE;
    let message_size = if send_payload_inline {
        inline_size
    } else {
        round_message_size(HEADER_SIZE + BODY_SIZE + port_count * PORT_DESCRIPTOR_SIZE + OUT_OF_LINE_DESCRIPTOR_SIZE)
    };
    let out_of_line_descriptor_offset = HEADER_SIZE + BODY_SIZE + port_count * PORT_DESCRIPTOR_SIZE;

    buffer.ensure_size(message_size);
    {
        let bytes = buffer.bytes_mut();
        bytes[..message_size].fill(0);
        let id = if send_payload_inline {
            IPC_INLINE_DATA_MESSAGE_ID
        } else {
            IPC_DATA_MESSAGE_ID
        };
        let mut bits = msgh_bits(MACH_MSG_TYPE_COPY_SEND, 0);
        let complex = port_count > 0 || !send_payload_inline;
        if complex {
            bits |= MACH_MSGH_BITS_COMPLEX;
        }
        write_header(bytes, bits, message_size, destination.name(), id);

        let mut offset = HEADER_SIZE;
        if complex {
            let descriptor_count = port_count + usize::from(!send_payload_inline);
            write_u32(bytes, offset, descriptor_count as u32);
            offset += BODY_SIZE;
            for (port, disposition) in &ports {
                write_u32(bytes, offset, port.name());
                write_u32(
                    bytes,
                    offset + 8,
                    (*disposition << 16) | (u32::from(MACH_MSG_PORT_DESCRIPTOR) << 24),
                );
                offset += PORT_DESCRIPTOR_SIZE;
            }
        }

        if send_payload_inline {
            write_u32(bytes, offset, payload.len() as u32);
            offset += INLINE_PAYLOAD_HEADER_SIZE;
            bytes[offset..offset + payload.len()].copy_from_slice(payload);
        } else {
            bytes[offset..offset + 8].copy_from_slice(&(payload.as_ptr() as u64).to_ne_bytes());
            write_u32(
                bytes,
                offset + 8,
                (u32::from(MACH_MSG_VIRTUAL_COPY) << 8) | (u32::from(MACH_MSG_OOL_DESCRIPTOR) << 24),
            );
            write_u32(bytes, offset + 12, payload.len() as u32);
        }
    }

    // NB: The message now owns the rights it moves, so their owners here must not give them back.
    for (port, disposition) in &mut ports {
        if matches!(
            *disposition,
            MACH_MSG_TYPE_MOVE_RECEIVE | MACH_MSG_TYPE_MOVE_SEND | MACH_MSG_TYPE_MOVE_SEND_ONCE
        ) {
            port.name = MACH_PORT_NULL;
        }
    }

    // NB: Until the kernel maps a copy of its own, the message's out-of-line descriptor points at the borrowed payload,
    //     which destroying the message must not deallocate.
    let forget_borrowed_payload = |buffer: &mut MessageBuffer| {
        if send_payload_inline {
            return false;
        }
        let bytes = buffer.bytes_mut();
        let descriptor =
            &mut bytes[out_of_line_descriptor_offset..out_of_line_descriptor_offset + OUT_OF_LINE_DESCRIPTOR_SIZE];
        if u64::from_ne_bytes(descriptor[..8].try_into().unwrap_or_default()) != payload.as_ptr() as u64 {
            return false;
        }
        descriptor[..8].fill(0);
        descriptor[12..16].fill(0);
        true
    };

    let result = send(buffer, message_size);
    match classify_send_attempt(result) {
        SendAttempt::Sent => SendOutcome::Sent,
        SendAttempt::QueueFull => {
            let mut returned = ReturnedMessage {
                buffer: MessageBuffer::new(message_size + TRAILER_SIZE),
                size: message_size,
            };
            returned.buffer.bytes_mut()[..message_size].copy_from_slice(&buffer.bytes()[..message_size]);
            // NB: The kernel hands back a mapping of its own of an out-of-line payload. One that still points at the
            //     borrowed payload would outlive it, so the message can't wait.
            if forget_borrowed_payload(&mut returned.buffer) {
                drop(returned);
                return SendOutcome::Failed(result);
            }
            SendOutcome::WouldBlock(PendingMessage(PendingMessageContents::Returned(returned)))
        }
        SendAttempt::FailedWithMessageBack => {
            forget_borrowed_payload(buffer);
            // NB: What came back of the message is ours, and the payload no longer in it.
            unsafe { mach_msg_destroy(buffer.header_pointer()) };
            SendOutcome::Failed(result)
        }
        SendAttempt::FailedWithMessageConsumed => SendOutcome::Failed(result),
    }
}

fn send_error_leaves_descriptors_untouched(result: KernReturn) -> bool {
    // NB: With inline port descriptors and one virtual-copy region, these errors precede descriptor copyin.
    matches!(
        result,
        MACH_SEND_INVALID_DATA
            | MACH_SEND_INVALID_DEST
            | MACH_SEND_INVALID_VOUCHER
            | MACH_SEND_MSG_TOO_SMALL
            | MACH_SEND_INVALID_REPLY
            | MACH_SEND_INVALID_NOTIFY
            | MACH_SEND_NO_BUFFER
            | MACH_SEND_TOO_LARGE
            | MACH_SEND_INVALID_HEADER
            | MACH_SEND_INVALID_TRAILER
            | MACH_SEND_INVALID_CONTEXT
            | MACH_SEND_INVALID_OPTIONS
            | MACH_SEND_NO_GRANT_DEST
            | MACH_SEND_MSG_FILTERED
    )
}

fn right_for_disposition(disposition: u32) -> PortRight {
    match disposition {
        MACH_MSG_TYPE_MOVE_RECEIVE => PortRight::Receive,
        MACH_MSG_TYPE_MOVE_SEND_ONCE => PortRight::SendOnce,
        _ => PortRight::Send,
    }
}

pub struct ReceivedMessage {
    pub id: i32,
    pub local_port: PortName,
    /// For a LibIPC data message, its bytes and the rights it moved, with their dispositions.
    pub data: Option<(PayloadBytes, Vec<(MachPort, u32)>)>,
    /// For a message with the id of `MACH_NOTIFY_DEAD_NAME`, the name it says has died. Only the kernel's own
    /// notification holds a reference to that name, which its receiver releases; on a port that a peer can send to, the
    /// name is whatever the peer wrote, and refers to nothing of this task's.
    pub dead_name: Option<PortName>,
}

#[derive(Debug)]
pub enum ReceiveError {
    /// A message that could not be received whole is gone. The kernel names the port it came in on, if it got that far.
    Kernel { code: KernReturn, local_port: PortName },
    /// A message that isn't shaped as LibIPC sends one, on the port it came in on.
    Malformed { what: &'static str, local_port: PortName },
}

/// Receives the next message from a port or port set, growing the buffer for a large one.
pub fn receive_message(buffer: &mut MessageBuffer, from: &MachPort) -> Result<ReceivedMessage, ReceiveError> {
    receive(buffer, from, None).map(|message| message.expect("a receive without a timeout returns a message"))
}

/// Receives the next message from a port or port set if one comes within the timeout.
pub fn receive_message_within(
    buffer: &mut MessageBuffer,
    from: &MachPort,
    timeout_in_milliseconds: u32,
) -> Result<Option<ReceivedMessage>, ReceiveError> {
    receive(buffer, from, Some(timeout_in_milliseconds))
}

fn receive(
    buffer: &mut MessageBuffer,
    from: &MachPort,
    timeout_in_milliseconds: Option<u32>,
) -> Result<Option<ReceivedMessage>, ReceiveError> {
    // NB: A failed receive names the port of its message here, if the kernel gets that far, so nothing is left over
    //     from the last message.
    write_u32(buffer.bytes_mut(), 12, MACH_PORT_NULL);
    let mut options = MACH_RCV_LARGE | MACH_RCV_LARGE_IDENTITY;
    if timeout_in_milliseconds.is_some() {
        options |= MACH_RCV_TIMEOUT;
    }
    // NB: A retry for a large message keeps to the caller's timeout: something else may take that message meanwhile.
    let deadline = timeout_in_milliseconds
        .map(|timeout| clock_monotonic_nanoseconds().saturating_add(i64::from(timeout) * 1_000_000));
    let remaining_timeout = || match deadline {
        Some(deadline) => {
            let remaining = deadline.saturating_sub(clock_monotonic_nanoseconds()).max(0) as u64;
            u32::try_from(remaining.div_ceil(1_000_000)).unwrap_or(u32::MAX)
        }
        None => MACH_MSG_TIMEOUT_NONE,
    };
    let mut result = receive_into(buffer, from.name(), options, remaining_timeout());
    // NB: The message stays queued, and its header says how large a buffer it needs and which port it waits on. The
    //     retry receives from that port alone, as on a port set it could take another port's message, which need not
    //     fit either.
    while result == MACH_RCV_TOO_LARGE {
        let port = read_u32(buffer.bytes(), 12).unwrap_or(MACH_PORT_NULL);
        let mut needed = read_u32(buffer.bytes(), 4).unwrap_or(0) as usize + TRAILER_SIZE;
        if needed <= buffer.size() {
            needed = buffer.size() * 2;
        }
        buffer.ensure_size(needed);
        result = receive_into(buffer, port, options, remaining_timeout());
    }
    if result == MACH_RCV_TIMED_OUT {
        buffer.shrink_after_large_message();
        return Ok(None);
    }
    let message = if result != MACH_MSG_SUCCESS {
        Err(ReceiveError::Kernel {
            code: result,
            local_port: read_u32(buffer.bytes(), 12).unwrap_or(MACH_PORT_NULL),
        })
    } else {
        parse_received_message(buffer).map(Some)
    };
    buffer.shrink_after_large_message();
    message
}

fn receive_into(buffer: &mut MessageBuffer, from: PortName, options: i32, timeout: u32) -> KernReturn {
    let size = u32::try_from(buffer.size()).unwrap_or(u32::MAX);
    // NB: The buffer is aligned for a header, and the call writes at most size bytes into it.
    unsafe {
        mach_msg(
            buffer.header_pointer(),
            MACH_RCV_MSG | options,
            0,
            size,
            from,
            timeout,
            MACH_PORT_NULL,
        )
    }
}

struct MessageResources<'a>(&'a mut MessageBuffer);

impl Drop for MessageResources<'_> {
    fn drop(&mut self) {
        // NB: This message owns its moved rights and deallocating memory regions. Descriptors adopted by Rust
        //     owners have been cleared; mach_msg_destroy() releases the remaining resources.
        unsafe { mach_msg_destroy(self.0.header_pointer()) };
    }
}

fn parse_received_message(buffer: &mut MessageBuffer) -> Result<ReceivedMessage, ReceiveError> {
    // NB: The kernel validates the Mach descriptor layout, but not the LibIPC layout. Keep ownership of all received
    //     resources, including header rights, until individual descriptors transfer into Rust owners.
    let resources = MessageResources(buffer);
    let bytes = resources.0.bytes_mut();
    let local_port = read_u32(bytes, 12).unwrap_or(MACH_PORT_NULL);
    let malformed = |what| ReceiveError::Malformed { what, local_port };
    let bits = read_u32(bytes, 0).ok_or(malformed("header"))?;
    let size = read_u32(bytes, 4).ok_or(malformed("header"))? as usize;
    let id = read_u32(bytes, 20).ok_or(malformed("header"))? as i32;
    if size > bytes.len() || size < HEADER_SIZE {
        return Err(malformed("size"));
    }
    let bytes = &mut bytes[..size];
    if id == MACH_NOTIFY_DEAD_NAME {
        // NB: After the header comes an NDR record, then the name.
        let name = read_u32(bytes, HEADER_SIZE + 8).ok_or(malformed("dead-name notification"))?;
        return Ok(ReceivedMessage {
            id,
            local_port,
            data: None,
            dead_name: is_valid(name).then_some(name),
        });
    }
    if id != IPC_DATA_MESSAGE_ID && id != IPC_INLINE_DATA_MESSAGE_ID {
        return Ok(ReceivedMessage {
            id,
            local_port,
            data: None,
            dead_name: None,
        });
    }

    let mut ports = Vec::new();
    let mut offset = HEADER_SIZE;
    let mut out_of_line = None;
    if bits & MACH_MSGH_BITS_COMPLEX != 0 {
        let descriptor_count = read_u32(bytes, offset).ok_or(malformed("body"))? as usize;
        offset += BODY_SIZE;
        let port_count = if id == IPC_DATA_MESSAGE_ID {
            descriptor_count.checked_sub(1).ok_or(malformed("descriptors"))?
        } else {
            descriptor_count
        };
        for _ in 0..port_count {
            let name = read_u32(bytes, offset).ok_or(malformed("port descriptor"))?;
            let descriptor_bits = read_u32(bytes, offset + 8).ok_or(malformed("port descriptor"))?;
            let disposition = (descriptor_bits >> 16) & 0xff;
            // NB: A right whose port died before this message was received arrives as MACH_PORT_DEAD, which is the
            //     kernel reporting a fact about the peer rather than a malformed message. Adopt it like any other, as
            //     TransportMachPort does, and leave it to whoever takes the attachment. The name holds no right in
            //     this task, so dropping it gives nothing back.
            if (descriptor_bits >> 24) as u8 != MACH_MSG_PORT_DESCRIPTOR
                || name == MACH_PORT_NULL
                || !matches!(
                    disposition,
                    MACH_MSG_TYPE_MOVE_RECEIVE | MACH_MSG_TYPE_MOVE_SEND | MACH_MSG_TYPE_MOVE_SEND_ONCE
                )
            {
                return Err(malformed("port descriptor"));
            }
            let port = MachPort::adopt(name, right_for_disposition(disposition));
            write_u32(bytes, offset, MACH_PORT_NULL);
            ports.push((port, disposition));
            offset += PORT_DESCRIPTOR_SIZE;
        }
        if id == IPC_DATA_MESSAGE_ID {
            let address = u64::from_ne_bytes(
                bytes
                    .get(offset..offset + 8)
                    .ok_or(malformed("out-of-line descriptor"))?
                    .try_into()
                    .unwrap_or_default(),
            );
            let descriptor_bits = read_u32(bytes, offset + 8).ok_or(malformed("out-of-line descriptor"))?;
            let payload_size = read_u32(bytes, offset + 12).ok_or(malformed("out-of-line descriptor"))? as usize;
            if (descriptor_bits >> 24) as u8 != MACH_MSG_OOL_DESCRIPTOR {
                return Err(malformed("out-of-line descriptor"));
            }
            // NB: The kernel mapped this region into this task for the message, and nothing else owns it.
            out_of_line = Some(unsafe { VmRegion::adopt(address as *const c_void, payload_size) });
            bytes[offset..offset + 8].fill(0);
            write_u32(bytes, offset + 12, 0);
        }
    } else if id == IPC_DATA_MESSAGE_ID {
        return Err(malformed("out-of-line message without descriptors"));
    }

    let payload = match out_of_line {
        Some(region) => PayloadBytes::OutOfLine(region),
        None => {
            let payload_size = read_u32(bytes, offset).ok_or(malformed("inline payload"))? as usize;
            offset += INLINE_PAYLOAD_HEADER_SIZE;
            if payload_size > size.saturating_sub(offset) {
                return Err(malformed("inline payload size"));
            }
            PayloadBytes::Inline(bytes[offset..offset + payload_size].to_vec())
        }
    };
    Ok(ReceivedMessage {
        id,
        local_port,
        data: Some((payload, ports)),
        dead_name: None,
    })
}

/// Memory the kernel mapped into this task for an out-of-line message payload, which this owns.
pub struct VmRegion {
    address: usize,
    size: usize,
}

impl VmRegion {
    /// # Safety
    ///
    /// The region must be one the kernel mapped into this task for a received message, and nothing else may own it.
    pub unsafe fn adopt(address: *const c_void, size: usize) -> Self {
        Self {
            address: address as usize,
            size,
        }
    }

    #[expect(clippy::disallowed_methods, reason = "The nonempty slice borrows an owned mapping.")]
    pub fn bytes(&self) -> &[u8] {
        if self.size == 0 {
            return &[];
        }
        // NB: The kernel mapped size readable bytes at address, which this region owns until it is dropped.
        unsafe { std::slice::from_raw_parts(self.address as *const u8, self.size) }
    }
}

impl Drop for VmRegion {
    fn drop(&mut self) {
        if self.size == 0 {
            return;
        }
        // NB: This region owns the mapping.
        unsafe {
            vm_deallocate(task_self(), self.address, self.size);
        }
    }
}

// NB: A VmRegion is plain memory that this task owns; nothing ties it to the thread that received it.
unsafe impl Send for VmRegion {}

// NB: As <mach/task_special_ports.h> numbers it. 1 would be TASK_KERNEL_PORT, the task control port.
const TASK_NAME_PORT: c_int = 3;
const SELF_TASK_NAME_PORT_MESSAGE_ID: i32 = 0x1234_CAFE;
// NB: The same number as IPC_INLINE_DATA_MESSAGE_ID, but this reply only ever arrives on the bootstrap reply port.
const IPC_CHANNEL_PORTS_MESSAGE_ID: i32 = 0x4950_C002;
const BOOTSTRAP_SEND_TIMEOUT_MS: u32 = 100;
/// How long a helper waits for its bootstrap server to answer, unless it has a deadline of its own.
pub const DEFAULT_BOOTSTRAP_REPLY_TIMEOUT_IN_MILLISECONDS: u32 = 5000;
const BOOTSTRAP_REQUEST_SIZE: usize = HEADER_SIZE + BODY_SIZE + PORT_DESCRIPTOR_SIZE;

/// Looks up a service that a process registered with the bootstrap server, as `Core::MachPort::look_up_from_bootstrap_server()`.
pub fn look_up_from_bootstrap_server(service_name: &str) -> Result<MachPort, std::io::Error> {
    let name = std::ffi::CString::new(service_name).map_err(|_| std::io::ErrorKind::InvalidInput)?;
    let mut port = MACH_PORT_NULL;
    // NB: The name is NUL-terminated and port points to a local that the call writes to.
    let result = unsafe { bootstrap_look_up(bootstrap_port, name.as_ptr(), &raw mut port) };
    if result != KERN_SUCCESS {
        return Err(KernelError(result).into());
    }
    Ok(MachPort::adopt(port, PortRight::Send))
}

/// Registers a receive right with the bootstrap server, for other processes to look up by the name, as
/// `Core::MachPort::register_with_bootstrap_server()` does.
pub fn register_with_bootstrap_server(receive_right: &MachPort, service_name: &str) -> Result<(), std::io::Error> {
    assert_eq!(receive_right.right(), PortRight::Receive);
    // NB: A name_t holds 128 characters, including the terminating NUL.
    if service_name.len() > 127 {
        return Err(std::io::Error::from_raw_os_error(libc::E2BIG));
    }
    let name = std::ffi::CString::new(service_name).map_err(|_| std::io::ErrorKind::InvalidInput)?;
    // NB: The name is NUL-terminated, and the call makes a send right of its own from the receive right.
    let result = unsafe { bootstrap_register(bootstrap_port, name.as_ptr().cast_mut(), receive_right.name()) };
    if result != KERN_SUCCESS {
        return Err(KernelError(result).into());
    }
    Ok(())
}

/// Why a bootstrap request did not get this process the ports of its transport.
#[derive(Debug)]
pub enum BootstrapError {
    /// A Mach call failed, at a step of the exchange.
    Kernel {
        step: &'static str,
        code: KernReturn,
    },
    /// The server let go of the reply port without answering.
    ServerClosedBeforeReplying,
    MalformedReply,
}

impl std::fmt::Display for BootstrapError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Kernel { step, code } => write!(formatter, "{step}: {}", error_string(*code)),
            Self::ServerClosedBeforeReplying => formatter.write_str("Mach bootstrap server closed before replying"),
            Self::MalformedReply => formatter.write_str("malformed Mach bootstrap reply"),
        }
    }
}

impl std::error::Error for BootstrapError {}

/// Asks a `TransportBootstrapMachServer` for the ports of this process's transport, as
/// `IPC::bootstrap_transport_from_server_port()` does, waiting no longer than the timeout for the reply.
///
/// Returns the receive right and the send right of this end, in that order.
pub fn bootstrap_transport_from_server_port(
    server_port: &MachPort,
    reply_timeout_in_milliseconds: u32,
) -> Result<(MachPort, MachPort), BootstrapError> {
    let kernel_error = |step| move |code| BootstrapError::Kernel { step, code };
    let reply_port = MachPort::allocate_receive_right().map_err(kernel_error("allocating the reply port"))?;

    // NB: Send only the task name port. It is enough for task_info() statistics, while the task control port would
    //     give the server full control over this process.
    let mut raw_task_name_port = MACH_PORT_NULL;
    // NB: raw_task_name_port points to a local that the call writes to.
    let result = unsafe { task_get_special_port(task_self(), TASK_NAME_PORT, &raw mut raw_task_name_port) };
    if result != KERN_SUCCESS {
        return Err(kernel_error("getting the task name port")(result));
    }
    let task_name_port = MachPort::adopt(raw_task_name_port, PortRight::Send);

    let mut request = [0u32; BOOTSTRAP_REQUEST_SIZE / size_of::<u32>()];
    {
        // NB: All words are initialized and u8 alignment covers the entire slice.
        let bytes = unsafe { request.align_to_mut::<u8>().1 };
        write_header(
            bytes,
            msgh_bits(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | MACH_MSGH_BITS_COMPLEX,
            BOOTSTRAP_REQUEST_SIZE,
            server_port.name(),
            SELF_TASK_NAME_PORT_MESSAGE_ID,
        );
        write_u32(bytes, 12, reply_port.name());
        write_u32(bytes, HEADER_SIZE, 1);
        write_u32(bytes, HEADER_SIZE + BODY_SIZE, task_name_port.name());
        write_u32(
            bytes,
            HEADER_SIZE + BODY_SIZE + 8,
            (MACH_MSG_TYPE_COPY_SEND << 16) | (u32::from(MACH_MSG_PORT_DESCRIPTOR) << 24),
        );
    }
    // NB: The request only copies send rights that this task keeps, and outlives the call.
    let result = unsafe {
        mach_msg(
            request.as_mut_ptr().cast(),
            MACH_SEND_MSG | MACH_SEND_TIMEOUT,
            BOOTSTRAP_REQUEST_SIZE as u32,
            0,
            MACH_PORT_NULL,
            BOOTSTRAP_SEND_TIMEOUT_MS,
            MACH_PORT_NULL,
        )
    };
    if result != KERN_SUCCESS {
        if result & !MACH_MSG_MASK == MACH_SEND_TIMED_OUT {
            // NB: A timeout returns the copied rights and the made send-once right in this header.
            unsafe { mach_msg_destroy(request.as_mut_ptr().cast()) };
        }
        return Err(kernel_error("sending the bootstrap request")(result));
    }
    drop(task_name_port);

    let mut reply = MessageBuffer::new(HEADER_SIZE + BODY_SIZE + 2 * PORT_DESCRIPTOR_SIZE + TRAILER_SIZE);
    let reply_capacity = reply.bytes().len() as u32;
    // NB: The buffer is large enough for the reply and its trailer, and outlives the call.
    let result = unsafe {
        mach_msg(
            reply.header_pointer(),
            MACH_RCV_MSG | MACH_RCV_TIMEOUT,
            0,
            reply_capacity,
            reply_port.name(),
            reply_timeout_in_milliseconds,
            MACH_PORT_NULL,
        )
    };
    if result != KERN_SUCCESS {
        return Err(kernel_error("receiving the bootstrap reply")(result));
    }

    // NB: Whatever this function does not adopt goes back to the kernel with the message. Adopted names are cleared
    //     from it first.
    let resources = MessageResources(&mut reply);
    let bytes = resources.0.bytes_mut();
    let bits = read_u32(bytes, 0).unwrap_or(0);
    let id = read_u32(bytes, 20).unwrap_or(0) as i32;
    if id == MACH_NOTIFY_SEND_ONCE {
        return Err(BootstrapError::ServerClosedBeforeReplying);
    }
    if id != IPC_CHANNEL_PORTS_MESSAGE_ID
        || bits & MACH_MSGH_BITS_COMPLEX == 0
        || read_u32(bytes, HEADER_SIZE) != Some(2)
    {
        return Err(BootstrapError::MalformedReply);
    }
    let receive_offset = HEADER_SIZE + BODY_SIZE;
    let send_offset = receive_offset + PORT_DESCRIPTOR_SIZE;
    let (Some((receive_name, MACH_MSG_TYPE_MOVE_RECEIVE)), Some((send_name, MACH_MSG_TYPE_MOVE_SEND))) = (
        read_port_descriptor(bytes, receive_offset),
        read_port_descriptor(bytes, send_offset),
    ) else {
        return Err(BootstrapError::MalformedReply);
    };
    write_u32(bytes, receive_offset, MACH_PORT_NULL);
    write_u32(bytes, send_offset, MACH_PORT_NULL);
    drop(resources);
    Ok((
        MachPort::adopt(receive_name, PortRight::Receive),
        MachPort::adopt(send_name, PortRight::Send),
    ))
}

// NB: The name and disposition of a port descriptor, if it is one.
fn read_port_descriptor(bytes: &[u8], offset: usize) -> Option<(PortName, u32)> {
    let name = read_u32(bytes, offset)?;
    let descriptor_bits = read_u32(bytes, offset + 8)?;
    ((descriptor_bits >> 24) as u8 == MACH_MSG_PORT_DESCRIPTOR).then_some((name, (descriptor_bits >> 16) & 0xff))
}

/// A request for the ports of a transport, from the process the audit trailer names.
pub struct BootstrapRequest {
    pub pid: i32,
    pub task_name_port: MachPort,
    /// The send-once right to answer through, which a request need not have.
    pub reply_port: Option<MachPort>,
}

pub enum ReceivedBootstrapRequest {
    Request(BootstrapRequest),
    /// A message that is not a well-formed request, which is dropped with what it carries, and what it was.
    Ignored(&'static str),
}

/// Waits for the next message on a bootstrap server's receive right. Any process that can look up the server's name
/// can send to it, so every field relied on is checked, and the sender of a malformed message can't make this fail.
pub fn receive_bootstrap_request(server_port: &MachPort) -> Result<ReceivedBootstrapRequest, KernReturn> {
    // NB: As <mach/message.h> lays out mach_msg_audit_trailer_t, and numbers its elements.
    const AUDIT_TRAILER_SIZE: usize = 52;
    const MACH_RCV_TRAILER_AUDIT: i32 = 3;
    const PID_OFFSET_IN_AUDIT_TRAILER: usize = 20 + 5 * size_of::<u32>();
    const MACH_RCV_INTERRUPTED: KernReturn = 0x1000_4005;

    let mut buffer = MessageBuffer::new(BOOTSTRAP_REQUEST_SIZE + AUDIT_TRAILER_SIZE);
    let capacity = buffer.size() as u32;
    // NB: The buffer is aligned for a header, and the call writes at most its size into it.
    let result = unsafe {
        mach_msg(
            buffer.header_pointer(),
            MACH_RCV_MSG | (MACH_RCV_TRAILER_AUDIT << 24),
            0,
            capacity,
            server_port.name(),
            MACH_MSG_TIMEOUT_NONE,
            MACH_PORT_NULL,
        )
    };
    match result {
        MACH_MSG_SUCCESS => {}
        // NB: The kernel has already destroyed such a message and its rights.
        MACH_RCV_TOO_LARGE | MACH_RCV_HEADER_ERROR | MACH_RCV_BODY_ERROR | MACH_RCV_INTERRUPTED => {
            return Ok(ReceivedBootstrapRequest::Ignored(
                "a Mach bootstrap message that could not be received",
            ));
        }
        error => return Err(error),
    }

    let resources = MessageResources(&mut buffer);
    let bytes = resources.0.bytes_mut();
    let bits = read_u32(bytes, 0).unwrap_or(0);
    let size = read_u32(bytes, 4).unwrap_or(0) as usize;
    let reply_port = read_u32(bytes, 8).unwrap_or(MACH_PORT_NULL);
    let id = read_u32(bytes, 20).unwrap_or(0) as i32;
    let descriptor_offset = HEADER_SIZE + BODY_SIZE;
    let is_well_formed = id == SELF_TASK_NAME_PORT_MESSAGE_ID
        && bits & MACH_MSGH_BITS_COMPLEX != 0
        && size == BOOTSTRAP_REQUEST_SIZE
        && (bits >> 8) & 0x1f == MACH_MSG_TYPE_MOVE_SEND
        // NB: The reply port is optional, but when present it must be the send-once right the client made.
        && (reply_port == MACH_PORT_NULL || bits & 0x1f == MACH_MSG_TYPE_MOVE_SEND_ONCE)
        && read_u32(bytes, HEADER_SIZE) == Some(1)
        && matches!(read_port_descriptor(bytes, descriptor_offset), Some((_, MACH_MSG_TYPE_MOVE_SEND)));
    if !is_well_formed {
        return Ok(ReceivedBootstrapRequest::Ignored("a malformed Mach bootstrap message"));
    }

    // NB: The audit trailer follows the message, so its position is only known for a well-formed request.
    let pid = read_u32(bytes, BOOTSTRAP_REQUEST_SIZE + PID_OFFSET_IN_AUDIT_TRAILER).unwrap_or(0) as i32;
    let task_name_port = read_u32(bytes, descriptor_offset).unwrap_or(MACH_PORT_NULL);
    write_u32(bytes, descriptor_offset, MACH_PORT_NULL);
    write_u32(bytes, 8, MACH_PORT_NULL);
    drop(resources);
    Ok(ReceivedBootstrapRequest::Request(BootstrapRequest {
        pid,
        task_name_port: MachPort::adopt(task_name_port, PortRight::Send),
        reply_port: (reply_port != MACH_PORT_NULL).then(|| MachPort::adopt(reply_port, PortRight::SendOnce)),
    }))
}

/// Answers a bootstrap request with the ports of the requester's transport, which the reply moves.
pub fn send_transport_ports_to_child(
    reply_port: Option<MachPort>,
    receive_right: MachPort,
    send_right: MachPort,
) -> Result<(), KernReturn> {
    const REPLY_SIZE: usize = HEADER_SIZE + BODY_SIZE + 2 * PORT_DESCRIPTOR_SIZE;
    const SEND_TIMEOUT_MS: u32 = 5000;
    const MACH_SEND_INTERRUPTED: KernReturn = 0x1000_0007;

    let reply_port = reply_port.map_or(MACH_PORT_NULL, |port| std::mem::ManuallyDrop::new(port).name());
    let receive_right = std::mem::ManuallyDrop::new(receive_right);
    let send_right = std::mem::ManuallyDrop::new(send_right);
    let mut reply = MessageBuffer::new(REPLY_SIZE);
    {
        let bytes = reply.bytes_mut();
        write_header(
            bytes,
            msgh_bits(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX,
            REPLY_SIZE,
            reply_port,
            IPC_CHANNEL_PORTS_MESSAGE_ID,
        );
        write_u32(bytes, HEADER_SIZE, 2);
        let mut offset = HEADER_SIZE + BODY_SIZE;
        for (name, disposition) in [
            (receive_right.name(), MACH_MSG_TYPE_MOVE_RECEIVE),
            (send_right.name(), MACH_MSG_TYPE_MOVE_SEND),
        ] {
            write_u32(bytes, offset, name);
            write_u32(
                bytes,
                offset + 8,
                (disposition << 16) | (u32::from(MACH_MSG_PORT_DESCRIPTOR) << 24),
            );
            offset += PORT_DESCRIPTOR_SIZE;
        }
    }
    // NB: The reply moves every right it names, and outlives the call.
    let result = unsafe {
        mach_msg(
            reply.header_pointer(),
            MACH_SEND_MSG | MACH_SEND_TIMEOUT,
            REPLY_SIZE as u32,
            0,
            MACH_PORT_NULL,
            SEND_TIMEOUT_MS,
            MACH_PORT_NULL,
        )
    };
    if result == MACH_MSG_SUCCESS {
        return Ok(());
    }
    // NB: These errors leave the reply's rights with this task. The child may have exited before the reply.
    if matches!(
        result,
        MACH_SEND_INVALID_DEST | MACH_SEND_TIMED_OUT | MACH_SEND_INTERRUPTED | MACH_SEND_NO_BUFFER
    ) {
        drop(MessageResources(&mut reply));
    }
    Err(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_buffer_grown_for_a_large_message_shrinks_back() {
        let receive = MachPort::allocate_receive_right().unwrap();
        let destination = receive.make_send_right().unwrap();
        let attachment = MachPort::allocate_receive_right().unwrap();
        let ports = (0..6000)
            .map(|_| (attachment.make_send_right().unwrap(), MACH_MSG_TYPE_MOVE_SEND))
            .collect();
        assert!(matches!(
            send_data_message(&mut MessageBuffer::new(4096), &destination, &[7], ports),
            SendOutcome::Sent
        ));
        let mut buffer = MessageBuffer::new(DEFAULT_RECEIVE_BUFFER_SIZE);
        let (payload, ports) = receive_message(&mut buffer, &receive).unwrap().data.unwrap();
        assert_eq!(payload.bytes(), [7]);
        assert_eq!(ports.len(), 6000);
        assert_eq!(buffer.size(), DEFAULT_RECEIVE_BUFFER_SIZE);
    }

    #[test]
    fn rights_to_dead_ports_are_accepted() {
        let message_size =
            round_message_size(HEADER_SIZE + BODY_SIZE + PORT_DESCRIPTOR_SIZE + INLINE_PAYLOAD_HEADER_SIZE + 1);
        let mut buffer = MessageBuffer::new(message_size + TRAILER_SIZE);
        let bytes = buffer.bytes_mut();
        write_header(
            bytes,
            MACH_MSGH_BITS_COMPLEX,
            message_size,
            MACH_PORT_NULL,
            IPC_INLINE_DATA_MESSAGE_ID,
        );
        let mut offset = HEADER_SIZE;
        write_u32(bytes, offset, 1);
        offset += BODY_SIZE;
        write_u32(bytes, offset, MACH_PORT_DEAD);
        write_u32(
            bytes,
            offset + 8,
            (MACH_MSG_TYPE_MOVE_SEND << 16) | (u32::from(MACH_MSG_PORT_DESCRIPTOR) << 24),
        );
        offset += PORT_DESCRIPTOR_SIZE;
        write_u32(bytes, offset, 1);
        bytes[offset + INLINE_PAYLOAD_HEADER_SIZE] = 7;
        let (payload, ports) = parse_received_message(&mut buffer).unwrap().data.unwrap();
        assert_eq!(payload.bytes(), [7]);
        assert_eq!(ports.len(), 1);
        assert_eq!(ports[0].0.name(), MACH_PORT_DEAD);
        assert_eq!(ports[0].1, MACH_MSG_TYPE_MOVE_SEND);
    }

    #[test]
    fn timed_out_sends_keep_the_mapping_the_kernel_returned() {
        let mut buffer = MessageBuffer::new(4096);
        let destination = MachPort::adopt(MACH_PORT_NULL, PortRight::Send);
        let payload = vec![7; 8192];
        let descriptor = HEADER_SIZE + BODY_SIZE;
        let outcome = send_data_message_with(&mut buffer, &destination, &payload, vec![], |buffer, _| {
            let bytes = buffer.bytes_mut();
            let address = u64::from_ne_bytes(bytes[descriptor..descriptor + 8].try_into().unwrap());
            assert_eq!(address, payload.as_ptr() as u64);
            assert_eq!(bytes[descriptor + 8], 0);
            assert_eq!(bytes[descriptor + 9], MACH_MSG_VIRTUAL_COPY);
            // NB: XNU returns a new mapping with deallocate set for a virtual-copy descriptor.
            bytes[descriptor..descriptor + 8].copy_from_slice(&1u64.to_ne_bytes());
            bytes[descriptor + 8] = 1;
            MACH_SEND_TIMED_OUT
        });
        let SendOutcome::WouldBlock(pending) = outcome else {
            panic!("a full queue keeps the message");
        };
        let PendingMessage(PendingMessageContents::Returned(returned)) = pending else {
            panic!("the kernel handed the message back");
        };
        let outcome = returned.try_send_with(|buffer, _| {
            let bytes = buffer.bytes_mut();
            assert_eq!(
                u64::from_ne_bytes(bytes[descriptor..descriptor + 8].try_into().unwrap()),
                1
            );
            assert_eq!(bytes[descriptor + 8], 1);
            MACH_MSG_SUCCESS
        });
        assert!(matches!(outcome, SendOutcome::Sent));
    }

    #[test]
    fn a_returned_message_that_still_borrows_the_payload_does_not_wait() {
        let mut buffer = MessageBuffer::new(4096);
        let destination = MachPort::adopt(MACH_PORT_NULL, PortRight::Send);
        let payload = vec![7; 8192];
        let outcome = send_data_message_with(&mut buffer, &destination, &payload, vec![], |_, _| MACH_SEND_TIMED_OUT);
        assert!(matches!(outcome, SendOutcome::Failed(MACH_SEND_TIMED_OUT)));
        assert_eq!(payload, vec![7; 8192]);
    }

    #[test]
    fn invalid_dispositions_are_refused_before_sending() {
        use std::io::Read;
        let (mut reader, writer) = std::io::pipe().unwrap();
        let port = file_port_from_fd(OwnedFd::from(writer)).unwrap();
        let destination = MachPort::adopt(MACH_PORT_NULL, PortRight::Send);
        let mut buffer = MessageBuffer::new(4096);
        let outcome = send_data_message_with(
            &mut buffer,
            &destination,
            &[1],
            vec![(port, MACH_MSG_TYPE_MOVE_RECEIVE)],
            |_, _| panic!("invalid dispositions must not be sent"),
        );
        assert!(matches!(outcome, SendOutcome::Failed(MACH_SEND_INVALID_RIGHT)));
        assert_eq!(reader.read(&mut [0]).unwrap(), 0);
    }

    #[test]
    fn failed_sends_release_unconsumed_attachments() {
        use std::io::Read;
        for error in [
            MACH_SEND_INVALID_DEST,
            MACH_SEND_NO_BUFFER,
            MACH_SEND_TOO_LARGE,
            MACH_SEND_INVALID_HEADER,
            MACH_SEND_INVALID_REPLY,
            MACH_SEND_INVALID_VOUCHER,
            MACH_SEND_MSG_TOO_SMALL,
            MACH_SEND_INVALID_DATA,
            MACH_SEND_INVALID_OPTIONS,
            MACH_SEND_INVALID_CONTEXT,
            MACH_SEND_NO_GRANT_DEST,
            MACH_SEND_MSG_FILTERED,
        ] {
            for payload in [vec![1], vec![1; 8192]] {
                let (mut reader, writer) = std::io::pipe().unwrap();
                let port = file_port_from_fd(OwnedFd::from(writer)).unwrap();
                let destination = MachPort::adopt(MACH_PORT_NULL, PortRight::Send);
                let mut buffer = MessageBuffer::new(4096);
                let outcome = send_data_message_with(
                    &mut buffer,
                    &destination,
                    &payload,
                    vec![(port, MACH_MSG_TYPE_MOVE_SEND)],
                    |_, _| error,
                );
                assert!(matches!(outcome, SendOutcome::Failed(failure) if failure == error));
                assert_eq!(reader.read(&mut [0]).unwrap(), 0);
                // NB: Destroying the message must leave the borrowed payload alone.
                assert!(payload.iter().all(|&byte| byte == 1));
            }
        }
    }

    #[test]
    fn body_copyin_errors_do_not_deallocate_consumed_names() {
        for error in [
            MACH_SEND_INVALID_MEMORY,
            MACH_SEND_INVALID_RIGHT,
            MACH_SEND_INVALID_TYPE,
        ] {
            let (reader, writer) = std::io::pipe().unwrap();
            let port = file_port_from_fd(OwnedFd::from(writer)).unwrap();
            // NB: This adds an independently owned reference to the send right.
            assert_eq!(
                unsafe { mach_port_mod_refs(task_self(), port.name(), 0, 1) },
                KERN_SUCCESS
            );
            let retained = MachPort::adopt(port.name(), PortRight::Send);
            let destination = MachPort::adopt(MACH_PORT_NULL, PortRight::Send);
            let outcome = send_data_message_with(
                &mut MessageBuffer::new(4096),
                &destination,
                &[1],
                vec![(port, MACH_MSG_TYPE_MOVE_SEND)],
                |buffer, _| {
                    // NB: Simulate the kernel consuming the preceding descriptor before a body-copyin error.
                    unsafe { mach_msg_destroy(buffer.header_pointer()) };
                    error
                },
            );
            assert!(matches!(outcome, SendOutcome::Failed(failure) if failure == error));
            drop(fd_from_file_port(retained).unwrap());
            drop(reader);
        }
    }

    fn fill_queue(receive: &MachPort, send: &MachPort) {
        loop {
            match send_header_only_message(send, IPC_WAKEUP_MESSAGE_ID, Some(0)) {
                MACH_MSG_SUCCESS => (),
                MACH_SEND_TIMED_OUT => break,
                error => panic!("queue fill failed: {}", error_string(error)),
            }
        }
        assert_eq!(receive.right(), PortRight::Receive);
    }

    #[test]
    fn a_message_left_waiting_gives_back_what_it_carries() {
        use std::io::Read;
        for payload in [vec![1], vec![1; 8192]] {
            let receive = MachPort::allocate_receive_right().unwrap();
            let destination = receive.make_send_right().unwrap();
            fill_queue(&receive, &destination);
            let (mut reader, writer) = std::io::pipe().unwrap();
            let port = file_port_from_fd(OwnedFd::from(writer)).unwrap();
            let attachment_receive = MachPort::allocate_receive_right().unwrap();
            let attachment_send = attachment_receive.make_send_right().unwrap();
            let outcome = send_data_message(
                &mut MessageBuffer::new(4096),
                &destination,
                &payload,
                vec![
                    (port, MACH_MSG_TYPE_MOVE_SEND),
                    (attachment_receive, MACH_MSG_TYPE_MOVE_RECEIVE),
                ],
            );
            let SendOutcome::WouldBlock(pending) = outcome else {
                panic!("a full queue keeps the message");
            };
            drop(pending);
            assert_eq!(reader.read(&mut [0]).unwrap(), 0);
            assert_eq!(
                send_header_only_message(&attachment_send, IPC_WAKEUP_MESSAGE_ID, Some(0)),
                MACH_SEND_INVALID_DEST
            );
        }
    }

    #[test]
    fn a_message_left_waiting_goes_out_whole_once_there_is_room() {
        let receive = MachPort::allocate_receive_right().unwrap();
        let destination = receive.make_send_right().unwrap();
        fill_queue(&receive, &destination);
        let attachment = MachPort::allocate_receive_right().unwrap();
        let payload = vec![7; 8192];
        let outcome = send_data_message(
            &mut MessageBuffer::new(4096),
            &destination,
            &payload,
            vec![(attachment, MACH_MSG_TYPE_MOVE_RECEIVE)],
        );
        let SendOutcome::WouldBlock(pending) = outcome else {
            panic!("a full queue keeps the message");
        };
        let mut scratch = MessageBuffer::new(4096);
        let SendOutcome::WouldBlock(pending) = pending.try_send(&mut scratch, &destination) else {
            panic!("the queue is still full");
        };
        let message = receive_message(&mut MessageBuffer::new(4096), &receive).unwrap();
        assert_eq!(message.id, IPC_WAKEUP_MESSAGE_ID);
        assert!(matches!(
            pending.try_send(&mut scratch, &destination),
            SendOutcome::Sent
        ));
        loop {
            let message = receive_message(&mut MessageBuffer::new(4096), &receive).unwrap();
            if let Some((bytes, ports)) = message.data {
                assert_eq!(bytes.bytes(), payload);
                assert_eq!(ports.len(), 1);
                assert_eq!(ports[0].0.right(), PortRight::Receive);
                break;
            }
        }
    }

    #[test]
    fn rights_are_given_back_when_dropped() {
        let receive = MachPort::allocate_receive_right().unwrap();
        let send = receive.make_send_right().unwrap();
        assert_eq!(send.name(), receive.name());
        drop(send);
        drop(receive);
    }
}
