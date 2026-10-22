/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The `wasi_snapshot_preview1` interface, for running WASI commands outside of a browser. Its functions are host
//! functions of a store, which work on the memory `set_memory()` names, and only exist on Unix hosts.

use crate::runtime::{FunctionAddress, MemoryAddress, Store, Trap, Value};
use crate::types::{FuncType, ValType};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;

/// The name of the module WASI functions are imported from.
pub const MODULE_NAME: &str = "wasi_snapshot_preview1";

/// A host directory that the guest sees at `guest_path`.
pub struct Preopen {
    pub host_path: String,
    pub guest_path: String,
    /// The host descriptor of the directory, once something has opened it.
    host_fd: Cell<Option<i32>>,
}

impl Preopen {
    pub fn new(host_path: String, guest_path: String) -> Self {
        Self {
            host_path,
            guest_path,
            host_fd: Cell::new(None),
        }
    }
}

/// The state of a WASI command: its arguments, environment, descriptors, and how it exited.
pub struct Wasi {
    arguments: Vec<String>,
    environment: Vec<String>,
    /// The preopened directories, which are the guest's descriptors from 3 up.
    preopens: Vec<Preopen>,
    /// The guest's open descriptors besides the preopens: 0 to 2, which are the host's standard input, output and
    /// error, and those path_open created.
    files: RefCell<HashMap<u32, File>>,
    next_fd: Cell<u32>,
    memory: Cell<Option<MemoryAddress>>,
    exit_code: Cell<Option<i32>>,
}

impl Drop for Wasi {
    fn drop(&mut self) {
        // The descriptors path_open created that the guest left open, and the preopens something opened, are the
        // state's own.
        for file in self.files.get_mut().values().filter(|file| file.owned) {
            let _ = platform::close(file.host_fd);
        }
        for fd in self.preopens.iter().filter_map(|preopen| preopen.host_fd.get()) {
            let _ = platform::close(fd);
        }
    }
}

/// A guest descriptor's host descriptor.
#[derive(Clone, Copy)]
struct File {
    host_fd: i32,
    /// Whether the guest owns the host descriptor, which closing the guest's then closes. The host's standard
    /// streams stay open whatever the guest closes.
    owned: bool,
}

/// A WASI errno value.
type Errno = u16;

const ERRNO_SUCCESS: Errno = 0;
const ERRNO_BADF: Errno = 8;
const ERRNO_FAULT: Errno = 21;
// NB: Only the Unix implementation maps host errors.
#[cfg_attr(not(unix), allow(dead_code))]
const ERRNO_INVAL: Errno = 28;
#[cfg_attr(not(unix), allow(dead_code))]
const ERRNO_IO: Errno = 29;
const ERRNO_ISDIR: Errno = 31;
const ERRNO_NOSYS: Errno = 52;
#[cfg_attr(not(unix), allow(dead_code))]
const ERRNO_NOTCAPABLE: Errno = 76;

/// What a guest descriptor refers to.
enum Descriptor {
    Host(i32),
    Preopen(usize),
}

impl Wasi {
    pub fn new(arguments: Vec<String>, environment: Vec<String>, preopens: Vec<Preopen>) -> Self {
        let next_fd = 3 + preopens.len() as u32;
        Self {
            arguments,
            environment,
            preopens,
            files: RefCell::new(
                (0..3)
                    .map(|fd| {
                        (
                            fd,
                            File {
                                host_fd: fd as i32,
                                owned: false,
                            },
                        )
                    })
                    .collect(),
            ),
            next_fd: Cell::new(next_fd),
            memory: Cell::new(None),
            exit_code: Cell::new(None),
        }
    }

    /// Have the functions work on `memory`, which is usually the memory the command exports.
    pub fn set_memory(&self, memory: MemoryAddress) {
        self.memory.set(Some(memory));
    }

    /// The code the command passed to proc_exit, if it called it.
    pub fn exit_code(&self) -> Option<i32> {
        self.exit_code.get()
    }

    /// Add the WASI function `name` to `store`. Returns None if there is no such function.
    pub fn allocate_function(self: &Rc<Self>, store: &mut Store, name: &str) -> Option<FunctionAddress> {
        let &(name, signature) = FUNCTIONS.iter().find(|(function, _)| *function == name)?;
        let (parameters, result) = signature.split_once('>').unwrap_or((signature, ""));
        let to_types = |text: &str| -> Vec<ValType> {
            text.chars()
                .map(|character| if character == 'I' { ValType::I64 } else { ValType::I32 })
                .collect()
        };
        let ty = FuncType {
            params: to_types(parameters),
            results: to_types(result),
        };
        let wasi = self.clone();
        Some(store.allocate_host_function(
            ty,
            Rc::new(move |store: &mut Store, arguments: &[Value]| wasi.call(store, name, arguments)),
        ))
    }

