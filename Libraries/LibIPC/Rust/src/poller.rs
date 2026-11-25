/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How the IO thread of the socket transports waits for any of their sockets: epoll on Linux, which hands back only
//! the sockets that are ready, and poll() over every socket elsewhere.
//!
//! A socket is registered with a token, which comes back with its events, and which the IO thread never gives to
//! another socket, so that an event left over from a transport whose IO has ended finds nothing.

#![allow(unsafe_code)]

use std::io;
use std::os::fd::RawFd;
use std::time::Duration;

#[derive(Clone, Copy)]
pub(crate) struct Event {
    pub(crate) token: u64,
    // NB: Also set once the peer has hung up or the socket has an error, which a read reports.
    pub(crate) readable: bool,
    pub(crate) writable: bool,
    pub(crate) hung_up: bool,
}

fn timeout_in_milliseconds(timeout: Option<Duration>) -> libc::c_int {
    match timeout {
        None => -1,
        Some(timeout) => libc::c_int::try_from(timeout.as_micros().div_ceil(1000)).unwrap_or(libc::c_int::MAX),
    }
}

#[cfg(any(target_os = "linux", target_os = "android"))]
pub(crate) use epoll::Poller;
#[cfg(not(any(target_os = "linux", target_os = "android")))]
pub(crate) use poll::Poller;

#[cfg(any(target_os = "linux", target_os = "android"))]
mod epoll {
    use super::*;
    use std::fs::File;
    use std::io::{Read, Write};
    use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

    const WAKEUP_TOKEN: u64 = u64::MAX;
    const MAX_EVENTS_PER_WAIT: usize = 64;

    pub(crate) struct Poller {
        epoll: OwnedFd,
        wakeup: File,
    }

    fn owned(fd: libc::c_int) -> io::Result<OwnedFd> {
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        // NB: The descriptor is new, and nothing else owns it.
        Ok(unsafe { OwnedFd::from_raw_fd(fd) })
    }

    impl Poller {
        pub(crate) fn new() -> io::Result<Self> {
            // NB: Both calls only create descriptors.
            let epoll = owned(unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) })?;
            let wakeup = owned(unsafe { libc::eventfd(0, libc::EFD_CLOEXEC | libc::EFD_NONBLOCK) })?;
            let poller = Self {
                epoll,
                wakeup: wakeup.into(),
            };
            poller.control(
                libc::EPOLL_CTL_ADD,
                poller.wakeup.as_raw_fd(),
                libc::EPOLLIN as u32,
                WAKEUP_TOKEN,
            )?;
            Ok(poller)
        }

        fn control(&self, operation: libc::c_int, fd: RawFd, events: u32, token: u64) -> io::Result<()> {
            let mut event = libc::epoll_event { events, u64: token };
            // NB: The event is a local that the call reads.
            if unsafe { libc::epoll_ctl(self.epoll.as_raw_fd(), operation, fd, &raw mut event) } < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        }

        fn interest(writable: bool) -> u32 {
            let mut events = libc::EPOLLIN | libc::EPOLLRDHUP;
            if writable {
                events |= libc::EPOLLOUT;
            }
            events as u32
        }

        pub(crate) fn add(&self, fd: RawFd, token: u64) -> io::Result<()> {
            self.control(libc::EPOLL_CTL_ADD, fd, Self::interest(false), token)
        }

        pub(crate) fn set_writable_interest(&self, fd: RawFd, token: u64, writable: bool) -> io::Result<()> {
            self.control(libc::EPOLL_CTL_MOD, fd, Self::interest(writable), token)
        }

        pub(crate) fn remove(&self, fd: RawFd) -> io::Result<()> {
            // NB: EPOLL_CTL_DEL ignores the event.
            if unsafe { libc::epoll_ctl(self.epoll.as_raw_fd(), libc::EPOLL_CTL_DEL, fd, std::ptr::null_mut()) } < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        }

        pub(crate) fn wake(&self) {
            // NB: A counter at its maximum already has the IO thread on its way.
            let _ = (&self.wakeup).write_all(&1u64.to_ne_bytes());
        }

        /// Waits no longer than the timeout, if there is one, for the registered sockets, and answers whether anything
        /// happened, a wakeup included.
        pub(crate) fn wait(&self, events: &mut Vec<Event>, timeout: Option<Duration>) -> io::Result<bool> {
            events.clear();
            let mut ready = [libc::epoll_event { events: 0, u64: 0 }; MAX_EVENTS_PER_WAIT];
            // NB: The call writes at most as many events as the buffer has room for.
            let count = unsafe {
                libc::epoll_wait(
                    self.epoll.as_raw_fd(),
                    ready.as_mut_ptr(),
                    ready.len() as libc::c_int,
                    timeout_in_milliseconds(timeout),
                )
            };
            if count < 0 {
                let error = io::Error::last_os_error();
                if error.kind() == io::ErrorKind::Interrupted {
                    return Ok(true);
                }
                return Err(error);
            }
            for event in &ready[..count as usize] {
                // NB: The struct is packed on some targets, so its fields are copied out rather than borrowed.
                let (flags, token) = (event.events as libc::c_int, event.u64);
                if token == WAKEUP_TOKEN {
                    // NB: Reading resets the counter.
                    let _ = (&self.wakeup).read(&mut [0; size_of::<u64>()]);
                    continue;
                }
                let hung_up = flags & (libc::EPOLLHUP | libc::EPOLLERR) != 0;
                events.push(Event {
                    token,
                    readable: hung_up || flags & (libc::EPOLLIN | libc::EPOLLRDHUP) != 0,
                    writable: flags & libc::EPOLLOUT != 0,
                    hung_up,
                });
            }
            Ok(count > 0)
        }
    }
}

