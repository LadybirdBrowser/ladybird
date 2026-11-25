/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Sending and receiving bytes with file descriptors over a Unix domain socket.
//!
//! This is the crate's other island of unsafe code: `sendmsg()` and `recvmsg()` with `SCM_RIGHTS` control messages, and
//! the socket options the transport sets. None of these calls blocks, whatever mode the socket is in, as the IO thread
//! that makes them serves every transport of the process.

#![allow(unsafe_code)]

use crate::receive_queue::PayloadBytes;
use crate::shared_memory::create_anonymous_file;

use std::io;
use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, OwnedFd, RawFd};

/// The most descriptors one `sendmsg()` carries, as `Core::LocalSocket::MAX_TRANSFER_FDS`.
pub const MAX_TRANSFER_FDS: usize = 128;

fn control_buffer_size(fd_count: usize) -> usize {
    // NB: CMSG_SPACE() only computes a size.
    unsafe { libc::CMSG_SPACE((fd_count * size_of::<RawFd>()) as u32) as usize }
}

// NB: Room for the control message of MAX_TRANSFER_FDS descriptors, which a test checks.
const SEND_CONTROL_BUFFER_WORDS: usize = 80;

/// Sends some of the bytes, with all of the descriptors, which go with the first byte, without waiting for room. Returns
/// how many bytes went.
pub fn send_with_fds(socket: RawFd, bytes: &[io::IoSlice<'_>], fds: &[BorrowedFd<'_>]) -> io::Result<usize> {
    assert!(fds.len() <= MAX_TRANSFER_FDS);
    // NB: IoSlice has the same layout as iovec on Unix.
    let mut control = [0u64; SEND_CONTROL_BUFFER_WORDS];
    // NB: An all-zero msghdr is a valid empty one.
    let mut message: libc::msghdr = unsafe { std::mem::zeroed() };
    message.msg_iov = bytes.as_ptr().cast_mut().cast();
    message.msg_iovlen = bytes.len() as _;
    if !fds.is_empty() {
        message.msg_control = control.as_mut_ptr().cast();
        message.msg_controllen = control_buffer_size(fds.len()) as _;
        // NB: The control buffer has room for one control message with the descriptors, which CMSG_FIRSTHDR() and
        //     CMSG_DATA() point into.
        unsafe {
            let header = libc::CMSG_FIRSTHDR(&raw const message);
            (*header).cmsg_level = libc::SOL_SOCKET;
            (*header).cmsg_type = libc::SCM_RIGHTS;
            (*header).cmsg_len = libc::CMSG_LEN((fds.len() * size_of::<RawFd>()) as u32) as _;
            let data = libc::CMSG_DATA(header).cast::<RawFd>();
            for (index, fd) in fds.iter().enumerate() {
                data.add(index).write_unaligned(fd.as_raw_fd());
            }
        }
    }
    // NB: The message points at the bytes and the control buffer, which outlive the call.
    let sent = unsafe { libc::sendmsg(socket, &raw const message, libc::MSG_NOSIGNAL | libc::MSG_DONTWAIT) };
    if sent < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(sent as usize)
}

pub(crate) struct Receiver {
    control: Vec<u64>,
}

impl Receiver {
    pub(crate) fn new() -> Self {
        Self {
            control: vec![0; control_buffer_size(MAX_TRANSFER_FDS).div_ceil(8)],
        }
    }

    /// Receives bytes and any descriptors that came with them, without waiting for them. Zero bytes mean the peer
    /// closed its end.
    pub(crate) fn receive_with_fds(
        &mut self,
        socket: RawFd,
        buffer: &mut [u8],
        fds: &mut Vec<OwnedFd>,
    ) -> io::Result<usize> {
        self.receive_with_fd_capacity(socket, buffer, fds, MAX_TRANSFER_FDS)
    }