    fn call(&self, store: &mut Store, name: &str, arguments: &[Value]) -> Result<Vec<Value>, Trap> {
        let argument = |index: usize| -> u64 {
            match arguments[index] {
                Value::I32(value) => u64::from(value as u32),
                Value::I64(value) => value as u64,
                _ => 0,
            }
        };
        if name == "proc_exit" {
            let code = argument(0) as u32 as i32;
            self.exit_code.set(Some(code));
            return Err(Trap::Host(format!("exit:{code}")));
        }
        let Some(memory) = self.memory.get() else {
            return Ok(vec![Value::I32(i32::from(ERRNO_FAULT))]);
        };
        let mut memory = Memory(store.memory_mut(memory).bytes_mut());
        let errno = match self.dispatch(&mut memory, name, &argument) {
            Ok(()) => ERRNO_SUCCESS,
            Err(errno) => errno,
        };
        Ok(vec![Value::I32(i32::from(errno))])
    }

    fn descriptor(&self, fd: u32) -> Option<Descriptor> {
        if let Some(preopen) = fd.checked_sub(3)
            && (preopen as usize) < self.preopens.len()
        {
            return Some(Descriptor::Preopen(preopen as usize));
        }
        self.files.borrow().get(&fd).map(|file| Descriptor::Host(file.host_fd))
    }

    /// The host descriptor of `fd`. Paths only reach what is beneath a descriptor the guest has (the capability model):
    /// the platform functions resolve them beneath the one this returns.
    fn host_fd(&self, fd: u32) -> Result<i32, Errno> {
        match self.descriptor(fd) {
            Some(Descriptor::Host(host_fd)) => Ok(host_fd),
            Some(Descriptor::Preopen(index)) => self.preopen_fd(index),
            None => Err(ERRNO_BADF),
        }
    }

    fn preopen_fd(&self, index: usize) -> Result<i32, Errno> {
        let preopen = &self.preopens[index];
        if let Some(fd) = preopen.host_fd.get() {
            return Ok(fd);
        }
        let fd = platform::open_directory(&preopen.host_path)?;
        preopen.host_fd.set(Some(fd));
        Ok(fd)
    }

