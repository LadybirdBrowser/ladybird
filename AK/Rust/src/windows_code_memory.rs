/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Memory for code generated at run time on Windows x86-64, which the JITs of LibJS and LibWasm share.
//!
//! A chunk of code is reserved as a whole and its pages are committed as code is written to them, so that only pages
//! holding code count against the system's commit limit. Code is never writable and executable at once.
//!
//! Windows walks stacks with unwind tables, not frame pointers, and takes code without a table entry for a leaf
//! function. A walk through generated code would read a return address from the middle of a frame and derail, and the
//! dispatch of an exception is such a walk, which has to get to the base of the thread for the process's unhandled
//! exception filter to run. All generated code keeps a frame pointer, so one entry can describe a whole range of it
//! however many functions the range holds, and the start of the range, where its prologue is. A walk from the prologue
//! of another function of the range takes the frame pointer of the function's caller for the function's, which skips
//! the caller but stays on track if the caller's frame is laid out the same way. Code that native code calls, which
//! keeps no frame pointer on Windows, has to start a range.

use core::ffi::c_void;

/// The page size on Windows x86-64.
pub const PAGE_SIZE: usize = 4096;

/// How much of the start of a chunk `add_unwind_info()` writes to, which code must stay out of.
pub const UNWIND_INFO_SIZE: usize = 64;

/// The most ranges of code a chunk can have.
const MAX_UNWIND_RANGES: usize = 2;

const MEM_COMMIT: u32 = 0x1000;
const MEM_RESERVE: u32 = 0x2000;
const MEM_RELEASE: u32 = 0x8000;
const PAGE_NOACCESS: u32 = 0x01;
const PAGE_READWRITE: u32 = 0x04;
const PAGE_EXECUTE_READ: u32 = 0x20;

const UNWIND_INFO_VERSION: u8 = 1;
const UWOP_PUSH_NONVOL: u8 = 0;
const UWOP_SET_FPREG: u8 = 3;
const RBP: u8 = 5;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn VirtualAlloc(address: *mut c_void, size: usize, allocation_type: u32, protect: u32) -> *mut c_void;
    fn VirtualFree(address: *mut c_void, size: usize, free_type: u32) -> i32;
    fn VirtualProtect(address: *mut c_void, size: usize, protect: u32, old_protect: *mut u32) -> i32;
    fn GetCurrentProcess() -> *mut c_void;
    fn FlushInstructionCache(process: *mut c_void, address: *const c_void, size: usize) -> i32;
    fn RtlAddFunctionTable(table: *const RuntimeFunction, entry_count: u32, base_address: u64) -> u8;
    fn RtlDeleteFunctionTable(table: *const RuntimeFunction) -> u8;
}

/// Reserves address space for a chunk of `size` bytes, or returns null if there is none. Nothing of it can be accessed
/// before `make_writable()`.
pub fn reserve(size: usize) -> *mut u8 {
    // SAFETY: A reservation at an address of the system's choosing has no preconditions.
    unsafe { VirtualAlloc(core::ptr::null_mut(), size, MEM_RESERVE, PAGE_NOACCESS).cast() }
}

/// Gives back the chunk at `base`.
///
/// # Safety
///
/// `base` must come from `reserve()`, and nothing may use the chunk anymore, which has no unwind info registered.
pub unsafe fn release(base: *mut u8) {
    // SAFETY: The caller keeps the contract.
    let result = unsafe { VirtualFree(base.cast(), 0, MEM_RELEASE) };
    assert_ne!(result, 0, "releasing generated code memory failed");
}

/// Makes the pages of a range writable and not executable, committing the ones that were not committed. Returns whether
/// that worked, which only fails when the system is out of commit, and then leaves the pages as they were.
///
/// # Safety
///
/// The range must be inside a chunk from `reserve()`, and no code may run in its pages until `make_executable()`.
pub unsafe fn make_writable(address: *mut u8, size: usize) -> bool {
    // NB: Committing pages that are committed already keeps their contents and sets their protection.
    // SAFETY: The caller keeps the contract.
    !unsafe { VirtualAlloc(address.cast(), size, MEM_COMMIT, PAGE_READWRITE) }.is_null()
}