    pub(crate) fn receive_with_fd_capacity(
        &mut self,
        socket: RawFd,
        buffer: &mut [u8],
        fds: &mut Vec<OwnedFd>,
        fd_capacity: usize,
    ) -> io::Result<usize> {
        let mut iov = libc::iovec {
            iov_base: buffer.as_mut_ptr().cast(),
            iov_len: buffer.len(),
        };
        assert!(fd_capacity <= MAX_TRANSFER_FDS);
        let control_size = control_buffer_size(fd_capacity);
        // NB: An all-zero msghdr is a valid empty one.
        let mut message: libc::msghdr = unsafe { std::mem::zeroed() };
        message.msg_iov = &raw mut iov;
        message.msg_iovlen = 1;
        message.msg_control = self.control.as_mut_ptr().cast();
        message.msg_controllen = control_size as _;
        #[cfg(any(
            target_os = "linux",
            target_os = "android",
            target_os = "freebsd",
            target_os = "dragonfly",
            target_os = "netbsd",
            target_os = "openbsd",
            target_os = "illumos",
            target_os = "hurd",
            target_os = "cygwin",
        ))]
        let flags = libc::MSG_CMSG_CLOEXEC;
        #[cfg(not(any(
            target_os = "linux",
            target_os = "android",
            target_os = "freebsd",
            target_os = "dragonfly",
            target_os = "netbsd",
            target_os = "openbsd",
            target_os = "illumos",
            target_os = "hurd",
            target_os = "cygwin",
        )))]
        let flags = 0;
        // NB: The message points at the buffer and the control buffer, which outlive the call.
        let received = unsafe { libc::recvmsg(socket, &raw mut message, flags | libc::MSG_DONTWAIT) };
        if received < 0 {
            return Err(io::Error::last_os_error());
        }
        message.msg_controllen = (message.msg_controllen as usize).min(control_size) as _;
        let first_fd = fds.len();
        let mut malformed_control = false;
        // NB: recvmsg() filled in the control messages it received, which CMSG_FIRSTHDR() and CMSG_NXTHDR() walk, and
        //     every descriptor in an SCM_RIGHTS message is a new one that this process owns.
        unsafe {
            let control_end = message.msg_control as usize + message.msg_controllen as usize;
            let header_size = libc::CMSG_LEN(0) as usize;
            let mut header = libc::CMSG_FIRSTHDR(&raw const message);
            while !header.is_null() {
                let remaining = control_end - header as usize;
                let length = (*header).cmsg_len as usize;
                let bounded_length = length.min(remaining);
                if bounded_length < header_size {
                    malformed_control = true;
                    break;
                }
                if (*header).cmsg_level == libc::SOL_SOCKET && (*header).cmsg_type == libc::SCM_RIGHTS {
                    let data = libc::CMSG_DATA(header).cast::<RawFd>();
                    let data_size = bounded_length - header_size;
                    for index in 0..data_size / size_of::<RawFd>() {
                        fds.push(OwnedFd::from_raw_fd(data.add(index).read_unaligned()));
                    }
                }
                if length > remaining {
                    malformed_control = true;
                    break;
                }
                header = libc::CMSG_NXTHDR(&raw const message, header);
            }
        }
        if flags == 0 {
            for fd in &fds[first_fd..] {
                // NB: The descriptor is owned and F_SETFD only changes its descriptor flags.
                if unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_SETFD, libc::FD_CLOEXEC) } < 0 {
                    return Err(io::Error::last_os_error());
                }
            }
        }
        if malformed_control || message.msg_flags & libc::MSG_CTRUNC != 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "truncated socket control message",
            ));
        }
        Ok(received as usize)
    }
}

/// How many bytes the socket has for the next read, as far as the kernel can tell.
pub(crate) fn readable_byte_count(socket: RawFd) -> usize {
    let mut count: libc::c_int = 0;
    // NB: FIONREAD writes the count to the local it points to.
    if unsafe { libc::ioctl(socket, libc::FIONREAD, &raw mut count) } < 0 {
        return 0;
    }
    usize::try_from(count).unwrap_or(0)
}

/// Asks for the buffer sizes LibIPC's sockets have always used, which the kernel may round or cap. A socket that keeps
/// its defaults still works.
pub(crate) fn set_buffer_sizes(socket: RawFd, size: usize) {
    let size = libc::c_int::try_from(size).unwrap_or(libc::c_int::MAX);
    for option in [libc::SO_SNDBUF, libc::SO_RCVBUF] {
        // NB: The option's value is the local the call reads.
        unsafe {
            libc::setsockopt(
                socket,
                libc::SOL_SOCKET,
                option,
                (&raw const size).cast(),
                size_of::<libc::c_int>() as libc::socklen_t,
            );
        }
    }
}

/// Shuts down one or both directions of the socket, which the peer sees whichever process has it.
pub(crate) fn shutdown(socket: RawFd, how: std::net::Shutdown) {
    let how = match how {
        std::net::Shutdown::Read => libc::SHUT_RD,
        std::net::Shutdown::Write => libc::SHUT_WR,
        std::net::Shutdown::Both => libc::SHUT_RDWR,
    };
    // NB: shutdown() only changes the state of the socket. One that is gone already has nothing left to shut down.
    unsafe { libc::shutdown(socket, how) };
}

/// Copies a payload into an anonymous file, whose descriptor goes last in the frame, and seals the file where the
/// receiver maps it.
pub(crate) fn copy_payload_to_file(bytes: &[u8]) -> io::Result<std::fs::File> {
    use std::io::Write;
    let mut file = std::fs::File::from(create_anonymous_file()?);
    // NB: A shared memory object of the BSDs grows by truncation only, not by writing past its end.
    file.set_len(bytes.len() as u64)?;
    file.write_all(bytes)?;
    #[cfg(target_os = "linux")]
    crate::shared_memory::seal(&file)?;
    Ok(file)
}