    fn dispatch(&self, memory: &mut Memory, name: &str, argument: &dyn Fn(usize) -> u64) -> Result<(), Errno> {
        let a = |index: usize| argument(index) as u32;
        match name {
            "args_get" => write_strings(memory, &self.arguments, a(0), a(1)),
            "args_sizes_get" => write_string_sizes(memory, &self.arguments, a(0), a(1)),
            "environ_get" => write_strings(memory, &self.environment, a(0), a(1)),
            "environ_sizes_get" => write_string_sizes(memory, &self.environment, a(0), a(1)),
            "clock_res_get" => memory.write_u64(a(1), platform::clock_resolution(a(0))?),
            "clock_time_get" => memory.write_u64(a(2), platform::clock_time(a(0))?),
            "fd_close" => match self.descriptor(a(0)) {
                Some(Descriptor::Host(_)) => {
                    let file = self.files.borrow_mut().remove(&a(0)).expect("the descriptor is a file");
                    if file.owned {
                        platform::close(file.host_fd)?;
                    }
                    Ok(())
                }
                Some(Descriptor::Preopen(_)) => Err(ERRNO_ISDIR),
                None => Err(ERRNO_BADF),
            },
            "fd_fdstat_get" => {
                let host_fd = self.host_fd(a(0))?;
                let (filetype, flags) = platform::fd_status(host_fd)?;
                memory.write_u8(a(1), filetype)?;
                memory.write_u16(a(1).wrapping_add(2), flags)?;
                // FIXME: Implement rights.
                memory.write_u64(a(1).wrapping_add(8), 0)?;
                memory.write_u64(a(1).wrapping_add(16), 0)
            }
            "fd_filestat_get" => {
                let host_fd = self.host_fd(a(0))?;
                write_filestat(memory, a(1), &platform::fstat(host_fd)?)
            }
            "fd_prestat_get" => {
                let Some(Descriptor::Preopen(index)) = self.descriptor(a(0)) else {
                    return Err(ERRNO_BADF);
                };
                // The only kind of preopened descriptor is a directory, tag 0.
                memory.write_u8(a(1), 0)?;
                memory.write_u32(a(1).wrapping_add(4), self.preopens[index].guest_path.len() as u32)
            }
            "fd_prestat_dir_name" => {
                let Some(Descriptor::Preopen(index)) = self.descriptor(a(0)) else {
                    return Err(ERRNO_BADF);
                };
                let name = self.preopens[index].guest_path.as_bytes();
                let length = (a(2) as usize).min(name.len());
                memory.slice(a(1), length as u32)?.copy_from_slice(&name[..length]);
                Ok(())
            }
            "fd_read" | "fd_write" => {
                let Some(Descriptor::Host(host_fd)) = self.descriptor(a(0)) else {
                    return Err(ERRNO_BADF);
                };
                let mut total: u32 = 0;
                for index in 0..a(2) {
                    let iovec = a(1).wrapping_add(index.wrapping_mul(8));
                    let buffer = memory.read_u32(iovec)?;
                    let length = memory.read_u32(iovec.wrapping_add(4))?;
                    let bytes = memory.slice(buffer, length)?;
                    let transferred = if name == "fd_read" {
                        platform::read(host_fd, bytes)?
                    } else {
                        platform::write(host_fd, bytes)?
                    };
                    total = total.wrapping_add(transferred as u32);
                    if transferred < bytes.len() {
                        break;
                    }
                }
                memory.write_u32(a(3), total)
            }
            "fd_seek" => {
                let Some(Descriptor::Host(host_fd)) = self.descriptor(a(0)) else {
                    return Err(ERRNO_BADF);
                };
                let position = platform::seek(host_fd, argument(1) as i64, a(2))?;
                memory.write_u64(a(3), position)
            }
            "fd_tell" => {
                let Some(Descriptor::Host(host_fd)) = self.descriptor(a(0)) else {
                    return Err(ERRNO_BADF);
                };
                let position = platform::seek(host_fd, 0, platform::WHENCE_CURRENT)?;
                memory.write_u64(a(1), position)
            }
            "path_create_directory" => {
                let directory = self.host_fd(a(0))?;
                platform::create_directory(directory, memory.slice(a(1), a(2))?)
            }
            "path_filestat_get" => {
                let directory = self.host_fd(a(0))?;
                let follow_symlinks = a(1) & LOOKUP_SYMLINK_FOLLOW != 0;
                let status = platform::stat_at(directory, memory.slice(a(2), a(3))?, follow_symlinks)?;
                write_filestat(memory, a(4), &status)
            }
            "path_open" => {
                let directory = self.host_fd(a(0))?;
                let options = OpenOptions {
                    follow_symlinks: a(1) & LOOKUP_SYMLINK_FOLLOW != 0,
                    open_flags: a(4) as u16,
                    rights: argument(5),
                    fd_flags: a(7) as u16,
                };
                let host_fd = platform::open_at(directory, memory.slice(a(2), a(3))?, &options)?;
                let fd = self.next_fd.get();
                self.next_fd.set(fd + 1);
                self.files.borrow_mut().insert(fd, File { host_fd, owned: true });
                memory.write_u32(a(8), fd)
            }
            "random_get" => platform::fill_with_random(memory.slice(a(0), a(1))?),
            "sched_yield" => Ok(()),
            _ => Err(ERRNO_NOSYS),
        }
    }
}

const LOOKUP_SYMLINK_FOLLOW: u32 = 1;

/// How path_open opens a file.
#[cfg_attr(not(unix), allow(dead_code))]
struct OpenOptions {
    follow_symlinks: bool,
    open_flags: u16,
    rights: u64,
    fd_flags: u16,
}