/// Makes the pages of a range executable and not writable, and has the processor see what was written to them. Returns
/// whether that worked, which only fails when the system is out of kernel memory.
///
/// # Safety
///
/// The range must be inside a chunk from `reserve()`, and all its pages must be committed.
pub unsafe fn make_executable(address: *mut u8, size: usize) -> bool {
    let mut old_protect = 0;
    // SAFETY: The caller keeps the contract.
    unsafe {
        VirtualProtect(address.cast(), size, PAGE_EXECUTE_READ, &raw mut old_protect) != 0
            && FlushInstructionCache(GetCurrentProcess(), address.cast(), size) != 0
    }
}

/// How the code of a range sets up its frames, which is all a stack walk needs to know about it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FrameLayout {
    /// `push rbp; mov rbp, rsp`: rbp points at the saved rbp.
    RbpAtSavedRbp,
    /// `push rbp; lea rbp, [rsp + 16]`: rbp points 8 bytes above the return address.
    RbpAboveReturnAddress,
}

/// A range of the code of a chunk, in bytes from the chunk's base.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CodeRange {
    pub start: u32,
    pub end: u32,
    pub frame_layout: FrameLayout,
}

/// `RUNTIME_FUNCTION`.
#[repr(C)]
#[derive(Clone, Copy)]
struct RuntimeFunction {
    begin_address: u32,
    end_address: u32,
    unwind_info_address: u32,
}

/// `UNWIND_INFO` with its two unwind codes, which come in the reverse order of what they undo.
#[repr(C, align(4))]
#[derive(Clone, Copy)]
struct UnwindInfo {
    version_and_flags: u8,
    size_of_prologue: u8,
    code_count: u8,
    frame_register_and_offset: u8,
    set_frame_register: [u8; 2],
    push_frame_register: [u8; 2],
}

impl UnwindInfo {
    const fn new(frame_layout: FrameLayout) -> Self {
        let (size_of_prologue, offset_in_units_of_16) = match frame_layout {
            // NB: 1 byte for the push, and 3 for the mov or 5 for the lea.
            FrameLayout::RbpAtSavedRbp => (4, 0),
            FrameLayout::RbpAboveReturnAddress => (6, 1),
        };
        Self {
            version_and_flags: UNWIND_INFO_VERSION,
            size_of_prologue,
            code_count: 2,
            frame_register_and_offset: RBP | (offset_in_units_of_16 << 4),
            set_frame_register: [size_of_prologue, UWOP_SET_FPREG],
            push_frame_register: [1, UWOP_PUSH_NONVOL | (RBP << 4)],
        }
    }
}

/// What `add_unwind_info()` writes to the start of a chunk. Entries of the table are in the order of their addresses,
/// and only the entries in use are registered.
#[repr(C)]
struct ChunkUnwindInfo {
    table: [RuntimeFunction; MAX_UNWIND_RANGES],
    rbp_at_saved_rbp: UnwindInfo,
    rbp_above_return_address: UnwindInfo,
}

const _: () = assert!(size_of::<ChunkUnwindInfo>() <= UNWIND_INFO_SIZE);

/// Tells the system how to walk through the ranges of code of the chunk at `base`, and how they are laid out. A range's
/// first bytes are taken for its prologue, so `start` should be where a function's prologue is, or inside the first
/// `UNWIND_INFO_SIZE` bytes of the chunk, where no code runs. Returns whether that worked, which only fails when the
/// system is out of commit or kernel memory.
///
/// # Safety
///
/// `base` must come from `reserve()` and have no unwind info yet, the ranges must be in order and inside the chunk, and
/// no code may run in the chunk's first page while this runs.
pub unsafe fn add_unwind_info(base: *mut u8, ranges: &[CodeRange]) -> bool {
    assert!(!ranges.is_empty() && ranges.len() <= MAX_UNWIND_RANGES);
    assert!(ranges.windows(2).all(|pair| pair[0].end <= pair[1].start));
    let info = base.cast::<ChunkUnwindInfo>();
    let mut table = [RuntimeFunction {
        begin_address: 0,
        end_address: 0,
        unwind_info_address: 0,
    }; MAX_UNWIND_RANGES];
    for (entry, range) in table.iter_mut().zip(ranges) {
        *entry = RuntimeFunction {
            begin_address: range.start,
            end_address: range.end,
            unwind_info_address: match range.frame_layout {
                FrameLayout::RbpAtSavedRbp => core::mem::offset_of!(ChunkUnwindInfo, rbp_at_saved_rbp),
                FrameLayout::RbpAboveReturnAddress => core::mem::offset_of!(ChunkUnwindInfo, rbp_above_return_address),
            } as u32,
        };
    }
    // SAFETY: The caller keeps the contract, and no code goes in the chunk's first bytes.
    unsafe {
        if !make_writable(base, UNWIND_INFO_SIZE) {
            return false;
        }
        info.write(ChunkUnwindInfo {
            table,
            rbp_at_saved_rbp: UnwindInfo::new(FrameLayout::RbpAtSavedRbp),
            rbp_above_return_address: UnwindInfo::new(FrameLayout::RbpAboveReturnAddress),
        });
        make_executable(base, UNWIND_INFO_SIZE)
            && RtlAddFunctionTable(&raw const (*info).table[0], ranges.len() as u32, base as u64) != 0
    }
}

