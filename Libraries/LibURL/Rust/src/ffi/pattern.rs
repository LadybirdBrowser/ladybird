/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;

use crate::ffi::url::RustFfiUrl;
use crate::ffi::url::RustUrlByteSlice;
use crate::pattern::ComponentResult;
use crate::pattern::GroupMatch;
use crate::pattern::IgnoreCase;
use crate::pattern::Init;
use crate::pattern::Input;
use crate::pattern::MatchInput;
use crate::pattern::Pattern;
use crate::pattern::PatternErrorOr;
use crate::pattern::Result as MatchResult;
use crate::url::Url;

/// Opaque URLPattern handle.
pub struct RustUrlPattern(Pattern);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RustUrlPatternInit {
    pub has_protocol: bool,
    pub protocol: RustUrlByteSlice,
    pub has_username: bool,
    pub username: RustUrlByteSlice,
    pub has_password: bool,
    pub password: RustUrlByteSlice,
    pub has_hostname: bool,
    pub hostname: RustUrlByteSlice,
    pub has_port: bool,
    pub port: RustUrlByteSlice,
    pub has_pathname: bool,
    pub pathname: RustUrlByteSlice,
    pub has_search: bool,
    pub search: RustUrlByteSlice,
    pub has_hash: bool,
    pub hash: RustUrlByteSlice,
    pub has_base_url: bool,
    pub base_url: RustUrlByteSlice,
}