/// Each function and its type: 'i' is i32 and 'I' is i64, with the result after '>'.
static FUNCTIONS: &[(&str, &str)] = &[
    ("args_get", "ii>i"),
    ("args_sizes_get", "ii>i"),
    ("environ_get", "ii>i"),
    ("environ_sizes_get", "ii>i"),
    ("clock_res_get", "ii>i"),
    ("clock_time_get", "iIi>i"),
    ("fd_advise", "iIIi>i"),
    ("fd_allocate", "iII>i"),
    ("fd_close", "i>i"),
    ("fd_datasync", "i>i"),
    ("fd_fdstat_get", "ii>i"),
    ("fd_fdstat_set_flags", "ii>i"),
    ("fd_fdstat_set_rights", "iII>i"),
    ("fd_filestat_get", "ii>i"),
    ("fd_filestat_set_size", "iI>i"),
    ("fd_filestat_set_times", "iIIi>i"),
    ("fd_pread", "iiiIi>i"),
    ("fd_prestat_get", "ii>i"),
    ("fd_prestat_dir_name", "iii>i"),
    ("fd_pwrite", "iiiIi>i"),
    ("fd_read", "iiii>i"),
    ("fd_readdir", "iiiIi>i"),
    ("fd_renumber", "ii>i"),
    ("fd_seek", "iIii>i"),
    ("fd_sync", "i>i"),
    ("fd_tell", "ii>i"),
    ("fd_write", "iiii>i"),
    ("path_create_directory", "iii>i"),
    ("path_filestat_get", "iiiii>i"),
    ("path_filestat_set_times", "iiiiIIi>i"),
    ("path_link", "iiiiiii>i"),
    ("path_open", "iiiiiIIii>i"),
    ("path_readlink", "iiiiii>i"),
    ("path_remove_directory", "iii>i"),
    ("path_rename", "iiiiii>i"),
    ("path_symlink", "iiiii>i"),
    ("path_unlink_file", "iii>i"),
    ("poll_oneoff", "iiii>i"),
    ("proc_exit", "i"),
    ("proc_raise", "i>i"),
    ("sched_yield", ">i"),
    ("random_get", "ii>i"),
    ("sock_accept", "iii>i"),
    ("sock_recv", "iiiiii>i"),
    ("sock_send", "iiiii>i"),
    ("sock_shutdown", "ii>i"),
];

/// The guest's memory, which every access checks the bounds of.
struct Memory<'a>(&'a mut [u8]);

impl Memory<'_> {
    fn slice(&mut self, address: u32, length: u32) -> Result<&mut [u8], Errno> {
        let start = address as usize;
        let end = start.checked_add(length as usize).ok_or(ERRNO_FAULT)?;
        self.0.get_mut(start..end).ok_or(ERRNO_FAULT)
    }

    fn read_u32(&mut self, address: u32) -> Result<u32, Errno> {
        let bytes = self.slice(address, 4)?;
        Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    fn write(&mut self, address: u32, bytes: &[u8]) -> Result<(), Errno> {
        self.slice(address, bytes.len() as u32)?.copy_from_slice(bytes);
        Ok(())
    }

    fn write_u8(&mut self, address: u32, value: u8) -> Result<(), Errno> {
        self.write(address, &[value])
    }

    fn write_u16(&mut self, address: u32, value: u16) -> Result<(), Errno> {
        self.write(address, &value.to_le_bytes())
    }

    fn write_u32(&mut self, address: u32, value: u32) -> Result<(), Errno> {
        self.write(address, &value.to_le_bytes())
    }

    fn write_u64(&mut self, address: u32, value: u64) -> Result<(), Errno> {
        self.write(address, &value.to_le_bytes())
    }
}

/// Write the pointers to `strings` at `pointers`, and the strings, each followed by a null byte, at `buffer`.
fn write_strings(memory: &mut Memory, strings: &[String], pointers: u32, buffer: u32) -> Result<(), Errno> {
    let mut offset = buffer;
    for (index, string) in strings.iter().enumerate() {
        memory.write_u32(pointers.wrapping_add(index as u32 * 4), offset)?;
        memory.write(offset, string.as_bytes())?;
        memory.write_u8(offset.wrapping_add(string.len() as u32), 0)?;
        offset = offset.wrapping_add(string.len() as u32 + 1);
    }
    Ok(())
}

fn write_string_sizes(memory: &mut Memory, strings: &[String], count: u32, size: u32) -> Result<(), Errno> {
    memory.write_u32(count, strings.len() as u32)?;
    let total: usize = strings.iter().map(|string| string.len() + 1).sum();
    memory.write_u32(size, total as u32)
}

/// What fd_filestat_get and path_filestat_get return.
pub struct FileStatus {
    device: u64,
    inode: u64,
    filetype: u8,
    link_count: u64,
    size: u64,
    access_time: u64,
    modification_time: u64,
    status_change_time: u64,
}

fn write_filestat(memory: &mut Memory, address: u32, status: &FileStatus) -> Result<(), Errno> {
    memory.write_u64(address, status.device)?;
    memory.write_u64(address.wrapping_add(8), status.inode)?;
    memory.write_u8(address.wrapping_add(16), status.filetype)?;
    memory.write_u64(address.wrapping_add(24), status.link_count)?;
    memory.write_u64(address.wrapping_add(32), status.size)?;
    memory.write_u64(address.wrapping_add(40), status.access_time)?;
    memory.write_u64(address.wrapping_add(48), status.modification_time)?;
    memory.write_u64(address.wrapping_add(56), status.status_change_time)
}