#[cfg(not(any(target_os = "linux", target_os = "android")))]
mod poll {
    use super::*;
    use std::io::{PipeReader, PipeWriter, Read, Write};
    use std::os::fd::AsRawFd;
    use std::sync::Mutex;

    use crate::transport::{is_shared_io_thread, lock};

    struct Registration {
        fd: RawFd,
        token: u64,
        writable: bool,
    }

    #[derive(Default)]
    struct WaitBuffers {
        pollfds: Vec<libc::pollfd>,
        tokens: Vec<u64>,
    }

    pub(crate) struct Poller {
        registrations: Mutex<Vec<Registration>>,
        wakeup_read: PipeReader,
        wakeup_write: PipeWriter,
        // NB: Only the IO thread waits, so this lock is never contended.
        wait_buffers: Mutex<WaitBuffers>,
    }

    fn set_nonblocking(fd: RawFd) -> io::Result<()> {
        // NB: fcntl() only changes the flags of a descriptor that the caller owns.
        unsafe {
            let flags = libc::fcntl(fd, libc::F_GETFL);
            if flags < 0 || libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) < 0 {
                return Err(io::Error::last_os_error());
            }
        }
        Ok(())
    }

    impl Poller {
        pub(crate) fn new() -> io::Result<Self> {
            let (wakeup_read, wakeup_write) = io::pipe()?;
            set_nonblocking(wakeup_read.as_raw_fd())?;
            set_nonblocking(wakeup_write.as_raw_fd())?;
            Ok(Self {
                registrations: Mutex::new(Vec::new()),
                wakeup_read,
                wakeup_write,
                wait_buffers: Mutex::new(WaitBuffers::default()),
            })
        }

        // NB: A change takes effect at the IO thread's next wait, which another thread wakes it for.
        fn wake_unless_on_io_thread(&self) {
            if !is_shared_io_thread() {
                self.wake();
            }
        }

        pub(crate) fn add(&self, fd: RawFd, token: u64) -> io::Result<()> {
            lock(&self.registrations).push(Registration {
                fd,
                token,
                writable: false,
            });
            self.wake_unless_on_io_thread();
            Ok(())
        }

        pub(crate) fn set_writable_interest(&self, fd: RawFd, token: u64, writable: bool) -> io::Result<()> {
            let mut registrations = lock(&self.registrations);
            let registration = registrations
                .iter_mut()
                .find(|registration| registration.fd == fd && registration.token == token)
                .ok_or(io::ErrorKind::NotFound)?;
            registration.writable = writable;
            drop(registrations);
            self.wake_unless_on_io_thread();
            Ok(())
        }

        pub(crate) fn remove(&self, fd: RawFd) -> io::Result<()> {
            lock(&self.registrations).retain(|registration| registration.fd != fd);
            Ok(())
        }

        pub(crate) fn wake(&self) {
            // NB: A full pipe already has the IO thread on its way.
            let _ = (&self.wakeup_write).write_all(&[0]);
        }

        /// Waits no longer than the timeout, if there is one, for the registered sockets, and answers whether anything
        /// happened, a wakeup included.
        pub(crate) fn wait(&self, events: &mut Vec<Event>, timeout: Option<Duration>) -> io::Result<bool> {
            events.clear();
            let mut buffers = lock(&self.wait_buffers);
            let WaitBuffers { pollfds, tokens } = &mut *buffers;
            pollfds.clear();
            tokens.clear();
            pollfds.push(libc::pollfd {
                fd: self.wakeup_read.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            });
            for registration in lock(&self.registrations).iter() {
                let mut interest = libc::POLLIN;
                if registration.writable {
                    interest |= libc::POLLOUT;
                }
                pollfds.push(libc::pollfd {
                    fd: registration.fd,
                    events: interest,
                    revents: 0,
                });
                tokens.push(registration.token);
            }
            // NB: The call writes the results into the array, which has as many entries as it is told.
            let count = unsafe {
                libc::poll(
                    pollfds.as_mut_ptr(),
                    pollfds.len() as libc::nfds_t,
                    timeout_in_milliseconds(timeout),
                )
            };
            if count < 0 {
                let error = io::Error::last_os_error();
                if error.kind() == io::ErrorKind::Interrupted {
                    return Ok(true);
                }
                return Err(error);
            }
            if pollfds[0].revents != 0 {
                let _ = (&self.wakeup_read).read(&mut [0; 64]);
            }
            for (pollfd, &token) in pollfds[1..].iter().zip(tokens.iter()) {
                if pollfd.revents == 0 {
                    continue;
                }
                let hung_up = pollfd.revents & (libc::POLLHUP | libc::POLLERR | libc::POLLNVAL) != 0;
                events.push(Event {
                    token,
                    readable: hung_up || pollfd.revents & libc::POLLIN != 0,
                    writable: pollfd.revents & libc::POLLOUT != 0,
                    hung_up,
                });
            }
            Ok(count > 0)
        }
    }
}