#[repr(u8)]
#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustUrlPatternComponent {
    Protocol,
    Username,
    Password,
    Hostname,
    Port,
    Pathname,
    Search,
    Hash,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RustUrlPatternGroup {
    pub name: RustUrlByteSlice,
    pub has_value: bool,
    pub value: RustUrlByteSlice,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RustUrlPatternComponentResult {
    pub input: RustUrlByteSlice,
    pub groups: *const RustUrlPatternGroup,
    pub group_count: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RustUrlPatternResultInput {
    pub is_string: bool,
    pub string: RustUrlByteSlice,
    pub init: RustUrlPatternInit,
}

#[repr(C)]
pub struct RustUrlPatternExecResult {
    pub inputs: *const RustUrlPatternResultInput,
    pub input_count: usize,
    pub protocol: RustUrlPatternComponentResult,
    pub username: RustUrlPatternComponentResult,
    pub password: RustUrlPatternComponentResult,
    pub hostname: RustUrlPatternComponentResult,
    pub port: RustUrlPatternComponentResult,
    pub pathname: RustUrlPatternComponentResult,
    pub search: RustUrlPatternComponentResult,
    pub hash: RustUrlPatternComponentResult,
}

pub type FfiUrlPatternResultFn = unsafe extern "C" fn(*mut c_void, *const RustUrlPatternExecResult);

fn abort_on_panic<F: FnOnce() -> R, R>(f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(_) => std::process::abort(),
    }
}

fn decode_utf8(slice: RustUrlByteSlice) -> String {
    if slice.data.is_null() {
        return String::new();
    }
    // SAFETY: slice.data is valid for slice.length bytes (contract with C++ caller).
    let bytes = unsafe { std::slice::from_raw_parts(slice.data, slice.length) };
    String::from_utf8_lossy(bytes).into_owned()
}

fn byte_slice(input: &str) -> RustUrlByteSlice {
    RustUrlByteSlice {
        data: input.as_ptr(),
        length: input.len(),
    }
}

fn empty_byte_slice() -> RustUrlByteSlice {
    RustUrlByteSlice {
        data: std::ptr::null(),
        length: 0,
    }
}

fn optional_string_from_raw(ptr: *const u8, len: usize) -> Option<String> {
    if ptr.is_null() {
        return None;
    }

    Some(decode_utf8(RustUrlByteSlice { data: ptr, length: len }))
}

fn optional_string_from_ffi(has_value: bool, slice: RustUrlByteSlice) -> Option<String> {
    if !has_value {
        return None;
    }

    Some(decode_utf8(slice))
}

fn init_from_ffi(ffi: &RustUrlPatternInit) -> Init {
    Init {
        protocol: optional_string_from_ffi(ffi.has_protocol, ffi.protocol),
        username: optional_string_from_ffi(ffi.has_username, ffi.username),
        password: optional_string_from_ffi(ffi.has_password, ffi.password),
        hostname: optional_string_from_ffi(ffi.has_hostname, ffi.hostname),
        port: optional_string_from_ffi(ffi.has_port, ffi.port),
        pathname: optional_string_from_ffi(ffi.has_pathname, ffi.pathname),
        search: optional_string_from_ffi(ffi.has_search, ffi.search),
        hash: optional_string_from_ffi(ffi.has_hash, ffi.hash),
        base_url: optional_string_from_ffi(ffi.has_base_url, ffi.base_url),
    }
}

fn init_to_ffi(init: &Init) -> RustUrlPatternInit {
    fn optional_string_to_ffi(value: Option<&String>) -> (bool, RustUrlByteSlice) {
        value
            .map(|value| (true, byte_slice(value)))
            .unwrap_or((false, empty_byte_slice()))
    }

    let (has_protocol, protocol) = optional_string_to_ffi(init.protocol.as_ref());
    let (has_username, username) = optional_string_to_ffi(init.username.as_ref());
    let (has_password, password) = optional_string_to_ffi(init.password.as_ref());
    let (has_hostname, hostname) = optional_string_to_ffi(init.hostname.as_ref());
    let (has_port, port) = optional_string_to_ffi(init.port.as_ref());
    let (has_pathname, pathname) = optional_string_to_ffi(init.pathname.as_ref());
    let (has_search, search) = optional_string_to_ffi(init.search.as_ref());
    let (has_hash, hash) = optional_string_to_ffi(init.hash.as_ref());
    let (has_base_url, base_url) = optional_string_to_ffi(init.base_url.as_ref());

    RustUrlPatternInit {
        has_protocol,
        protocol,
        has_username,
        username,
        has_password,
        password,
        has_hostname,
        hostname,
        has_port,
        port,
        has_pathname,
        pathname,
        has_search,
        search,
        has_hash,
        hash,
        has_base_url,
        base_url,
    }
}

fn url_from_ffi(ffi: &RustFfiUrl) -> Url {
    super::url::url_from_ffi(ffi)
}

fn component_pattern_string(pattern: &Pattern, component: RustUrlPatternComponent) -> &str {
    match component {
        RustUrlPatternComponent::Protocol => &pattern.protocol_component().pattern_string,
        RustUrlPatternComponent::Username => &pattern.username_component().pattern_string,
        RustUrlPatternComponent::Password => &pattern.password_component().pattern_string,
        RustUrlPatternComponent::Hostname => &pattern.hostname_component().pattern_string,
        RustUrlPatternComponent::Port => &pattern.port_component().pattern_string,
        RustUrlPatternComponent::Pathname => &pattern.pathname_component().pattern_string,
        RustUrlPatternComponent::Search => &pattern.search_component().pattern_string,
        RustUrlPatternComponent::Hash => &pattern.hash_component().pattern_string,
    }
}

#[derive(Default)]
struct ExecResultStorage {
    inputs: Vec<RustUrlPatternResultInput>,
    protocol_groups: Vec<RustUrlPatternGroup>,
    username_groups: Vec<RustUrlPatternGroup>,
    password_groups: Vec<RustUrlPatternGroup>,
    hostname_groups: Vec<RustUrlPatternGroup>,
    port_groups: Vec<RustUrlPatternGroup>,
    pathname_groups: Vec<RustUrlPatternGroup>,
    search_groups: Vec<RustUrlPatternGroup>,
    hash_groups: Vec<RustUrlPatternGroup>,
    ffi_result: Option<RustUrlPatternExecResult>,
}

impl ExecResultStorage {
    fn input_to_ffi(input: &Input) -> RustUrlPatternResultInput {
        match input {
            Input::String(value) => RustUrlPatternResultInput {
                is_string: true,
                string: byte_slice(value),
                init: RustUrlPatternInit {
                    has_protocol: false,
                    protocol: empty_byte_slice(),
                    has_username: false,
                    username: empty_byte_slice(),
                    has_password: false,
                    password: empty_byte_slice(),
                    has_hostname: false,
                    hostname: empty_byte_slice(),
                    has_port: false,
                    port: empty_byte_slice(),
                    has_pathname: false,
                    pathname: empty_byte_slice(),
                    has_search: false,
                    search: empty_byte_slice(),
                    has_hash: false,
                    hash: empty_byte_slice(),
                    has_base_url: false,
                    base_url: empty_byte_slice(),
                },
            },
            Input::Init(value) => RustUrlPatternResultInput {
                is_string: false,
                string: empty_byte_slice(),
                init: init_to_ffi(value),
            },
        }
    }

    fn component_result_to_ffi(
        component_result: &ComponentResult,
        groups: &mut Vec<RustUrlPatternGroup>,
    ) -> RustUrlPatternComponentResult {
        groups.reserve(component_result.groups.len());
        for (name, value) in &component_result.groups {
            let (has_value, value) = match value {
                GroupMatch::String(value) => (true, byte_slice(value)),
                GroupMatch::Empty => (false, empty_byte_slice()),
            };

            groups.push(RustUrlPatternGroup {
                name: byte_slice(name),
                has_value,
                value,
            });
        }

        RustUrlPatternComponentResult {
            input: byte_slice(&component_result.input),
            groups: groups.as_ptr(),
            group_count: groups.len(),
        }
    }

    fn fill_from_result(&mut self, result: &MatchResult) {
        self.inputs.reserve(result.inputs.len());
        for input in &result.inputs {
            self.inputs.push(Self::input_to_ffi(input));
        }

        let protocol = Self::component_result_to_ffi(&result.protocol, &mut self.protocol_groups);
        let username = Self::component_result_to_ffi(&result.username, &mut self.username_groups);
        let password = Self::component_result_to_ffi(&result.password, &mut self.password_groups);
        let hostname = Self::component_result_to_ffi(&result.hostname, &mut self.hostname_groups);
        let port = Self::component_result_to_ffi(&result.port, &mut self.port_groups);
        let pathname = Self::component_result_to_ffi(&result.pathname, &mut self.pathname_groups);
        let search = Self::component_result_to_ffi(&result.search, &mut self.search_groups);
        let hash = Self::component_result_to_ffi(&result.hash, &mut self.hash_groups);

        self.ffi_result = Some(RustUrlPatternExecResult {
            inputs: self.inputs.as_ptr(),
            input_count: self.inputs.len(),
            protocol,
            username,
            password,
            hostname,
            port,
            pathname,
            search,
            hash,
        });
    }
}

fn write_error(error_out: *mut *const u8, error_len_out: *mut usize, message: String) {
    if error_out.is_null() || error_len_out.is_null() {
        return;
    }

    let leaked = message.into_bytes().into_boxed_slice();
    // SAFETY: caller provided valid output pointers.
    unsafe {
        *error_len_out = leaked.len();
        *error_out = Box::into_raw(leaked) as *const u8;
    }
}

/// Compile a URLPattern from a constructor string.
///
/// # Safety
/// `input` must be valid for `input_len` bytes of UTF-8. If `base_url` is non-null,
/// it must be valid for `base_url_len` bytes of UTF-8. `error_out` and
/// `error_len_out` must be valid pointers when non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_create_from_string(
    input: *const u8,
    input_len: usize,
    base_url: *const u8,
    base_url_len: usize,
    ignore_case: IgnoreCase,
    error_out: *mut *const u8,
    error_len_out: *mut usize,
) -> *mut RustUrlPattern {
    abort_on_panic(|| {
        if input.is_null() {
            return std::ptr::null_mut();
        }

        // SAFETY: caller guarantees input is valid for input_len bytes.
        let input_bytes = unsafe { std::slice::from_raw_parts(input, input_len) };
        let Ok(input_str) = std::str::from_utf8(input_bytes) else {
            return std::ptr::null_mut();
        };
        let base_url = optional_string_from_raw(base_url, base_url_len);

        match Pattern::create(&Input::String(input_str.to_string()), &base_url, ignore_case) {
            Ok(pattern) => Box::into_raw(Box::new(RustUrlPattern(pattern))),
            Err(error) => {
                write_error(error_out, error_len_out, error.message);
                std::ptr::null_mut()
            }
        }
    })
}

