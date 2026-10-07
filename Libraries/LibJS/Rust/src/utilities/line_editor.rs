/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The line editor that the REPL and the debugger prompt of js read their input with. js's C++ main passes in the
//! functions of libedit, so that the runtime, which programs that edit no lines link too, does not depend on it.

use core::cell::Cell;
use core::ffi::{CStr, c_char, c_int};
use std::io::IsTerminal;

/// Completes the line being edited, given as a NUL-terminated string. Returns NULL, or a NULL-terminated, malloc()ed
/// array of malloc()ed strings, the first of which replaces the word before the cursor, as rl_completion_func_t does.
pub type LineCompletionFunction = unsafe extern "C" fn(line: *const c_char) -> *mut *mut c_char;

/// What js's C++ main passes to libjs_rust_js_main(): functions of libedit's <editline/readline.h>, and one
/// that installs a completion function. Utilities/js.cpp declares it field for field. None of the functions
/// is NULL, and each one is called on the thread that called libjs_rust_js_main().
#[repr(C)]
#[derive(Clone, Copy)]
pub struct JSLineEditor {
    /// readline(): shows the prompt when the standard input and output are terminals, and reads a line. Returns the
    /// line without its newline, in memory from malloc() that the caller frees, or NULL at the end of the input.
    pub readline: unsafe extern "C" fn(prompt: *const c_char) -> *mut c_char,
    /// add_history(), which copies the line.
    pub add_history: unsafe extern "C" fn(line: *const c_char) -> c_int,
    pub read_history: unsafe extern "C" fn(path: *const c_char) -> c_int,
    pub write_history: unsafe extern "C" fn(path: *const c_char) -> c_int,
    /// Makes readline() complete the line it edits with the function instead of with file names.
    pub set_line_completion_function: unsafe extern "C" fn(complete_line: LineCompletionFunction),
}

/// What makes the completions of a line, the longest common prefix of which replaces the word that is completed.
pub type LineCompleter = fn(line: &[u8]) -> Vec<Vec<u8>>;

thread_local! {
    static LINE_COMPLETER: Cell<Option<LineCompleter>> = const { Cell::new(None) };
}

impl JSLineEditor {
    /// Shows `prompt` and reads a line, without its newline. Returns None at the end of the input.
    pub fn read_line(&self, prompt: &CStr) -> Option<Vec<u8>> {
        // NB: libedit only prompts when the standard input and output are terminals, and writes the prompt to the
        //     stdout of C stdio, which what the runtime buffers for the standard output has to come out before.
        if std::io::stdin().is_terminal() && std::io::stdout().is_terminal() {
            crate::standard_output::flush();
        }

        // SAFETY: The prompt is NUL-terminated, and readline() returns NULL or a NUL-terminated line from malloc(),
        // which is copied before it is freed.
        unsafe {
            let raw_line = (self.readline)(prompt.as_ptr());
            if raw_line.is_null() {
                return None;
            }
            let line = CStr::from_ptr(raw_line).to_bytes().to_vec();
            libc::free(raw_line.cast());
            Some(line)
        }
    }

    pub fn add_line_to_history(&self, line: &CStr) {
        // SAFETY: The line is NUL-terminated, and add_history() copies it.
        unsafe { (self.add_history)(line.as_ptr()) };
    }

    pub fn read_history_file(&self, path: &CStr) {
        // SAFETY: The path is NUL-terminated.
        unsafe { (self.read_history)(path.as_ptr()) };
    }

    pub fn write_history_file(&self, path: &CStr) {
        // SAFETY: The path is NUL-terminated.
        unsafe { (self.write_history)(path.as_ptr()) };
    }

    /// Makes the line editor complete the line it edits with `line_completer`.
    pub fn complete_lines_with(&self, line_completer: LineCompleter) {
        LINE_COMPLETER.set(Some(line_completer));
        // SAFETY: complete_line_with_line_completer() reads only the NUL-terminated line it is given.
        unsafe { (self.set_line_completion_function)(complete_line_with_line_completer) };
    }
}

