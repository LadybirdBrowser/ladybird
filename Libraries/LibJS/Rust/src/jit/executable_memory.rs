/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Memory for JIT code.
//!
//! Code is carved out of large chunks that are mapped read+execute, and every write makes the affected pages writable
//! (and not executable) for the duration of the copy: memory is never writable and executable at once. On Linux
//! x86-64 with memory protection keys, the pages are writable and executable, but a protection key, allocated before
//! the process has threads, keeps every thread from writing them except the one writing code, while it writes. On
//! macOS, chunks are MAP_JIT mappings whose write protection is toggled per thread instead. On Windows, chunks are only
//! reserved, and pages get committed when code is first written to them.
//!
//! Every VM has its own chunks, which only the VM's thread writes and runs code in, so flipping the protection of
//! pages that hold other installed code is safe.

use core::cell::RefCell;
use std::rc::Rc;

#[cfg(windows)]
use ak::windows_code_memory;

const CHUNK_SIZE: usize = 4 * 1024 * 1024;
const ALLOCATION_ALIGNMENT: usize = 64;

/// How much of the start of a chunk holds no code.
#[cfg(windows)]
const CHUNK_HEADER_SIZE: usize = windows_code_memory::UNWIND_INFO_SIZE;
#[cfg(not(windows))]
const CHUNK_HEADER_SIZE: usize = 0;

const _: () = assert!(CHUNK_HEADER_SIZE.is_multiple_of(ALLOCATION_ALIGNMENT));

#[cfg(not(windows))]
fn page_size() -> usize {
    // SAFETY: sysconf() has no preconditions.
    let size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    usize::try_from(size).expect("the page size is positive")
}

#[cfg(windows)]
fn page_size() -> usize {
    windows_code_memory::PAGE_SIZE
}

fn align_up(value: usize, alignment: usize) -> usize {
    value.div_ceil(alignment) * alignment
}

#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
mod protection_key {
    use std::sync::OnceLock;

    /// PKEY_DISABLE_WRITE from <sys/mman.h>.
    const PKEY_DISABLE_WRITE: libc::c_uint = 2;

    unsafe extern "C" {
        /// glibc's wrapper of WRPKRU for one key.
        fn pkey_set(key: libc::c_int, rights: libc::c_uint) -> libc::c_int;
        safe fn pkey_alloc(flags: libc::c_uint, access_rights: libc::c_uint) -> libc::c_int;
        pub fn pkey_mprotect(
            address: *mut libc::c_void,
            length: libc::size_t,
            protection: libc::c_int,
            key: libc::c_int,
        ) -> libc::c_int;
    }

    static KEY: OnceLock<libc::c_int> = OnceLock::new();

    // NB: pkey_alloc() sets the rights to the key for the running thread only, and a new thread copies the rights of
    //     the thread that creates it. Allocating the key while the library loads, before there is a second thread, is
    //     what makes every thread start out unable to write with it.
    #[used]
    #[unsafe(link_section = ".init_array")]
    static ALLOCATE_KEY_ON_LOAD: extern "C" fn() = allocate_key;

    extern "C" fn allocate_key() {
        // NB: A thread that exists already would keep whatever rights it has to the key.
        if !process_has_one_thread() {
            return;
        }
        let key = pkey_alloc(0, PKEY_DISABLE_WRITE);
        if key >= 0 {
            let _ = KEY.set(key);
        }
    }

    fn process_has_one_thread() -> bool {
        // NB: A directory that cannot be read to its end may list any number of threads.
        std::fs::read_dir("/proc/self/task")
            .and_then(|threads| threads.collect::<Result<Vec<_>, _>>())
            .is_ok_and(|threads| threads.len() == 1)
    }

    /// The memory protection key of chunks whose write access is toggled per thread with the key instead of with
    /// mprotect(), or None if the CPU or kernel lacks protection keys, or the process had threads when this library
    /// loaded. Threads may write the chunks only while they write code; the pages themselves are writable and
    /// executable. One key serves every VM in the process.
    pub fn key() -> Option<libc::c_int> {
        KEY.get().copied()
    }

    /// Allows (or forbids again) the running thread to write memory with the key.
    pub fn set_writable(key: libc::c_int, writable: bool) {
        let rights = if writable { 0 } else { PKEY_DISABLE_WRITE };
        // SAFETY: The key was allocated by allocate_key().
        let result = unsafe { pkey_set(key, rights) };
        assert_eq!(result, 0, "pkey_set() failed");
    }
}

#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn pthread_jit_write_protect_np(enabled: libc::c_int);
    fn sys_icache_invalidate(start: *mut libc::c_void, length: libc::size_t);
}