/// Compile a URLPattern from an init dictionary.
///
/// # Safety
/// `init` must be a valid pointer. `error_out` and `error_len_out` must be
/// valid pointers when non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_create_from_init(
    init: *const RustUrlPatternInit,
    ignore_case: IgnoreCase,
    error_out: *mut *const u8,
    error_len_out: *mut usize,
) -> *mut RustUrlPattern {
    abort_on_panic(|| {
        let Some(init) = (unsafe { init.as_ref() }) else {
            return std::ptr::null_mut();
        };
        let init = init_from_ffi(init);

        match Pattern::create(&Input::Init(init), &None, ignore_case) {
            Ok(pattern) => Box::into_raw(Box::new(RustUrlPattern(pattern))),
            Err(error) => {
                write_error(error_out, error_len_out, error.message);
                std::ptr::null_mut()
            }
        }
    })
}

/// Free an error string returned by a URLPattern create function.
///
/// # Safety
/// `error` must be a pointer returned via a create function's `error_out`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_free_error(error: *mut u8, len: usize) {
    if !error.is_null() {
        // SAFETY: pointer originates from `Box::into_raw` in `write_error`.
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(error, len)) });
    }
}

/// Free a compiled URLPattern.
///
/// # Safety
/// `pattern` must be a valid pointer returned by a create function, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_free(pattern: *mut RustUrlPattern) {
    if !pattern.is_null() {
        // SAFETY: pointer originates from `Box::into_raw` in a create function.
        drop(unsafe { Box::from_raw(pattern) });
    }
}