/// Undoes `add_unwind_info()` for the chunk at `base`.
///
/// # Safety
///
/// The chunk must have unwind info, and stay mapped for the duration of the call.
pub unsafe fn delete_unwind_info(base: *mut u8) {
    // SAFETY: The caller keeps the contract.
    let result = unsafe { RtlDeleteFunctionTable(base.cast::<RuntimeFunction>()) };
    assert_ne!(result, 0, "deleting the unwind info of generated code failed");
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `CONTEXT`.
    #[repr(C, align(16))]
    struct Context([u8; 1232]);

    impl Context {
        const RSP_OFFSET: usize = 0x98;
        const RIP_OFFSET: usize = 0xf8;

        fn register(&mut self, offset: usize) -> &mut u64 {
            // SAFETY: The offsets are of 8-byte aligned registers in the structure.
            unsafe { &mut *self.0.as_mut_ptr().add(offset).cast::<u64>() }
        }
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn RtlCaptureContext(context: *mut Context);
        fn RtlLookupFunctionEntry(pc: u64, image_base: *mut u64, history: *mut c_void) -> *const RuntimeFunction;
        fn RtlVirtualUnwind(
            handler_type: u32,
            image_base: u64,
            pc: u64,
            function: *const RuntimeFunction,
            context: *mut Context,
            handler_data: *mut *mut c_void,
            establisher_frame: *mut u64,
            context_pointers: *mut c_void,
        ) -> *mut c_void;
    }

    const CHUNK_SIZE: usize = 1 << 20;
    const CODE_OFFSET: usize = PAGE_SIZE;

    const FRAME_COUNT: usize = 32;

    /// `extern "C" fn(callback, frames)` with a `push rbp; mov rbp, rsp` frame, which calls `callback(frames)` in it.
    const RBP_AT_SAVED_RBP_CODE: &[u8] = &[
        0x55, // push rbp
        0x48, 0x89, 0xe5, // mov rbp, rsp
        0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00, // sub rsp, 256
        0x48, 0x89, 0xc8, // mov rax, rcx
        0x48, 0x89, 0xd1, // mov rcx, rdx
        0xff, 0xd0, // call rax
        0x48, 0x89, 0xec, // mov rsp, rbp
        0x5d, // pop rbp
        0xc3, // ret
    ];

    /// `extern "C" fn(callback, frames)` with a `push rbp; lea rbp, [rsp + 16]` frame, which calls `callback(frames)`
    /// in it.
    const RBP_ABOVE_RETURN_ADDRESS_CODE: &[u8] = &[
        0x55, // push rbp
        0x48, 0x8d, 0x6c, 0x24, 0x10, // lea rbp, [rsp + 16]
        0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00, // sub rsp, 256
        0x48, 0x89, 0xc8, // mov rax, rcx
        0x48, 0x89, 0xd1, // mov rcx, rdx
        0xff, 0xd0, // call rax
        0x48, 0x8d, 0x65, 0xf0, // lea rsp, [rbp - 16]
        0x5d, // pop rbp
        0xc3, // ret
    ];

    fn function_entry_of(pc: usize) -> *const RuntimeFunction {
        let mut image_base = 0;
        // SAFETY: Looking up a function has no preconditions.
        unsafe { RtlLookupFunctionEntry(pc as u64, &raw mut image_base, core::ptr::null_mut()) }
    }

    /// Walks the stack like the dispatch of an exception does, into `frames`.
    extern "C" fn capture_frames(frames: *mut [usize; FRAME_COUNT]) {
        let mut context = Context([0; 1232]);
        // SAFETY: The caller passes frames of its own, and the walk stays on the stack as long as the unwind info is
        //         right.
        unsafe {
            RtlCaptureContext(&raw mut context);
            for frame in (*frames).iter_mut() {
                let pc = *context.register(Context::RIP_OFFSET);
                if pc == 0 {
                    break;
                }
                *frame = pc as usize;
                let mut image_base = 0;
                let function = RtlLookupFunctionEntry(pc, &raw mut image_base, core::ptr::null_mut());
                if function.is_null() {
                    let stack_pointer = *context.register(Context::RSP_OFFSET);
                    *context.register(Context::RIP_OFFSET) = *(stack_pointer as *const u64);
                    *context.register(Context::RSP_OFFSET) = stack_pointer + 8;
                    continue;
                }
                let mut handler_data = core::ptr::null_mut();
                let mut establisher_frame = 0;
                RtlVirtualUnwind(
                    0,
                    image_base,
                    pc,
                    function,
                    &raw mut context,
                    &raw mut handler_data,
                    &raw mut establisher_frame,
                    core::ptr::null_mut(),
                );
            }
        }
    }

    type Callback = extern "C" fn(*mut [usize; FRAME_COUNT]);

    /// The code at `code` calling `capture_frames()`, in a function a walk has to come back through.
    #[inline(never)]
    fn call_generated_code(code: *const u8) -> [usize; FRAME_COUNT] {
        let mut frames = [0; FRAME_COUNT];
        // SAFETY: The code is a function of this type.
        let code: extern "C" fn(Callback, *mut [usize; FRAME_COUNT]) = unsafe { core::mem::transmute(code) };
        code(capture_frames, &raw mut frames);
        // NB: Keeps this from turning into a tail call, which would leave this function out of the walk.
        core::hint::black_box(frames)
    }

    /// Runs `code` in a chunk that describes it with `frame_layout`, and checks that a walk from inside it gets back to
    /// its caller.
    fn check_walk_through(code: &[u8], frame_layout: FrameLayout) {
        let base = reserve(CHUNK_SIZE);
        assert!(!base.is_null());
        // SAFETY: The chunk was just reserved, and nothing runs in it until the code is written and executable.
        unsafe {
            assert!(add_unwind_info(
                base,
                &[CodeRange {
                    start: 0,
                    end: CHUNK_SIZE as u32,
                    frame_layout,
                }],
            ));
            let code_start = base.add(CODE_OFFSET);
            assert!(make_writable(code_start, code.len()));
            core::ptr::copy_nonoverlapping(code.as_ptr(), code_start, code.len());
            assert!(make_executable(code_start, code.len()));

            let frames = call_generated_code(code_start);
            let code_range = code_start as usize..code_start as usize + code.len();
            let index = frames
                .iter()
                .position(|frame| code_range.contains(frame))
                .expect("the walk goes through the generated code");
            let caller = frames[index + 1];
            assert_ne!(caller, 0, "the walk ends at the generated code");
            assert_eq!(
                function_entry_of(caller),
                function_entry_of(call_generated_code as *const () as usize),
                "the walk does not continue in the caller of the generated code"
            );

            delete_unwind_info(base);
            release(base);
        }
    }

    #[test]
    fn walks_through_frames_with_rbp_at_saved_rbp() {
        check_walk_through(RBP_AT_SAVED_RBP_CODE, FrameLayout::RbpAtSavedRbp);
    }

    #[test]
    fn walks_through_frames_with_rbp_above_return_address() {
        check_walk_through(RBP_ABOVE_RETURN_ADDRESS_CODE, FrameLayout::RbpAboveReturnAddress);
    }

    #[test]
    fn keeps_contents_when_making_committed_pages_writable_again() {
        let base = reserve(CHUNK_SIZE);
        assert!(!base.is_null());
        // SAFETY: The chunk was just reserved, and no code runs in it.
        unsafe {
            assert!(make_writable(base, PAGE_SIZE));
            base.write(0x5a);
            assert!(make_executable(base, PAGE_SIZE));
            assert!(make_writable(base, PAGE_SIZE));
            assert_eq!(base.read(), 0x5a);
            base.write(0xa5);
            assert_eq!(base.read(), 0xa5);
            release(base);
        }
    }
}
