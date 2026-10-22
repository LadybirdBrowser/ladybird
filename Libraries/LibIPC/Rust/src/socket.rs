/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Sending and receiving bytes with file descriptors over a Unix domain socket.
//!
//! This is the crate's other island of unsafe code: `sendmsg()` and `recvmsg()` with `SCM_RIGHTS` control messages.

#![allow(unsafe_code)]

use crate::shared_memory::create_anonymous_file;

use std::io;
use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, Ordering};

/// The most descriptors one `sendmsg()` carries, as `Core::LocalSocket::MAX_TRANSFER_FDS`.
pub const MAX_TRANSFER_FDS: usize = 128;

fn control_buffer_size(fd_count: usize) -> usize {
    // NB: CMSG_SPACE() only computes a size.
    unsafe { libc::CMSG_SPACE((fd_count * size_of::<RawFd>()) as u32) as usize }
}

/// What the threads of a transport wait for besides their socket: a pipe that `signal()` makes readable for good. It
/// ends their waits as `shutdown()` on the socket would, which the sandboxes of helper processes do not allow.
pub(crate) struct Stop {
    reader: io::PipeReader,
    writer: io::PipeWriter,
    signaled: AtomicBool,
}

impl Stop {
    pub(crate) fn new() -> io::Result<Self> {
        let (reader, writer) = io::pipe()?;
        Ok(Self {
            reader,
            writer,
            signaled: AtomicBool::new(false),
        })
    }

    pub(crate) fn signal(&self) {
        if !self.signaled.swap(true, Ordering::AcqRel) {
            // NB: One byte makes the pipe readable for good, since nothing reads it. The pipe is empty, so this
            //     cannot block.
            let _ = io::Write::write_all(&mut &self.writer, &[0]);
        }
    }

    #[cfg(test)]
    pub(crate) fn is_signaled(&self) -> bool {
        self.signaled.load(Ordering::Acquire)
    }
}

/// Waits until the socket is ready for `events` (`POLLIN` or `POLLOUT`), or has hung up or failed, which the read or
/// write that follows reports, and answers true; or until `stop` is signaled, and answers false.
pub(crate) fn wait_for(socket: &UnixStream, events: libc::c_short, stop: Option<&Stop>) -> io::Result<bool> {
    let mut fds = [
        libc::pollfd {
            fd: socket.as_raw_fd(),
            events,
            revents: 0,
        },
        libc::pollfd {
            fd: stop.map_or(-1, |stop| stop.reader.as_raw_fd()),
            events: libc::POLLIN,
            revents: 0,
        },
    ];
    loop {
        // NB: poll() writes the events of the two descriptors in the array, and ignores a negative one.
        if unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, -1) } < 0 {
            let error = io::Error::last_os_error();
            if error.kind() == io::ErrorKind::Interrupted {
                continue;
            }
            return Err(error);
        }
        if fds[1].revents != 0 {
            return Ok(false);
        }
        if fds[0].revents != 0 {
            return Ok(true);
        }
    }
}