#[cfg(unix)]
mod platform {
    use super::{ERRNO_INVAL, ERRNO_IO, ERRNO_NOTCAPABLE, Errno, FileStatus, OpenOptions};
    use std::collections::VecDeque;
    use std::ffi::CString;

    pub const WHENCE_CURRENT: u32 = 1;

    /// The most symbolic links one path resolution follows, as Linux's.
    const MAX_SYMLINKS: u32 = 40;

    /// A path resolved beneath a directory: the directory its last component is in, and that component, which is
    /// neither `..` nor a symbolic link that the resolution was to follow.
    struct Beneath {
        directory: i32,
        /// Whether `directory` is a descriptor of the resolution's own, which it closes.
        owned: bool,
        name: CString,
        /// Whether the path ended in a slash, which only a directory may be named with.
        must_be_directory: bool,
    }

    impl Drop for Beneath {
        fn drop(&mut self) {
            if self.owned {
                unsafe { libc::close(self.directory) };
            }
        }
    }

    fn components(path: &[u8]) -> impl DoubleEndedIterator<Item = &[u8]> {
        path.split(|byte| *byte == b'/')
            .filter(|component| !component.is_empty() && *component != b".")
    }

    /// The symbolic link `name` in `directory` points to, or None if it is not one.
    fn link_target(directory: i32, name: &CString) -> Result<Option<Vec<u8>>, Errno> {
        let mut status = unsafe { std::mem::zeroed::<libc::stat>() };
        if unsafe { libc::fstatat(directory, name.as_ptr(), &raw mut status, libc::AT_SYMLINK_NOFOLLOW) } != 0
            || status.st_mode & libc::S_IFMT != libc::S_IFLNK
        {
            return Ok(None);
        }
        let mut target = vec![0u8; libc::PATH_MAX as usize];
        let length = unsafe { libc::readlinkat(directory, name.as_ptr(), target.as_mut_ptr().cast(), target.len()) };
        if length < 0 {
            return Err(last_errno());
        }
        // NB: readlinkat() truncates a target that does not fit without saying so.
        if length as usize == target.len() {
            return Err(errno_of(libc::ENAMETOOLONG));
        }
        target.truncate(length as usize);
        Ok(Some(target))
    }

    /// Resolves `path` beneath the directory `base`, one component at a time, so that it never leaves it: absolute
    /// paths and symbolic links, and `..` above `base`, are not capable. Symbolic links in the middle are followed
    /// (beneath `base`), and the last one too if `follow_last`, or if the path ends in a slash, as POSIX resolves it.
    fn resolve_beneath(base: i32, path: &[u8], follow_last: bool) -> Result<Beneath, Errno> {
        if path.is_empty() {
            return Err(errno_of(libc::ENOENT));
        }
        if path[0] == b'/' {
            return Err(ERRNO_NOTCAPABLE);
        }
        // The directories below `base` the resolution is in, innermost last, which it owns.
        let mut directories: Vec<i32> = Vec::new();
        let close_all = |directories: &mut Vec<i32>| {
            for fd in directories.drain(..) {
                unsafe { libc::close(fd) };
            }
        };
        let mut queue: VecDeque<Vec<u8>> = components(path).map(<[u8]>::to_vec).collect();
        let mut must_be_directory = path.last() == Some(&b'/');
        let mut links = 0;
        let result = loop {
            let Some(component) = queue.pop_front() else {
                break Ok(c".".to_owned());
            };
            let current = *directories.last().unwrap_or(&base);
            if component == b".." {
                match directories.pop() {
                    Some(fd) => unsafe {
                        libc::close(fd);
                    },
                    None => break Err(ERRNO_NOTCAPABLE),
                }
                continue;
            }
            let name = match path_of(&component) {
                Ok(name) => name,
                Err(errno) => break Err(errno),
            };
            let last = queue.is_empty();
            if !last || follow_last || must_be_directory {
                match link_target(current, &name) {
                    Ok(Some(target)) => {
                        links += 1;
                        if links > MAX_SYMLINKS {
                            break Err(errno_of(libc::ELOOP));
                        }
                        if target.first() == Some(&b'/') {
                            break Err(ERRNO_NOTCAPABLE);
                        }
                        if last && target.last() == Some(&b'/') {
                            must_be_directory = true;
                        }
                        for component in components(&target).rev() {
                            queue.push_front(component.to_vec());
                        }
                        continue;
                    }
                    Ok(None) => {}
                    Err(errno) => break Err(errno),
                }
            }
            if last {
                break Ok(name);
            }
            let fd = unsafe {
                libc::openat(
                    current,
                    name.as_ptr(),
                    libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_RDONLY | libc::O_CLOEXEC,
                )
            };
            if fd < 0 {
                break Err(last_errno());
            }
            directories.push(fd);
        };
        match result {
            Ok(name) => {
                let directory = directories.pop();
                close_all(&mut directories);
                Ok(Beneath {
                    directory: directory.unwrap_or(base),
                    owned: directory.is_some(),
                    name,
                    must_be_directory,
                })
            }
            Err(errno) => {
                close_all(&mut directories);
                Err(errno)
            }
        }
    }

