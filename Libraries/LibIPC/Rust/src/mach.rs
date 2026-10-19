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
pub const MACH_SEND_TIMEOUT: i32 = 0x10;
pub const MACH_RCV_TIMEOUT: i32 = 0x100;

pub const MACH_MSG_SUCCESS: KernReturn = 0;
const MACH_MSG_MASK: KernReturn = 0x0000_3e00;
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

pub const MACH_MSG_TIMEOUT_NONE: u32 = 0;

pub const MACH_NOTIFY_NO_SENDERS: i32 = 0o106;
pub const MACH_NOTIFY_SEND_ONCE: i32 = 0o107;

const MACH_PORT_RIGHT_RECEIVE: u32 = 1;
const MACH_PORT_RIGHT_PORT_SET: u32 = 3;

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
const MAX_RETAINED_RECEIVE_BUFFER_SIZE: usize = 1024 * 1024;

unsafe extern "C" {
    static mach_task_self_: PortName;

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
    fn vm_deallocate(task: PortName, address: usize, size: usize) -> KernReturn;
    fn mach_error_string(error: KernReturn) -> *const c_char;
    fn fileport_makeport(fd: c_int, port: *mut PortName) -> c_int;
    fn fileport_makefd(port: PortName) -> c_int;
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
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

pub const IPC_DATA_MESSAGE_ID: i32 = 0x4950_C001;
pub const IPC_INLINE_DATA_MESSAGE_ID: i32 = 0x4950_C002;
pub const IPC_WAKEUP_MESSAGE_ID: i32 = 0x4950_C003;
pub const IPC_RECEIVE_BARRIER_MESSAGE_ID: i32 = 0x4950_C004;

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

    fn release_excess_capacity(&mut self) {
        if self.words.capacity() * 8 > MAX_RETAINED_RECEIVE_BUFFER_SIZE {
            *self = Self::new(MAX_RETAINED_RECEIVE_BUFFER_SIZE);
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

/// Sends LibIPC's data message through a send right this task keeps, moving the rights of the attachments with it.
///
/// A payload that fits goes inline after a 32-bit size; a larger one goes out of line, as `TransportMachPort` sends it.
pub fn send_data_message(
    buffer: &mut MessageBuffer,
    destination: &MachPort,
    payload: &[u8],
    ports: Vec<(MachPort, u32)>,
    should_retry: impl FnMut() -> bool,
) -> KernReturn {
    send_data_message_with(buffer, destination, payload, ports, should_retry, |buffer, size| {
        // NB: The buffer owns its moved rights and borrows the original payload until a timeout returns a mapping.
        unsafe {
            mach_msg(
                buffer.header_pointer(),
                MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                size as u32,
                0,
                MACH_PORT_NULL,
                100,
                MACH_PORT_NULL,
            )
        }
    })
}

fn send_data_message_with(
    buffer: &mut MessageBuffer,
    destination: &MachPort,
    payload: &[u8],
    mut ports: Vec<(MachPort, u32)>,
    mut should_retry: impl FnMut() -> bool,
    mut send: impl FnMut(&mut MessageBuffer, usize) -> KernReturn,
) -> KernReturn {
    if payload.len() > crate::MAX_MESSAGE_PAYLOAD_SIZE || ports.len() > crate::MAX_MESSAGE_ATTACHMENT_COUNT {
        return MACH_SEND_TOO_LARGE;
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
        return MACH_SEND_INVALID_RIGHT;
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

    // NB: The message now owns moved rights. Header errors leave them untouched; body-copyin errors can destroy
    //     a prefix without reporting its length, so those original names must never be deallocated here.
    for (port, disposition) in &mut ports {
        if matches!(
            *disposition,
            MACH_MSG_TYPE_MOVE_RECEIVE | MACH_MSG_TYPE_MOVE_SEND | MACH_MSG_TYPE_MOVE_SEND_ONCE
        ) {
            port.name = MACH_PORT_NULL;
        }
    }
    let resources = MessageResources(buffer);
    loop {
        let result = send(resources.0, message_size);
        if result & !MACH_MSG_MASK == MACH_SEND_TIMED_OUT {
            // NB: A timeout pseudo-receives the message, possibly with new names and mappings. Retry or destroy
            //     that returned message, rather than the original port owners or payload address.
            if result == MACH_SEND_TIMED_OUT && should_retry() {
                continue;
            }
            return result;
        }
        if !send_error_leaves_descriptors_untouched(result) {
            std::mem::forget(resources);
        }
        return result;
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

/// The bytes of a received LibIPC message, copied from the message or mapped out of line.
pub enum PayloadBytes {
    Inline(Vec<u8>),
    OutOfLine(VmRegion),
}

impl AsRef<[u8]> for PayloadBytes {
    fn as_ref(&self) -> &[u8] {
        self.bytes()
    }
}

impl PayloadBytes {
    pub fn bytes(&self) -> &[u8] {
        match self {
            Self::Inline(bytes) => bytes,
            Self::OutOfLine(region) => region.bytes(),
        }
    }
}

pub struct ReceivedMessage {
    pub id: i32,
    pub local_port: PortName,
    /// For a LibIPC data message, its bytes and the rights it moved, with their dispositions.
    pub data: Option<(PayloadBytes, Vec<(MachPort, u32)>)>,
}

#[derive(Debug)]
pub enum ReceiveError {
    Kernel(KernReturn),
    Malformed(&'static str),
}

/// Receives the next message from a port or port set, growing the buffer for a large one.
pub fn receive_message(buffer: &mut MessageBuffer, from: &MachPort) -> Result<ReceivedMessage, ReceiveError> {
    receive(buffer, from, 0)
}

/// Receives the next message if one is already queued, without waiting for one.
pub fn try_receive_message(
    buffer: &mut MessageBuffer,
    from: &MachPort,
) -> Result<Option<ReceivedMessage>, ReceiveError> {
    match receive(buffer, from, MACH_RCV_TIMEOUT) {
        Err(ReceiveError::Kernel(MACH_RCV_TIMED_OUT)) => Ok(None),
        result => result.map(Some),
    }
}

fn receive(buffer: &mut MessageBuffer, from: &MachPort, options: i32) -> Result<ReceivedMessage, ReceiveError> {
    let mut result = receive_into(buffer, from, MACH_RCV_LARGE | options);
    if result == MACH_RCV_TOO_LARGE {
        // NB: The message stays queued; its header says how large a buffer it needs.
        let needed = read_u32(buffer.bytes(), 4).unwrap_or(0) as usize + TRAILER_SIZE;
        buffer.ensure_size(needed);
        result = receive_into(buffer, from, options);
    }
    let message = if result != MACH_MSG_SUCCESS {
        Err(ReceiveError::Kernel(result))
    } else {
        parse_received_message(buffer)
    };
    buffer.release_excess_capacity();
    message
}

fn receive_into(buffer: &mut MessageBuffer, from: &MachPort, options: i32) -> KernReturn {
    let size = u32::try_from(buffer.words.len() * 8).unwrap_or(u32::MAX);
    // NB: The buffer is aligned for a header, and the call writes at most size bytes into it.
    unsafe {
        mach_msg(
            buffer.header_pointer(),
            MACH_RCV_MSG | options,
            0,
            size,
            from.name(),
            MACH_MSG_TIMEOUT_NONE,
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
    let malformed = ReceiveError::Malformed;
    let bits = read_u32(bytes, 0).ok_or(malformed("header"))?;
    let size = read_u32(bytes, 4).ok_or(malformed("header"))? as usize;
    let local_port = read_u32(bytes, 12).ok_or(malformed("header"))?;
    let id = read_u32(bytes, 20).ok_or(malformed("header"))? as i32;
    if size > bytes.len() || size < HEADER_SIZE {
        return Err(malformed("size"));
    }
    let bytes = &mut bytes[..size];
    if id != IPC_DATA_MESSAGE_ID && id != IPC_INLINE_DATA_MESSAGE_ID {
        return Ok(ReceivedMessage {
            id,
            local_port,
            data: None,
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn large_receives_reuse_the_grown_buffer() {
        let receive = MachPort::allocate_receive_right().unwrap();
        let destination = receive.make_send_right().unwrap();
        let attachment = MachPort::allocate_receive_right().unwrap();
        let ports = (0..6000)
            .map(|_| (attachment.make_send_right().unwrap(), MACH_MSG_TYPE_MOVE_SEND))
            .collect();
        assert_eq!(
            send_data_message(&mut MessageBuffer::new(4096), &destination, &[7], ports, || true),
            MACH_MSG_SUCCESS
        );
        let mut buffer = MessageBuffer::new(DEFAULT_RECEIVE_BUFFER_SIZE);
        let (payload, ports) = receive_message(&mut buffer, &receive).unwrap().data.unwrap();
        assert_eq!(payload.bytes(), [7]);
        assert_eq!(ports.len(), 6000);
        let buffer_address = buffer.words.as_ptr();
        let buffer_size = buffer.words.len() * 8;
        assert!(buffer_size > DEFAULT_RECEIVE_BUFFER_SIZE);
        assert!(buffer.words.capacity() * 8 <= MAX_RETAINED_RECEIVE_BUFFER_SIZE);
        drop(ports);
        let ports = (0..6000)
            .map(|_| (attachment.make_send_right().unwrap(), MACH_MSG_TYPE_MOVE_SEND))
            .collect();
        assert_eq!(
            send_data_message(&mut MessageBuffer::new(4096), &destination, &[8], ports, || true),
            MACH_MSG_SUCCESS
        );
        let (payload, ports) = receive_message(&mut buffer, &receive).unwrap().data.unwrap();
        assert_eq!(payload.bytes(), [8]);
        assert_eq!(ports.len(), 6000);
        assert_eq!(buffer.words.len() * 8, buffer_size);
        assert_eq!(buffer.words.as_ptr(), buffer_address);
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
    fn receive_buffer_retention_is_capped() {
        let mut buffer = MessageBuffer::new(DEFAULT_RECEIVE_BUFFER_SIZE);
        buffer.ensure_size(2 * DEFAULT_RECEIVE_BUFFER_SIZE);
        let address = buffer.words.as_ptr();
        buffer.release_excess_capacity();
        assert_eq!(buffer.words.as_ptr(), address);
        buffer.ensure_size(MAX_RETAINED_RECEIVE_BUFFER_SIZE + 8);
        buffer.release_excess_capacity();
        assert_eq!(buffer.words.len() * 8, MAX_RETAINED_RECEIVE_BUFFER_SIZE);
        assert_eq!(buffer.words.capacity() * 8, MAX_RETAINED_RECEIVE_BUFFER_SIZE);
    }

    #[test]
    fn timed_out_sends_preserve_returned_memory_ownership() {
        let mut buffer = MessageBuffer::new(4096);
        let destination = MachPort::adopt(MACH_PORT_NULL, PortRight::Send);
        let payload = vec![7; 8192];
        let descriptor = HEADER_SIZE + BODY_SIZE;
        let mut attempt = 0;
        assert_eq!(
            send_data_message_with(
                &mut buffer,
                &destination,
                &payload,
                vec![],
                || true,
                |buffer, _| {
                    let bytes = buffer.bytes_mut();
                    let address = u64::from_ne_bytes(bytes[descriptor..descriptor + 8].try_into().unwrap());
                    if attempt == 0 {
                        assert_eq!(address, payload.as_ptr() as u64);
                        assert_eq!(bytes[descriptor + 8], 0);
                    } else {
                        assert_eq!(address, attempt);
                        assert_eq!(bytes[descriptor + 8], 1);
                    }
                    assert_eq!(bytes[descriptor + 9], MACH_MSG_VIRTUAL_COPY);
                    attempt += 1;
                    if attempt == 3 {
                        return MACH_MSG_SUCCESS;
                    }
                    // NB: XNU returns a new mapping with deallocate set for a virtual-copy descriptor.
                    bytes[descriptor..descriptor + 8].copy_from_slice(&attempt.to_ne_bytes());
                    bytes[descriptor + 8] = 1;
                    MACH_SEND_TIMED_OUT
                }
            ),
            MACH_MSG_SUCCESS
        );
        assert_eq!(attempt, 3);
    }

    #[test]
    fn too_many_attachments_are_refused_before_sending() {
        let mut buffer = MessageBuffer::new(4096);
        let destination = MachPort::allocate_receive_right().unwrap();
        let ports = (0..crate::MAX_MESSAGE_ATTACHMENT_COUNT + 1)
            .map(|_| {
                (
                    MachPort::adopt(MACH_PORT_NULL, PortRight::Send),
                    MACH_MSG_TYPE_COPY_SEND,
                )
            })
            .collect();
        assert_eq!(
            send_data_message(&mut buffer, &destination, &[1], ports, || false),
            MACH_SEND_TOO_LARGE
        );
    }

    #[test]
    fn invalid_dispositions_are_refused_before_sending() {
        use std::io::Read;
        let (mut reader, writer) = std::io::pipe().unwrap();
        let port = file_port_from_fd(OwnedFd::from(writer)).unwrap();
        let destination = MachPort::adopt(MACH_PORT_NULL, PortRight::Send);
        let mut buffer = MessageBuffer::new(4096);
        assert_eq!(
            send_data_message_with(
                &mut buffer,
                &destination,
                &[1],
                vec![(port, MACH_MSG_TYPE_MOVE_RECEIVE)],
                || false,
                |_, _| panic!("invalid dispositions must not be sent")
            ),
            MACH_SEND_INVALID_RIGHT
        );
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
            let (mut reader, writer) = std::io::pipe().unwrap();
            let port = file_port_from_fd(OwnedFd::from(writer)).unwrap();
            let destination = MachPort::adopt(MACH_PORT_NULL, PortRight::Send);
            let mut buffer = MessageBuffer::new(4096);
            assert_eq!(
                send_data_message_with(
                    &mut buffer,
                    &destination,
                    &[1],
                    vec![(port, MACH_MSG_TYPE_MOVE_SEND)],
                    || false,
                    |_, _| error
                ),
                error
            );
            assert_eq!(reader.read(&mut [0]).unwrap(), 0);
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
            assert_eq!(
                send_data_message_with(
                    &mut MessageBuffer::new(4096),
                    &destination,
                    &[1],
                    vec![(port, MACH_MSG_TYPE_MOVE_SEND)],
                    || false,
                    |buffer, _| {
                        // NB: Simulate the kernel consuming the preceding descriptor before a body-copyin error.
                        unsafe { mach_msg_destroy(buffer.header_pointer()) };
                        error
                    }
                ),
                error
            );
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
    fn timed_out_sends_destroy_the_returned_message() {
        use std::io::Read;
        for payload in [vec![1], vec![1; 8192]] {
            let receive = MachPort::allocate_receive_right().unwrap();
            let destination = receive.make_send_right().unwrap();
            fill_queue(&receive, &destination);
            let (mut reader, writer) = std::io::pipe().unwrap();
            let port = file_port_from_fd(OwnedFd::from(writer)).unwrap();
            let attachment_receive = MachPort::allocate_receive_right().unwrap();
            let attachment_send = attachment_receive.make_send_right().unwrap();
            assert_eq!(
                send_data_message(
                    &mut MessageBuffer::new(4096),
                    &destination,
                    &payload,
                    vec![
                        (port, MACH_MSG_TYPE_MOVE_SEND),
                        (attachment_receive, MACH_MSG_TYPE_MOVE_RECEIVE)
                    ],
                    || false
                ),
                MACH_SEND_TIMED_OUT
            );
            assert_eq!(reader.read(&mut [0]).unwrap(), 0);
            assert_eq!(
                send_header_only_message(&attachment_send, IPC_WAKEUP_MESSAGE_ID, Some(0)),
                MACH_SEND_INVALID_DEST
            );
        }
    }

    #[test]
    fn timed_out_sends_retry_the_returned_rights_and_memory() {
        let receive = MachPort::allocate_receive_right().unwrap();
        let destination = receive.make_send_right().unwrap();
        fill_queue(&receive, &destination);
        let attachment = MachPort::allocate_receive_right().unwrap();
        let payload = vec![7; 8192];
        let mut retries = 0;
        assert_eq!(
            send_data_message(
                &mut MessageBuffer::new(4096),
                &destination,
                &payload,
                vec![(attachment, MACH_MSG_TYPE_MOVE_RECEIVE)],
                || {
                    retries += 1;
                    assert_eq!(retries, 1);
                    let message = receive_message(&mut MessageBuffer::new(4096), &receive).unwrap();
                    assert_eq!(message.id, IPC_WAKEUP_MESSAGE_ID);
                    true
                }
            ),
            MACH_MSG_SUCCESS
        );
        assert_eq!(retries, 1);
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