/// Sends some of the bytes, with all of the descriptors, which go with the first byte, without waiting for room.
/// Returns how many bytes went, or `WouldBlock` where none could go yet.
pub fn send_with_fds(socket: &UnixStream, bytes: &[io::IoSlice<'_>], fds: &[BorrowedFd<'_>]) -> io::Result<usize> {
    assert!(fds.len() <= MAX_TRANSFER_FDS);
    // NB: IoSlice has the same layout as iovec on Unix.
    let mut control = vec![0u64; control_buffer_size(fds.len()).div_ceil(8)];
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
    let sent = unsafe {
        libc::sendmsg(
            socket.as_raw_fd(),
            &raw const message,
            libc::MSG_NOSIGNAL | libc::MSG_DONTWAIT,
        )
    };
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

    /// Receives bytes and any descriptors that came with them. Zero bytes mean the peer closed its end.
    pub(crate) fn receive_with_fds(
        &mut self,
        socket: &UnixStream,
        buffer: &mut [u8],
        fds: &mut Vec<OwnedFd>,
    ) -> io::Result<usize> {
        self.receive_with_fd_capacity(socket, buffer, fds, MAX_TRANSFER_FDS)
    }

    pub(crate) fn receive_with_fd_capacity(
        &mut self,
        socket: &UnixStream,
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
        let received = unsafe { libc::recvmsg(socket.as_raw_fd(), &raw mut message, flags) };
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

/// Copies a payload into an anonymous file, whose descriptor goes last in the frame.
pub(crate) fn copy_payload_to_file(bytes: &[u8]) -> io::Result<std::fs::File> {
    use std::io::Write;
    let mut file = std::fs::File::from(create_anonymous_file()?);
    file.write_all(bytes)?;
    Ok(file)
}

/// Copies bytes before decoding, using pread so a size change reports a short read.
pub(crate) fn copy_payload_from_file(fd: OwnedFd, size: usize) -> io::Result<Vec<u8>> {
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

    #[cfg(target_os = "linux")]
    #[test]
    fn receiving_payloads_does_not_add_seals() {
        for flags in [0, libc::MFD_ALLOW_SEALING] {
            // NB: memfd_create() returns a new descriptor for a NUL-terminated name.
            let fd = unsafe { libc::memfd_create(c"".as_ptr(), libc::MFD_CLOEXEC | flags) };
            assert!(fd >= 0);
            // NB: The descriptor is new and nothing else owns it.
            let fd = unsafe { OwnedFd::from_raw_fd(fd) };
            let mut file = std::fs::File::from(fd);
            std::io::Write::write_all(&mut file, b"payload").unwrap();
            // NB: F_GET_SEALS only reads the owned object's seals.
            let seals = unsafe { libc::fcntl(file.as_raw_fd(), libc::F_GET_SEALS) };
            assert_eq!(
                copy_payload_from_file(file.try_clone().unwrap().into(), 7).unwrap(),
                b"payload"
            );
            // NB: The file is still owned and F_GET_SEALS does not change it.
            assert_eq!(unsafe { libc::fcntl(file.as_raw_fd(), libc::F_GET_SEALS) }, seals);
            file.set_len(0).unwrap();
        }
    }

    #[test]
    fn transports_accept_nonblocking_sockets() {
        use crate::transport_socket::TransportSocket;

        let (a, b) = UnixStream::pair().unwrap();
        a.set_nonblocking(true).unwrap();
        b.set_nonblocking(true).unwrap();
        let descriptor = a.try_clone().unwrap();
        let a = TransportSocket::new(a).unwrap();
        let b = TransportSocket::new(b).unwrap();
        // NB: The socket is owned and F_GETFL only reads its status flags.
        let flags = unsafe { libc::fcntl(descriptor.as_raw_fd(), libc::F_GETFL) };
        assert!(flags >= 0);
        assert_eq!(flags & libc::O_NONBLOCK, 0);
        a.post_message(vec![1], Vec::new()).unwrap();
        b.post_message(vec![2], Vec::new()).unwrap();
        assert_eq!(a.receive().unwrap().bytes(), [2]);
        assert_eq!(b.receive().unwrap().bytes(), [1]);
    }

    #[test]
    fn received_descriptors_are_close_on_exec() {
        use std::os::fd::AsFd;

        let (sender, receiver) = UnixStream::pair().unwrap();
        let file = std::fs::File::open("/dev/null").unwrap();
        send_with_fds(&sender, &[io::IoSlice::new(&[1])], &[file.as_fd()]).unwrap();
        let mut fds = Vec::new();
        Receiver::new().receive_with_fds(&receiver, &mut [0], &mut fds).unwrap();
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
                &sender,
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
                .receive_with_fds(&receiver, &mut bytes, &mut fds)
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
            send_with_fds(&sender, &[io::IoSlice::new(&[1])], &[])
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
        send_with_fds(&sender, &[io::IoSlice::new(&[1])], &[writer.as_fd()]).unwrap();
        receiver.receive_with_fds(&socket, &mut [0], &mut fds).unwrap();
        assert_eq!(fds.len(), 1);
        fds.clear();
        drop(writer);
        assert_eq!(reader.read(&mut [0]).unwrap(), 0);
        send_with_fds(&sender, &[io::IoSlice::new(&[2])], &[]).unwrap();
        let mut byte = [0];
        assert_eq!(receiver.receive_with_fds(&socket, &mut byte, &mut fds).unwrap(), 1);
        assert_eq!(byte, [2]);
        assert!(fds.is_empty());
    }

    #[test]
    fn truncated_control_messages_adopt_the_delivered_descriptors() {
        use std::os::fd::AsFd;

        let (sender, receiver) = UnixStream::pair().unwrap();
        let (mut reader, writer) = std::io::pipe().unwrap();
        send_with_fds(&sender, &[io::IoSlice::new(&[1])], &[writer.as_fd(); 3]).unwrap();
        drop(writer);
        let mut fds = Vec::new();
        let error = Receiver::new()
            .receive_with_fd_capacity(&receiver, &mut [0; 1], &mut fds, 1)
            .unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidData);
        assert!(!fds.is_empty());
        assert!(fds.len() < 3);
        drop(fds);
        assert_eq!(reader.read(&mut [0]).unwrap(), 0);
    }
}