#[cfg(not(any(target_os = "macos", windows)))]
unsafe extern "C" {
    /// The compiler runtime's instruction cache flush (a no-op on x86-64).
    fn __clear_cache(start: *mut libc::c_char, end: *mut libc::c_char);
}

struct Chunk {
    start: *mut u8,
    size: usize,
    /// Whether the protection key guards writes to the chunk.
    has_protection_key: bool,
}

impl Chunk {
    fn map(size: usize) -> Self {
        #[cfg(target_os = "macos")]
        {
            // SAFETY: An anonymous mapping has no preconditions.
            let chunk = unsafe {
                libc::mmap(
                    core::ptr::null_mut(),
                    size,
                    libc::PROT_READ | libc::PROT_WRITE | libc::PROT_EXEC,
                    libc::MAP_PRIVATE | libc::MAP_ANON | libc::MAP_JIT,
                    -1,
                    0,
                )
            };
            assert!(chunk != libc::MAP_FAILED, "mapping JIT code memory failed");
            Self {
                start: chunk.cast(),
                size,
                has_protection_key: false,
            }
        }
        #[cfg(windows)]
        {
            let chunk = windows_code_memory::reserve(size);
            assert!(!chunk.is_null(), "reserving JIT code memory failed");
            // NB: The range starts at the chunk's first code, whose prologue the unwind info then describes (see
            //     `JitState::entry_trampoline()`).
            let unwind_range = windows_code_memory::CodeRange {
                start: CHUNK_HEADER_SIZE as u32,
                end: u32::try_from(size).expect("a chunk of JIT code is smaller than 4 GiB"),
                frame_layout: windows_code_memory::FrameLayout::RbpAtSavedRbp,
            };
            // SAFETY: The chunk was just reserved, so it has no unwind info yet and no code runs in it.
            let added = unsafe { windows_code_memory::add_unwind_info(chunk, &[unwind_range]) };
            assert!(added, "registering the unwind info of JIT code failed");
            Self {
                start: chunk,
                size,
                has_protection_key: false,
            }
        }
        #[cfg(not(any(target_os = "macos", windows)))]
        {
            // SAFETY: An anonymous mapping has no preconditions.
            let chunk = unsafe {
                libc::mmap(
                    core::ptr::null_mut(),
                    size,
                    libc::PROT_READ | libc::PROT_EXEC,
                    libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
                    -1,
                    0,
                )
            };
            assert!(chunk != libc::MAP_FAILED, "mapping JIT code memory failed");
            #[allow(unused_mut)]
            let mut has_protection_key = false;
            // NB: With a protection key, the pages can be writable without any thread being able to write them, which
            //     makes writing code a per-thread register change instead of a page protection change.
            #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
            if let Some(key) = protection_key::key() {
                // SAFETY: The chunk was just mapped with this size.
                has_protection_key = unsafe {
                    protection_key::pkey_mprotect(
                        chunk,
                        size,
                        libc::PROT_READ | libc::PROT_WRITE | libc::PROT_EXEC,
                        key,
                    )
                } == 0;
            }
            Self {
                start: chunk.cast(),
                size,
                has_protection_key,
            }
        }
    }

    fn contains(&self, address: *const u8) -> bool {
        let start = self.start as usize;
        (start..start + self.size).contains(&(address as usize))
    }
}

impl Drop for Chunk {
    fn drop(&mut self) {
        // SAFETY: The chunk was mapped with this size. Every block of it holds the allocator that owns the chunk, so
        // no code lives in it anymore when it drops.
        #[cfg(not(windows))]
        unsafe {
            libc::munmap(self.start.cast(), self.size);
        }
        // SAFETY: As above.
        #[cfg(windows)]
        unsafe {
            windows_code_memory::delete_unwind_info(self.start);
            windows_code_memory::release(self.start);
        }
    }
}

struct CodeWrite<'a> {
    destination: *mut u8,
    code: &'a [u8],
}