    const NANOSECONDS_PER_SECOND: u64 = 1_000_000_000;

    fn last_errno() -> Errno {
        errno_of(std::io::Error::last_os_error().raw_os_error().unwrap_or(0))
    }

    /// The WASI errno of a host errno.
    fn errno_of(host: i32) -> Errno {
        match host {
            libc::E2BIG => 1,
            libc::EACCES => 2,
            libc::EADDRINUSE => 3,
            libc::EADDRNOTAVAIL => 4,
            libc::EAFNOSUPPORT => 5,
            libc::EAGAIN => 6,
            libc::EALREADY => 7,
            libc::EBADF => 8,
            libc::EBUSY => 10,
            libc::ECHILD => 12,
            libc::ECONNREFUSED => 14,
            libc::ECONNRESET => 15,
            libc::EDEADLK => 16,
            libc::EDOM => 18,
            libc::EEXIST => 20,
            libc::EFAULT => 21,
            libc::EFBIG => 22,
            libc::EINTR => 27,
            libc::EINVAL => 28,
            libc::EIO => 29,
            libc::EISDIR => 31,
            libc::ELOOP => 32,
            libc::EMFILE => 33,
            libc::EMLINK => 34,
            libc::ENAMETOOLONG => 37,
            libc::ENFILE => 41,
            libc::ENOBUFS => 42,
            libc::ENODEV => 43,
            libc::ENOENT => 44,
            libc::ENOEXEC => 45,
            libc::ENOMEM => 48,
            libc::ENOSPC => 51,
            libc::ENOSYS => 52,
            libc::ENOTDIR => 54,
            libc::ENOTEMPTY => 55,
            libc::ENOTSUP => 58,
            libc::ENOTTY => 59,
            libc::ENXIO => 60,
            libc::EOVERFLOW => 61,
            libc::EPERM => 63,
            libc::EPIPE => 64,
            libc::ERANGE => 68,
            libc::EROFS => 69,
            libc::ESPIPE => 70,
            libc::ESRCH => 71,
            libc::ETIMEDOUT => 73,
            libc::EXDEV => 75,
            _ => ERRNO_IO,
        }
    }

    fn path_of(bytes: &[u8]) -> Result<CString, Errno> {
        CString::new(bytes).map_err(|_| ERRNO_INVAL)
    }

    pub fn open_directory(path: &str) -> Result<i32, Errno> {
        let path = path_of(path.as_bytes())?;
        let fd = unsafe { libc::open(path.as_ptr(), libc::O_DIRECTORY | libc::O_RDONLY | libc::O_CLOEXEC) };
        if fd < 0 { Err(last_errno()) } else { Ok(fd) }
    }

    pub fn open_at(directory: i32, path: &[u8], options: &OpenOptions) -> Result<i32, Errno> {
        const OFLAGS_CREAT: u16 = 1;
        const OFLAGS_DIRECTORY: u16 = 2;
        const OFLAGS_EXCL: u16 = 4;
        const OFLAGS_TRUNC: u16 = 8;
        const FDFLAGS_APPEND: u16 = 1;
        const FDFLAGS_NONBLOCK: u16 = 4;
        const FDFLAGS_SYNC: u16 = 16;
        const RIGHTS_FD_READ: u64 = 1 << 1;
        const RIGHTS_FD_WRITE: u64 = 1 << 6;

        let resolved = resolve_beneath(directory, path, options.follow_symlinks)?;
        let readable = options.rights & RIGHTS_FD_READ != 0;
        let writable = options.rights & RIGHTS_FD_WRITE != 0;
        let mut flags = match (readable, writable) {
            (_, false) => libc::O_RDONLY,
            (false, true) => libc::O_WRONLY,
            (true, true) => libc::O_RDWR,
        };
        flags |= libc::O_CLOEXEC;
        if options.open_flags & OFLAGS_DIRECTORY != 0 {
            flags = libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC;
        }
        if resolved.must_be_directory {
            // NB: As on Linux, a file cannot be created with a name that ends in a slash.
            if options.open_flags & OFLAGS_CREAT != 0 {
                return Err(errno_of(libc::EISDIR));
            }
            flags |= libc::O_DIRECTORY;
        }
        if options.open_flags & OFLAGS_CREAT != 0 {
            flags |= libc::O_CREAT;
        }
        if options.open_flags & OFLAGS_EXCL != 0 {
            flags |= libc::O_EXCL;
        }
        if options.open_flags & OFLAGS_TRUNC != 0 {
            flags |= libc::O_TRUNC;
        }
        if options.fd_flags & FDFLAGS_APPEND != 0 {
            flags |= libc::O_APPEND;
        }
        if options.fd_flags & FDFLAGS_NONBLOCK != 0 {
            flags |= libc::O_NONBLOCK;
        }
        if options.fd_flags & FDFLAGS_SYNC != 0 {
            flags |= libc::O_SYNC;
        }
        // NB: The resolution followed the symbolic links it was to follow: a link in the last place now is one that
        //     appeared since, which must not be followed out.
        flags |= libc::O_NOFOLLOW;
        let fd = unsafe { libc::openat(resolved.directory, resolved.name.as_ptr(), flags, 0o644 as libc::c_uint) };
        if fd < 0 { Err(last_errno()) } else { Ok(fd) }
    }