/// Return whether the pattern contains regexp groups.
///
/// # Safety
/// `pattern` must be a valid pointer returned by a create function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_has_regexp_groups(pattern: *const RustUrlPattern) -> bool {
    if pattern.is_null() {
        return false;
    }

    // SAFETY: caller guarantees pattern is valid.
    let pattern = unsafe { &*pattern };
    pattern.0.has_regexp_groups()
}

/// Return a component's canonical pattern string as a borrowed slice.
///
/// The returned slice is valid until `pattern` is freed.
///
/// # Safety
/// `pattern` must be a valid pointer returned by a create function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_component_pattern_string(
    pattern: *const RustUrlPattern,
    component: RustUrlPatternComponent,
) -> RustUrlByteSlice {
    if pattern.is_null() {
        return empty_byte_slice();
    }

    // SAFETY: caller guarantees pattern is valid.
    let pattern = unsafe { &*pattern };
    let pattern_string = component_pattern_string(&pattern.0, component);
    RustUrlByteSlice {
        data: pattern_string.as_ptr(),
        length: pattern_string.len(),
    }
}

fn exec_match_result(
    result: PatternErrorOr<Option<MatchResult>>,
    error_out: *mut *const u8,
    error_len_out: *mut usize,
    ctx: *mut c_void,
    on_complete: FfiUrlPatternResultFn,
) -> bool {
    match result {
        Ok(Some(result)) => {
            let mut storage = ExecResultStorage::default();
            storage.fill_from_result(&result);
            // SAFETY: callback is provided by caller and `ffi_result` points into `storage`,
            // which remains alive for the duration of the callback.
            unsafe { on_complete(ctx, storage.ffi_result.as_ref().unwrap()) };
            true
        }
        Ok(None) => {
            // SAFETY: callback is provided by caller.
            unsafe { on_complete(ctx, std::ptr::null()) };
            true
        }
        Err(error) => {
            write_error(error_out, error_len_out, error.message);
            false
        }
    }
}