/// Copies each code to its destination, which all lie in one chunk, making the pages writable (and not executable)
/// for the duration of the copies, once for all of them.
fn write_codes_in_chunk(chunk: &Chunk, writes: &[CodeWrite<'_>]) {
    let copy_all = || {
        for write in writes {
            // SAFETY: The destination was allocated for the code in a chunk that is writable during this copy.
            unsafe { core::ptr::copy_nonoverlapping(write.code.as_ptr(), write.destination, write.code.len()) };
        }
    };

    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    if chunk.has_protection_key {
        let key = protection_key::key().expect("a chunk with a protection key has one");
        protection_key::set_writable(key, true);
        copy_all();
        protection_key::set_writable(key, false);
        return;
    }
    let _ = chunk.has_protection_key;

    #[cfg(target_os = "macos")]
    {
        // SAFETY: Toggles the running thread's write protection of MAP_JIT memory.
        unsafe { pthread_jit_write_protect_np(0) };
        copy_all();
        // SAFETY: As above.
        unsafe { pthread_jit_write_protect_np(1) };
        for write in writes {
            // SAFETY: The destination holds the code that was just written.
            unsafe { sys_icache_invalidate(write.destination.cast(), write.code.len()) };
        }
    }
    // NB: Making a page writable commits it, so pages between the writes stay as they are.
    #[cfg(windows)]
    {
        let runs = written_page_runs(writes);
        for &(start, end) in &runs {
            // SAFETY: The pages lie in a reserved chunk, and no code runs while this thread writes it.
            let writable = unsafe { windows_code_memory::make_writable(start as *mut u8, end - start) };
            assert!(writable, "making JIT code writable failed");
        }
        copy_all();
        for &(start, end) in &runs {
            // SAFETY: The pages were committed above.
            let executable = unsafe { windows_code_memory::make_executable(start as *mut u8, end - start) };
            assert!(executable, "making JIT code executable failed");
        }
    }
    #[cfg(not(any(target_os = "macos", windows)))]
    {
        let page_mask = !(page_size() - 1);
        let page_start = writes
            .iter()
            .map(|write| write.destination as usize & page_mask)
            .min()
            .expect("there is a write");
        let page_end = writes
            .iter()
            .map(|write| align_up(write.destination as usize + write.code.len(), page_size()))
            .max()
            .expect("there is a write");
        let pages = page_start as *mut libc::c_void;
        // SAFETY: The pages lie in a mapped chunk.
        let result = unsafe { libc::mprotect(pages, page_end - page_start, libc::PROT_READ | libc::PROT_WRITE) };
        assert_eq!(result, 0, "making JIT code writable failed");
        copy_all();
        // SAFETY: As above.
        let result = unsafe { libc::mprotect(pages, page_end - page_start, libc::PROT_READ | libc::PROT_EXEC) };
        assert_eq!(result, 0, "making JIT code executable failed");
        for write in writes {
            // SAFETY: The destination holds the code that was just written.
            unsafe { __clear_cache(write.destination.cast(), write.destination.add(write.code.len()).cast()) };
        }
    }
}

/// The pages the writes go to, as runs of consecutive pages in the order of their addresses, each from its first page
/// to the end of its last.
#[cfg(windows)]
fn written_page_runs(writes: &[CodeWrite<'_>]) -> Vec<(usize, usize)> {
    let page_mask = !(page_size() - 1);
    let mut pages: Vec<(usize, usize)> = writes
        .iter()
        .map(|write| {
            let start = write.destination as usize;
            (start & page_mask, align_up(start + write.code.len(), page_size()))
        })
        .collect();
    pages.sort_unstable();
    let mut runs: Vec<(usize, usize)> = Vec::with_capacity(pages.len());
    for (start, end) in pages {
        match runs.last_mut() {
            Some(run) if start <= run.1 => run.1 = run.1.max(end),
            _ => runs.push((start, end)),
        }
    }
    runs
}

#[derive(Clone, Copy)]
struct FreeBlock {
    start: *mut u8,
    size: usize,
    /// The index of the chunk the block lies in.
    chunk: usize,
}

impl FreeBlock {
    fn end(&self) -> *mut u8 {
        self.start.wrapping_add(self.size)
    }
}

/// A first-fit allocator over chunks that stay mapped until the allocator goes away. Free blocks are kept sorted by
/// address and coalesced within their chunk. Chunks that the kernel happened to map next to each other may differ in
/// how their writes are protected (see `write_codes_in_chunk()`), so no block, and no allocation, spans two of them.
#[derive(Default)]
pub struct CodeAllocator {
    free_blocks: Vec<FreeBlock>,
    chunks: Vec<Chunk>,
}

impl CodeAllocator {
    fn allocate(&mut self, size: usize) -> *mut u8 {
        let size = align_up(size, ALLOCATION_ALIGNMENT);
        if let Some(index) = self.free_blocks.iter().position(|block| block.size >= size) {
            let block = &mut self.free_blocks[index];
            let address = block.start;
            block.start = block.start.wrapping_add(size);
            block.size -= size;
            if block.size == 0 {
                self.free_blocks.remove(index);
            }
            return address;
        }

        let chunk = Chunk::map(CHUNK_SIZE.max(align_up(CHUNK_HEADER_SIZE + size, page_size())));
        let start = chunk.start.wrapping_add(CHUNK_HEADER_SIZE);
        let code_size = chunk.size - CHUNK_HEADER_SIZE;
        self.chunks.push(chunk);
        if code_size > size {
            self.free(start.wrapping_add(size), code_size - size);
        }
        start
    }

    fn free(&mut self, start: *mut u8, size: usize) {
        let size = align_up(size, ALLOCATION_ALIGNMENT);
        let chunk = self.chunk_index_of(start);
        let index = self.free_blocks.partition_point(|block| block.start < start);
        self.free_blocks.insert(index, FreeBlock { start, size, chunk });
        let adjacent = |blocks: &[FreeBlock], first: usize| {
            blocks[first].end() == blocks[first + 1].start && blocks[first].chunk == blocks[first + 1].chunk
        };
        if index + 1 < self.free_blocks.len() && adjacent(&self.free_blocks, index) {
            self.free_blocks[index].size += self.free_blocks[index + 1].size;
            self.free_blocks.remove(index + 1);
        }
        if index > 0 && adjacent(&self.free_blocks, index - 1) {
            self.free_blocks[index - 1].size += self.free_blocks[index].size;
            self.free_blocks.remove(index);
        }
    }

    fn chunk_index_of(&self, address: *const u8) -> usize {
        self.chunks
            .iter()
            .position(|chunk| chunk.contains(address))
            .expect("JIT code lies in a chunk")
    }
}

/// Installed code: a block of a VM's executable memory, which goes back to the VM's allocator when dropped.
pub struct ExecutableMemory {
    address: *mut u8,
    size: usize,
    allocator: Rc<RefCell<CodeAllocator>>,
}

impl ExecutableMemory {
    /// Copies each code into newly allocated executable memory. Pages that more than one of them go to are made
    /// writable only once. The names describe the codes in /tmp/perf-<pid>.map when `perf_map` is set.
    pub fn allocate(
        allocator: &Rc<RefCell<CodeAllocator>>,
        codes: &[&[u8]],
        names: &[&str],
        perf_map: bool,
    ) -> Vec<ExecutableMemory> {
        assert_eq!(codes.len(), names.len());
        let memories: Vec<ExecutableMemory> = codes
            .iter()
            .map(|code| {
                assert!(!code.is_empty());
                ExecutableMemory {
                    address: allocator.borrow_mut().allocate(code.len()),
                    size: code.len(),
                    allocator: Rc::clone(allocator),
                }
            })
            .collect();

        // NB: Changing page protections is expensive, so code going into the same chunk is written with one change
        //     for all of it.
        let mut writes: Vec<(usize, CodeWrite<'_>)> = memories
            .iter()
            .zip(codes)
            .map(|(memory, code)| {
                let chunk = allocator.borrow().chunk_index_of(memory.address);
                (
                    chunk,
                    CodeWrite {
                        destination: memory.address,
                        code,
                    },
                )
            })
            .collect();
        writes.sort_by_key(|(chunk, write)| (*chunk, write.destination as usize));
        let mut start = 0;
        while start < writes.len() {
            let chunk = writes[start].0;
            let end = start + writes[start..].iter().take_while(|(other, _)| *other == chunk).count();
            let chunk_writes: Vec<CodeWrite<'_>> = writes[start..end]
                .iter()
                .map(|(_, write)| CodeWrite {
                    destination: write.destination,
                    code: write.code,
                })
                .collect();
            write_codes_in_chunk(&allocator.borrow().chunks[chunk], &chunk_writes);
            start = end;
        }

        if perf_map {
            for (memory, name) in memories.iter().zip(names) {
                write_perf_map_entry(memory.address, memory.size, name);
            }
        }
        memories
    }

    pub fn address(&self) -> *const u8 {
        self.address
    }

    /// Overwrites the code at each offset with the bytes that go there, which the code is not running at the time.
    pub fn patch(&self, patches: &[(u32, Vec<u8>)]) {
        if patches.is_empty() {
            return;
        }
        let writes: Vec<CodeWrite<'_>> = patches
            .iter()
            .map(|(offset, bytes)| {
                assert!(*offset as usize + bytes.len() <= self.size);
                CodeWrite {
                    // NB: The offset is within the memory, as asserted.
                    destination: self.address.wrapping_add(*offset as usize),
                    code: bytes,
                }
            })
            .collect();
        let allocator = self.allocator.borrow();
        let chunk = allocator.chunk_index_of(self.address);
        write_codes_in_chunk(&allocator.chunks[chunk], &writes);
    }

    pub fn size(&self) -> usize {
        self.size
    }
}

impl Drop for ExecutableMemory {
    fn drop(&mut self) {
        self.allocator.borrow_mut().free(self.address, self.size);
    }
}

fn write_perf_map_entry(address: *const u8, size: usize, name: &str) {
    let name = if name.is_empty() { "<anonymous>" } else { name };
    if ak::perf_map::write_perf_map_entry(address as usize, size, &format!("JIT {name}")).is_err() {
        eprintln!("LIBJS_JIT: Unable to write /tmp/perf-{}.map", std::process::id());
    }
}