    pub fn close(fd: i32) -> Result<(), Errno> {
        if unsafe { libc::close(fd) } != 0 {
            return Err(last_errno());
        }
        Ok(())
    }

    pub fn read(fd: i32, bytes: &mut [u8]) -> Result<usize, Errno> {
        let result = unsafe { libc::read(fd, bytes.as_mut_ptr().cast(), bytes.len()) };
        if result < 0 {
            Err(last_errno())
        } else {
            Ok(result as usize)
        }
    }

    pub fn write(fd: i32, bytes: &[u8]) -> Result<usize, Errno> {
        let result = unsafe { libc::write(fd, bytes.as_ptr().cast(), bytes.len()) };
        if result < 0 {
            Err(last_errno())
        } else {
            Ok(result as usize)
        }
    }

    pub fn seek(fd: i32, offset: i64, whence: u32) -> Result<u64, Errno> {
        let whence = match whence {
            0 => libc::SEEK_SET,
            1 => libc::SEEK_CUR,
            2 => libc::SEEK_END,
            _ => return Err(ERRNO_INVAL),
        };
        let result = unsafe { libc::lseek(fd, offset as libc::off_t, whence) };
        if result < 0 {
            Err(last_errno())
        } else {
            Ok(result as u64)
        }
    }

    pub fn create_directory(directory: i32, path: &[u8]) -> Result<(), Errno> {
        let resolved = resolve_beneath(directory, path, false)?;
        if unsafe { libc::mkdirat(resolved.directory, resolved.name.as_ptr(), 0o755) } != 0 {
            return Err(last_errno());
        }
        Ok(())
    }

    fn filetype_of(mode: libc::mode_t) -> u8 {
        match mode & libc::S_IFMT {
            libc::S_IFBLK => 1,
            libc::S_IFCHR => 2,
            libc::S_IFDIR => 3,
            libc::S_IFREG => 4,
            libc::S_IFSOCK => 6,
            libc::S_IFLNK => 7,
            _ => 0,
        }
    }

    // NB: The types of stat's fields differ between hosts.
    #[allow(clippy::unnecessary_cast)]
    fn status_of(status: &libc::stat) -> FileStatus {
        let time = |seconds: i64, nanoseconds: i64| -> u64 {
            (seconds as u64)
                .wrapping_mul(NANOSECONDS_PER_SECOND)
                .wrapping_add(nanoseconds as u64)
        };
        FileStatus {
            device: status.st_dev as u64,
            inode: status.st_ino as u64,
            filetype: filetype_of(status.st_mode),
            link_count: status.st_nlink as u64,
            size: status.st_size as u64,
            access_time: time(status.st_atime as i64, status.st_atime_nsec as i64),
            modification_time: time(status.st_mtime as i64, status.st_mtime_nsec as i64),
            status_change_time: time(status.st_ctime as i64, status.st_ctime_nsec as i64),
        }
    }

    pub fn fstat(fd: i32) -> Result<FileStatus, Errno> {
        let mut status = unsafe { std::mem::zeroed::<libc::stat>() };
        if unsafe { libc::fstat(fd, &raw mut status) } != 0 {
            return Err(last_errno());
        }
        Ok(status_of(&status))
    }