unsafe extern "C" fn complete_line_with_line_completer(line: *const c_char) -> *mut *mut c_char {
    let Some(line_completer) = LINE_COMPLETER.get() else {
        return core::ptr::null_mut();
    };
    // SAFETY: The line editor passes the NUL-terminated line it edits, which stays alive until this returns.
    let completions = line_completer(unsafe { CStr::from_ptr(line) }.to_bytes());
    if completions.is_empty() {
        return core::ptr::null_mut();
    }
    completion_matches(common_prefix_of(&completions), &completions)
}

/// The longest run of bytes that every completion starts with, which replaces the word that is completed.
fn common_prefix_of(completions: &[Vec<u8>]) -> &[u8] {
    let mut common_prefix = completions[0].as_slice();
    for completion in &completions[1..] {
        let prefix_length = common_prefix
            .iter()
            .zip(completion)
            .take_while(|(prefix_byte, completion_byte)| prefix_byte == completion_byte)
            .count();
        common_prefix = &common_prefix[..prefix_length];
    }
    common_prefix
}

/// The array that a LineCompletionFunction returns: `common_prefix`, then each of `completions`, as C strings that
/// end at their first NUL like strdup() makes them. Returns NULL if it runs out of memory.
fn completion_matches(common_prefix: &[u8], completions: &[Vec<u8>]) -> *mut *mut c_char {
    // SAFETY: The array has room for every string and the terminating NULL, which calloc() zeroes. On failure,
    // everything allocated so far is freed.
    unsafe {
        let matches = libc::calloc(completions.len() + 2, size_of::<*mut c_char>()).cast::<*mut c_char>();
        if matches.is_null() {
            return core::ptr::null_mut();
        }
        let strings = core::iter::once(common_prefix).chain(completions.iter().map(Vec::as_slice));
        for (index, string) in strings.enumerate() {
            let copy = strndup(string);
            if copy.is_null() {
                for allocated_index in 0..index {
                    libc::free((*matches.add(allocated_index)).cast());
                }
                libc::free(matches.cast());
                return core::ptr::null_mut();
            }
            *matches.add(index) = copy;
        }
        matches
    }
}

/// strndup(): a copy of `string` in memory from malloc(), which ends at the first NUL of the string.
#[cfg(unix)]
fn strndup(string: &[u8]) -> *mut c_char {
    // SAFETY: strndup() reads at most the length of the string.
    unsafe { libc::strndup(string.as_ptr().cast(), string.len()) }
}

/// strndup(), which the C runtime of Windows lacks.
#[cfg(windows)]
fn strndup(string: &[u8]) -> *mut c_char {
    let length = string.iter().position(|&byte| byte == 0).unwrap_or(string.len());
    // SAFETY: The copy has room for the bytes before the first NUL and the NUL that follows them.
    unsafe {
        let copy = libc::malloc(length + 1).cast::<u8>();
        if !copy.is_null() {
            core::ptr::copy_nonoverlapping(string.as_ptr(), copy, length);
            copy.add(length).write(0);
        }
        copy.cast()
    }
}

/// Shows `prompt` and reads a line without libedit, as fgets() into a buffer of 4096 bytes does, with its newline.
/// Returns None at the end of the input.
pub fn read_line_without_line_editor(prompt: &CStr) -> Option<Vec<u8>> {
    use std::io::BufRead;

    crate::standard_output::out(prompt.to_bytes());
    crate::standard_output::flush();

    let mut line = Vec::new();
    let mut standard_input = std::io::stdin().lock();
    while line.len() < 4095 {
        let Ok(buffer) = standard_input.fill_buf() else {
            break;
        };
        let Some(&byte) = buffer.first() else {
            break;
        };
        standard_input.consume(1);
        line.push(byte);
        if byte == b'\n' {
            break;
        }
    }
    if line.is_empty() {
        return None;
    }
    Some(line)
}