/// Execute a compiled pattern against a string input and return a borrowed view
/// of the match result to a callback.
///
/// # Safety
/// `pattern` must be valid. `input` must be valid for `input_len` bytes of UTF-8.
/// If `base_url` is non-null, it must be valid for `base_url_len` bytes of UTF-8.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_exec_string(
    pattern: *const RustUrlPattern,
    input: *const u8,
    input_len: usize,
    base_url: *const u8,
    base_url_len: usize,
    error_out: *mut *const u8,
    error_len_out: *mut usize,
    ctx: *mut c_void,
    on_complete: FfiUrlPatternResultFn,
) -> bool {
    abort_on_panic(|| {
        if pattern.is_null() || input.is_null() {
            return false;
        }

        // SAFETY: caller guarantees pointers are valid.
        let pattern = unsafe { &*pattern };
        let input_bytes = unsafe { std::slice::from_raw_parts(input, input_len) };
        let Ok(input_str) = std::str::from_utf8(input_bytes) else {
            return false;
        };
        let base_url = optional_string_from_raw(base_url, base_url_len);

        exec_match_result(
            pattern.0.r#match(&MatchInput::String(input_str.to_string()), &base_url),
            error_out,
            error_len_out,
            ctx,
            on_complete,
        )
    })
}

/// Execute a compiled pattern against an init dictionary and return a borrowed
/// view of the match result to a callback.
///
/// # Safety
/// `pattern` and `input` must be valid pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_exec_init(
    pattern: *const RustUrlPattern,
    input: *const RustUrlPatternInit,
    error_out: *mut *const u8,
    error_len_out: *mut usize,
    ctx: *mut c_void,
    on_complete: FfiUrlPatternResultFn,
) -> bool {
    abort_on_panic(|| {
        if pattern.is_null() || input.is_null() {
            return false;
        }

        // SAFETY: caller guarantees pointers are valid.
        let pattern = unsafe { &*pattern };
        let input = init_from_ffi(unsafe { &*input });

        exec_match_result(
            pattern.0.r#match(&MatchInput::Init(input), &None),
            error_out,
            error_len_out,
            ctx,
            on_complete,
        )
    })
}

/// Test a compiled pattern against a string input.
///
/// # Safety
/// `pattern` must be valid. `input` must be valid for `input_len` bytes of UTF-8.
/// If `base_url` is non-null, it must be valid for `base_url_len` bytes of UTF-8.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_test_string(
    pattern: *const RustUrlPattern,
    input: *const u8,
    input_len: usize,
    base_url: *const u8,
    base_url_len: usize,
) -> bool {
    abort_on_panic(|| {
        if pattern.is_null() || input.is_null() {
            return false;
        }

        // SAFETY: caller guarantees pointers are valid.
        let pattern = unsafe { &*pattern };
        let input_bytes = unsafe { std::slice::from_raw_parts(input, input_len) };
        let Ok(input_str) = std::str::from_utf8(input_bytes) else {
            return false;
        };
        let base_url = optional_string_from_raw(base_url, base_url_len);

        matches!(
            pattern.0.r#match(&MatchInput::String(input_str.to_string()), &base_url),
            Ok(Some(_))
        )
    })
}

/// Test a compiled pattern against an init dictionary.
///
/// # Safety
/// `pattern` and `input` must be valid pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_test_init(
    pattern: *const RustUrlPattern,
    input: *const RustUrlPatternInit,
) -> bool {
    abort_on_panic(|| {
        if pattern.is_null() || input.is_null() {
            return false;
        }

        // SAFETY: caller guarantees pointers are valid.
        let pattern = unsafe { &*pattern };
        let input = init_from_ffi(unsafe { &*input });

        matches!(pattern.0.r#match(&MatchInput::Init(input), &None), Ok(Some(_)))
    })
}

/// Test a compiled pattern against a parsed URL.
///
/// # Safety
/// `pattern` and `input` must be valid pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_pattern_test_url(pattern: *const RustUrlPattern, input: *const RustFfiUrl) -> bool {
    abort_on_panic(|| {
        if pattern.is_null() || input.is_null() {
            return false;
        }

        // SAFETY: caller guarantees pointers are valid.
        let pattern = unsafe { &*pattern };
        let input = url_from_ffi(unsafe { &*input });

        matches!(pattern.0.r#match(&MatchInput::Url(input), &None), Ok(Some(_)))
    })
}