    pub fn stat_at(directory: i32, path: &[u8], follow_symlinks: bool) -> Result<FileStatus, Errno> {
        let resolved = resolve_beneath(directory, path, follow_symlinks)?;
        let mut status = unsafe { std::mem::zeroed::<libc::stat>() };
        let (directory, name) = (resolved.directory, resolved.name.as_ptr());
        if unsafe { libc::fstatat(directory, name, &raw mut status, libc::AT_SYMLINK_NOFOLLOW) } != 0 {
            return Err(last_errno());
        }
        if resolved.must_be_directory && status.st_mode & libc::S_IFMT != libc::S_IFDIR {
            return Err(errno_of(libc::ENOTDIR));
        }
        Ok(status_of(&status))
    }

    /// The filetype and fdflags of a descriptor.
    pub fn fd_status(fd: i32) -> Result<(u8, u16), Errno> {
        let status = fstat(fd)?;
        let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
        if flags < 0 {
            return Err(last_errno());
        }
        let mut fd_flags = 0;
        if flags & libc::O_APPEND != 0 {
            fd_flags |= 1;
        }
        if flags & libc::O_NONBLOCK != 0 {
            fd_flags |= 4;
        }
        if flags & libc::O_SYNC == libc::O_SYNC {
            fd_flags |= 16;
        }
        Ok((status.filetype, fd_flags))
    }

    fn clock_of(id: u32) -> Result<libc::clockid_t, Errno> {
        match id {
            0 => Ok(libc::CLOCK_REALTIME),
            1 => Ok(libc::CLOCK_MONOTONIC),
            2 => Ok(libc::CLOCK_PROCESS_CPUTIME_ID),
            3 => Ok(libc::CLOCK_THREAD_CPUTIME_ID),
            _ => Err(ERRNO_INVAL),
        }
    }

    fn nanoseconds_of(time: &libc::timespec) -> u64 {
        (time.tv_sec as u64)
            .wrapping_mul(NANOSECONDS_PER_SECOND)
            .wrapping_add(time.tv_nsec as u64)
    }

    pub fn clock_time(id: u32) -> Result<u64, Errno> {
        let mut time = unsafe { std::mem::zeroed::<libc::timespec>() };
        if unsafe { libc::clock_gettime(clock_of(id)?, &raw mut time) } != 0 {
            return Err(last_errno());
        }
        Ok(nanoseconds_of(&time))
    }

    pub fn clock_resolution(id: u32) -> Result<u64, Errno> {
        let mut time = unsafe { std::mem::zeroed::<libc::timespec>() };
        if unsafe { libc::clock_getres(clock_of(id)?, &raw mut time) } != 0 {
            return Err(last_errno());
        }
        Ok(nanoseconds_of(&time))
    }

    // NB: The libc crate does not bind getentropy() for musl, whose C library has it.
    #[cfg(target_env = "musl")]
    unsafe extern "C" {
        fn getentropy(buffer: *mut libc::c_void, length: libc::size_t) -> libc::c_int;
    }
    #[cfg(not(target_env = "musl"))]
    use libc::getentropy;

    pub fn fill_with_random(bytes: &mut [u8]) -> Result<(), Errno> {
        // getentropy() fills at most 256 bytes at a time.
        for chunk in bytes.chunks_mut(256) {
            if unsafe { getentropy(chunk.as_mut_ptr().cast(), chunk.len()) } != 0 {
                return Err(last_errno());
            }
        }
        Ok(())
    }
}

#[cfg(not(unix))]
mod platform {
    use super::{ERRNO_NOSYS, Errno, FileStatus, OpenOptions};

    pub const WHENCE_CURRENT: u32 = 1;

    pub fn open_directory(_: &str) -> Result<i32, Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn open_at(_: i32, _: &[u8], _: &OpenOptions) -> Result<i32, Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn close(_: i32) -> Result<(), Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn read(_: i32, _: &mut [u8]) -> Result<usize, Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn write(_: i32, _: &[u8]) -> Result<usize, Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn seek(_: i32, _: i64, _: u32) -> Result<u64, Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn create_directory(_: i32, _: &[u8]) -> Result<(), Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn fstat(_: i32) -> Result<FileStatus, Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn stat_at(_: i32, _: &[u8], _: bool) -> Result<FileStatus, Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn fd_status(_: i32) -> Result<(u8, u16), Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn clock_time(_: u32) -> Result<u64, Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn clock_resolution(_: u32) -> Result<u64, Errno> {
        Err(ERRNO_NOSYS)
    }
    pub fn fill_with_random(_: &mut [u8]) -> Result<(), Errno> {
        Err(ERRNO_NOSYS)
    }
}