/// The payload of a message in the file it came in, which on Linux the sender has sealed.
pub(crate) fn payload_from_file(fd: OwnedFd, size: usize) -> io::Result<PayloadBytes> {
    #[cfg(target_os = "linux")]
    return Ok(PayloadBytes::Mapped(crate::shared_memory::MappedPayload::map(
        fd, size,
    )?));
    #[cfg(not(target_os = "linux"))]
    return Ok(PayloadBytes::Inline(copy_payload_from_file(fd, size)?));
}

/// Copies bytes before decoding, using pread so a size change reports a short read. The sender can still write to an
/// unsealed file, so the copy is what gets decoded.
#[cfg(not(target_os = "linux"))]
fn copy_payload_from_file(fd: OwnedFd, size: usize) -> io::Result<Vec<u8>> {
    use std::os::unix::fs::FileExt;
    let file = std::fs::File::from(fd);
    if file.metadata()?.len() < size as u64 {
        return Err(io::Error::from(io::ErrorKind::UnexpectedEof));
    }
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(size)
        .map_err(|_| io::Error::from(io::ErrorKind::OutOfMemory))?;
    bytes.resize(size, 0);
    file.read_exact_at(&mut bytes, 0)?;
    Ok(bytes)
}

/// Raises this process's soft limit on open file descriptors, as LibMain does for every process, so that a test can
/// hold more descriptors than the common default of 1024.
#[cfg(test)]
pub(crate) fn raise_descriptor_limit() {
    let mut limit = libc::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    // NB: limit is a local that getrlimit() writes and setrlimit() reads.
    unsafe {
        assert_eq!(libc::getrlimit(libc::RLIMIT_NOFILE, &raw mut limit), 0);
        limit.rlim_cur = limit.rlim_max.min(65536);
        assert_eq!(libc::setrlimit(libc::RLIMIT_NOFILE, &raw const limit), 0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;
    use std::os::unix::net::UnixStream;

    #[cfg(target_os = "linux")]
    #[test]
    fn only_sealed_payload_files_are_mapped() {
        let file = copy_payload_to_file(b"payload").unwrap();
        // NB: F_GET_SEALS only reads the seals of the owned file.
        let seals = unsafe { libc::fcntl(file.as_raw_fd(), libc::F_GET_SEALS) };
        assert_ne!(seals & libc::F_SEAL_WRITE, 0);
        assert_eq!(payload_from_file(file.into(), 7).unwrap().bytes(), b"payload");

        // NB: memfd_create() returns a new descriptor for a NUL-terminated name.
        let fd = unsafe { libc::memfd_create(c"".as_ptr(), libc::MFD_CLOEXEC) };
        assert!(fd >= 0);
        // NB: The descriptor is new and nothing else owns it.
        let mut unsealable = std::fs::File::from(unsafe { OwnedFd::from_raw_fd(fd) });
        std::io::Write::write_all(&mut unsealable, b"payload").unwrap();
        let unsealed = crate::shared_memory::create_unsealed_file_holding(b"payload");
        for file in [unsealable, unsealed] {
            assert_eq!(
                payload_from_file(file.into(), 7).err().unwrap().kind(),
                io::ErrorKind::InvalidData
            );
        }

        let sparse = std::fs::File::from(crate::shared_memory::create_anonymous_file().unwrap());
        sparse.set_len(7).unwrap();
        crate::shared_memory::seal(&sparse).unwrap();
        assert_eq!(
            payload_from_file(sparse.into(), 7).err().unwrap().kind(),
            io::ErrorKind::InvalidData
        );

        for bytes_of_another_size in [&b"pay"[..], b"payload!"] {
            let file = copy_payload_to_file(bytes_of_another_size).unwrap();
            assert_eq!(
                payload_from_file(file.into(), 7).err().unwrap().kind(),
                io::ErrorKind::InvalidData
            );
        }
    }

    #[test]
    fn the_send_control_buffer_has_room_for_every_descriptor() {
        assert!(control_buffer_size(MAX_TRANSFER_FDS) <= SEND_CONTROL_BUFFER_WORDS * size_of::<u64>());
    }

    #[test]
    fn transports_leave_the_mode_of_their_socket_alone() {
        use crate::transport_socket::TransportSocket;

        for nonblocking in [false, true] {
            let (a, b) = std::os::unix::net::UnixStream::pair().unwrap();
            a.set_nonblocking(nonblocking).unwrap();
            b.set_nonblocking(nonblocking).unwrap();
            let descriptor = a.as_raw_fd();
            let a = TransportSocket::new(a.into()).unwrap();
            let b = TransportSocket::new(b.into()).unwrap();
            // NB: The transport owns the socket, and F_GETFL only reads its status flags.
            let flags = unsafe { libc::fcntl(descriptor, libc::F_GETFL) };
            assert!(flags >= 0);
            assert_eq!(flags & libc::O_NONBLOCK != 0, nonblocking);
            a.post_message(vec![1], Vec::new()).unwrap();
            b.post_message(vec![2], Vec::new()).unwrap();
            assert_eq!(a.receive().unwrap().bytes(), [2]);
            assert_eq!(b.receive().unwrap().bytes(), [1]);
        }
    }

    #[test]
    fn received_descriptors_are_close_on_exec() {
        use std::os::fd::AsFd;

        let (sender, receiver) = UnixStream::pair().unwrap();
        let file = std::fs::File::open("/dev/null").unwrap();
        send_with_fds(sender.as_raw_fd(), &[io::IoSlice::new(&[1])], &[file.as_fd()]).unwrap();
        let mut fds = Vec::new();
        Receiver::new()
            .receive_with_fds(receiver.as_raw_fd(), &mut [0], &mut fds)
            .unwrap();
        assert_eq!(fds.len(), 1);
        // NB: The descriptor is owned and F_GETFD only reads its descriptor flags.
        let flags = unsafe { libc::fcntl(fds[0].as_raw_fd(), libc::F_GETFD) };
        assert!(flags >= 0);
        assert_ne!(flags & libc::FD_CLOEXEC, 0);
    }

    #[test]
    fn vectored_sends_keep_the_payload_and_descriptors_together() {
        use std::os::fd::AsFd;
        let (sender, receiver) = UnixStream::pair().unwrap();
        let (mut reader, writer) = std::io::pipe().unwrap();
        assert_eq!(
            send_with_fds(
                sender.as_raw_fd(),
                &[io::IoSlice::new(b"head"), io::IoSlice::new(b"payload")],
                &[writer.as_fd()]
            )
            .unwrap(),
            11
        );
        drop(writer);
        let mut bytes = [0; 11];
        let mut fds = Vec::new();
        assert_eq!(
            Receiver::new()
                .receive_with_fds(receiver.as_raw_fd(), &mut bytes, &mut fds)
                .unwrap(),
            11
        );
        assert_eq!(&bytes, b"headpayload");
        assert_eq!(fds.len(), 1);
        drop(fds);
        assert_eq!(reader.read(&mut [0]).unwrap(), 0);
    }

    #[test]
    fn sending_after_the_peer_closes_returns_an_error() {
        let (sender, receiver) = UnixStream::pair().unwrap();
        drop(receiver);
        assert_eq!(
            send_with_fds(sender.as_raw_fd(), &[io::IoSlice::new(&[1])], &[])
                .unwrap_err()
                .kind(),
            io::ErrorKind::BrokenPipe
        );
    }

    #[test]
    fn received_descriptors_do_not_carry_over_between_reads() {
        use std::os::fd::AsFd;

        let (sender, socket) = UnixStream::pair().unwrap();
        let (mut reader, writer) = std::io::pipe().unwrap();
        let mut receiver = Receiver::new();
        let mut fds = Vec::new();
        send_with_fds(sender.as_raw_fd(), &[io::IoSlice::new(&[1])], &[writer.as_fd()]).unwrap();
        receiver
            .receive_with_fds(socket.as_raw_fd(), &mut [0], &mut fds)
            .unwrap();
        assert_eq!(fds.len(), 1);
        fds.clear();
        drop(writer);
        assert_eq!(reader.read(&mut [0]).unwrap(), 0);
        send_with_fds(sender.as_raw_fd(), &[io::IoSlice::new(&[2])], &[]).unwrap();
        let mut byte = [0];
        assert_eq!(
            receiver
                .receive_with_fds(socket.as_raw_fd(), &mut byte, &mut fds)
                .unwrap(),
            1
        );
        assert_eq!(byte, [2]);
        assert!(fds.is_empty());
    }

    #[test]
    fn truncated_control_messages_adopt_the_delivered_descriptors() {
        use std::os::fd::AsFd;

        let (sender, receiver) = UnixStream::pair().unwrap();
        let (mut reader, writer) = std::io::pipe().unwrap();
        send_with_fds(sender.as_raw_fd(), &[io::IoSlice::new(&[1])], &[writer.as_fd(); 3]).unwrap();
        drop(writer);
        let mut fds = Vec::new();
        let error = Receiver::new()
            .receive_with_fd_capacity(receiver.as_raw_fd(), &mut [0; 1], &mut fds, 1)
            .unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidData);
        assert!(!fds.is_empty());
        assert!(fds.len() < 3);
        drop(fds);
        assert_eq!(reader.read(&mut [0]).unwrap(), 0);
    }
}
