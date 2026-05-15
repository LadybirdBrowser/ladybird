/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod entities;
pub mod interned_names;
pub mod parser;
pub mod token;
pub mod tokenizer;

use std::ptr;
use token::{Attribute, Position, TokenPayload, TokenType};
use tokenizer::{HtmlTokenizer, State};

/// Opaque handle for the Rust tokenizer, passed across the FFI boundary.
pub struct RustFfiTokenizerHandle {
    pub(crate) tokenizer: HtmlTokenizer,
    /// Temporary storage for the last token's string data, kept alive
    /// so that pointers in RustFfiToken remain valid until the next call.
    last_tag_name: Vec<u8>,
    last_comment: Vec<u8>,
    last_doctype_name: Vec<u8>,
    last_public_id: Vec<u8>,
    last_system_id: Vec<u8>,
    last_attributes: Vec<RustFfiAttribute>,
    last_attr_names: Vec<Vec<u8>>,
    last_attr_values: Vec<Vec<u8>>,
    last_unparsed_input: Vec<u8>,
}

/// C-compatible token representation.
///
/// Pointer fields (`tag_name_ptr`, `comment_ptr`, doctype/attribute pointers)
/// borrow into buffers owned by the `RustFfiTokenizerHandle` that produced this
/// token. They remain valid only until the next call into the tokenizer for
/// that handle (any of `next_token`, `insert_input`, `destroy`, etc.).
/// Callers must consume the data before making the next call.
#[repr(C)]
pub struct RustFfiToken {
    pub token_type: u8,
    pub code_point: u32,
    pub self_closing: bool,
    pub had_duplicate_attribute: bool,

    /// If nonzero, an interned tag-name id (1-based index into
    /// `interned_names::INTERNED_TAG_NAMES`). When set, `tag_name_ptr` /
    /// `tag_name_len` are unused and the C++ side uses its parallel
    /// FlyString table directly.
    pub tag_name_id: u16,
    pub tag_name_ptr: *const u8,
    pub tag_name_len: usize,

    pub comment_ptr: *const u8,
    pub comment_len: usize,

    pub doctype_name_ptr: *const u8,
    pub doctype_name_len: usize,
    pub public_id_ptr: *const u8,
    pub public_id_len: usize,
    pub system_id_ptr: *const u8,
    pub system_id_len: usize,
    pub force_quirks: bool,
    pub missing_name: bool,
    pub missing_public_id: bool,
    pub missing_system_id: bool,

    pub attributes_ptr: *const RustFfiAttribute,
    pub attributes_len: usize,

    pub start_line: u64,
    pub start_column: u64,
    pub end_line: u64,
    pub end_column: u64,
}

/// C-compatible attribute representation.
#[repr(C)]
pub struct RustFfiAttribute {
    /// If nonzero, an interned attribute-name id (1-based index into
    /// `interned_names::INTERNED_ATTR_NAMES`). When set, `name_ptr` /
    /// `name_len` are unused.
    pub name_id: u16,
    pub name_ptr: *const u8,
    pub name_len: usize,
    pub value_ptr: *const u8,
    pub value_len: usize,
    pub name_start_line: u64,
    pub name_start_column: u64,
    pub name_end_line: u64,
    pub name_end_column: u64,
    pub value_start_line: u64,
    pub value_start_column: u64,
    pub value_end_line: u64,
    pub value_end_column: u64,
}

impl Default for RustFfiToken {
    fn default() -> Self {
        RustFfiToken {
            token_type: TokenType::Invalid as u8,
            code_point: 0,
            self_closing: false,
            had_duplicate_attribute: false,
            tag_name_id: 0,
            tag_name_ptr: ptr::null(),
            tag_name_len: 0,
            comment_ptr: ptr::null(),
            comment_len: 0,
            doctype_name_ptr: ptr::null(),
            doctype_name_len: 0,
            public_id_ptr: ptr::null(),
            public_id_len: 0,
            system_id_ptr: ptr::null(),
            system_id_len: 0,
            force_quirks: false,
            missing_name: true,
            missing_public_id: true,
            missing_system_id: true,
            attributes_ptr: ptr::null(),
            attributes_len: 0,
            start_line: 0,
            start_column: 0,
            end_line: 0,
            end_column: 0,
        }
    }
}

fn position_to_ffi(pos: &Position) -> (u64, u64) {
    (pos.line, pos.column)
}

/// Create a new Rust HTML tokenizer from UTF-32 code points.
///
/// # Safety
/// `input` must point to `len` valid u32 values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_create(input: *const u32, len: usize) -> *mut RustFfiTokenizerHandle {
    let code_points = if input.is_null() || len == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(input, len) }.to_vec()
    };

    make_handle(HtmlTokenizer::new(code_points))
}

/// Create a new Rust HTML tokenizer directly from a UTF-8 byte buffer.
/// Rust decodes the bytes to code points internally, skipping the C++
/// side's 4x-expanded Vec<u32> copy.
///
/// # Safety
/// `bytes` must point to `len` valid UTF-8 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_create_from_utf8(
    bytes: *const u8,
    len: usize,
) -> *mut RustFfiTokenizerHandle {
    let code_points = if bytes.is_null() || len == 0 {
        Vec::new()
    } else {
        let slice = unsafe { std::slice::from_raw_parts(bytes, len) };
        decode_utf8_to_u32(slice)
    };

    make_handle(HtmlTokenizer::new(code_points))
}

/// Expand a UTF-8 byte slice into a `Vec<u32>` of code points. For the
/// dominant ASCII case we use a tight loop with a single unchecked
/// write per byte and only fall back to `str::chars()` decoding when
/// we see a continuation byte. The caller is responsible for ensuring
/// the input is valid UTF-8.
fn decode_utf8_to_u32(bytes: &[u8]) -> Vec<u32> {
    let mut out: Vec<u32> = Vec::with_capacity(bytes.len());
    // SAFETY: we reserved `bytes.len()` slots and will only write up to
    // that many u32s (one per input byte; multi-byte sequences produce
    // fewer u32s than bytes, so we stay within bounds).
    let out_ptr = out.as_mut_ptr();
    let mut write_idx: usize = 0;
    let mut i: usize = 0;
    let n = bytes.len();
    while i < n {
        let b = bytes[i];
        if b < 0x80 {
            unsafe { std::ptr::write(out_ptr.add(write_idx), b as u32) };
            write_idx += 1;
            i += 1;
        } else {
            // Slow path: decode one code point via str::chars.
            // SAFETY: the input is valid UTF-8 by precondition.
            let tail = unsafe { std::str::from_utf8_unchecked(&bytes[i..]) };
            if let Some(ch) = tail.chars().next() {
                unsafe { std::ptr::write(out_ptr.add(write_idx), ch as u32) };
                write_idx += 1;
                i += ch.len_utf8();
            } else {
                break;
            }
        }
    }
    // SAFETY: we wrote exactly `write_idx` elements, all in-bounds.
    unsafe { out.set_len(write_idx) };
    out
}

fn make_handle(tokenizer: HtmlTokenizer) -> *mut RustFfiTokenizerHandle {
    let handle = Box::new(RustFfiTokenizerHandle {
        tokenizer,
        last_tag_name: Vec::new(),
        last_comment: Vec::new(),
        last_doctype_name: Vec::new(),
        last_public_id: Vec::new(),
        last_system_id: Vec::new(),
        last_attributes: Vec::new(),
        last_attr_names: Vec::new(),
        last_attr_values: Vec::new(),
        last_unparsed_input: Vec::new(),
    });

    Box::into_raw(handle)
}

/// Get the next token from the tokenizer.
/// Returns true if a token was produced, false if no more tokens.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_tokenizer_create`.
/// `out` must be a valid pointer to an RustFfiToken.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_next_token(
    handle: *mut RustFfiTokenizerHandle,
    out: *mut RustFfiToken,
    stop_at_insertion_point: bool,
    cdata_allowed: bool,
) -> bool {
    if handle.is_null() || out.is_null() {
        return false;
    }
    let handle = unsafe { &mut *handle };
    let out = unsafe { &mut *out };

    // Fast-path text character in the Data state. For ASCII text runs
    // this skips the whole state-machine function call plus Token
    // construction/drop. Most real HTML is dominated by such runs.
    if !stop_at_insertion_point && let Some((cp, pos)) = handle.tokenizer.try_fast_data_char() {
        out.token_type = TokenType::Character as u8;
        out.code_point = cp;
        out.start_line = pos.line;
        out.start_column = pos.column;
        out.end_line = pos.line;
        out.end_column = pos.column;
        return true;
    }

    next_token_slow(handle, out, stop_at_insertion_point, cdata_allowed)
}

/// The state-machine and marshalling portion of the FFI token fetch,
/// pulled out of `rust_html_tokenizer_next_token` so that the fast
/// Data-state path in the outer function keeps a tiny stack frame
/// and a short prologue. Marked `#[inline(never)]` so the compiler
/// doesn't re-inline the whole thing and re-inflate the outer frame.
#[inline(never)]
fn next_token_slow(
    handle: &mut RustFfiTokenizerHandle,
    out: &mut RustFfiToken,
    stop_at_insertion_point: bool,
    cdata_allowed: bool,
) -> bool {
    let token = match handle.tokenizer.next_token(stop_at_insertion_point, cdata_allowed) {
        Some(t) => t,
        None => return false,
    };

    // Fast path: Character and EOF tokens only need a few fields.
    // Skip zeroing ~200 bytes of RustFfiToken for 95%+ of all tokens.
    match token.token_type {
        TokenType::Character | TokenType::EndOfFile => {
            out.token_type = token.token_type as u8;
            out.code_point = token.code_point;
            out.start_line = token.start_position.line;
            out.start_column = token.start_position.column;
            out.end_line = token.end_position.line;
            out.end_column = token.end_position.column;
            return true;
        }
        _ => {}
    }

    *out = RustFfiToken::default();
    out.token_type = token.token_type as u8;

    let (sl, sc) = position_to_ffi(&token.start_position);
    let (el, ec) = position_to_ffi(&token.end_position);
    out.start_line = sl;
    out.start_column = sc;
    out.end_line = el;
    out.end_column = ec;

    // Store string data in handle so pointers stay valid.
    match token.payload {
        TokenPayload::Tag {
            tag_name,
            tag_name_id,
            self_closing,
            had_duplicate_attribute,
            attributes,
        } => {
            out.self_closing = self_closing;
            out.had_duplicate_attribute = had_duplicate_attribute;
            // Tokenizer already resolved intern ids, so we trust tag_name_id.
            out.tag_name_id = tag_name_id;
            if tag_name_id == 0 {
                handle.last_tag_name = tag_name.into_bytes();
                out.tag_name_ptr = handle.last_tag_name.as_ptr();
                out.tag_name_len = handle.last_tag_name.len();
            }

            // Convert attributes. Move owned Strings out of each Attribute
            // instead of cloning, then point the FfiAttribute at the stable
            // heap bytes that now live in handle.last_attr_{names,values}
            // (unless the name was interned, in which case skip the copy).
            handle.last_attr_names.clear();
            handle.last_attr_values.clear();
            handle.last_attributes.clear();
            handle.last_attr_names.reserve(attributes.len());
            handle.last_attr_values.reserve(attributes.len());
            handle.last_attributes.reserve(attributes.len());
            for attr in attributes {
                let Attribute {
                    local_name,
                    local_name_id,
                    value,
                    name_start_position,
                    name_end_position,
                    value_start_position,
                    value_end_position,
                } = attr;
                if local_name_id == 0 {
                    handle.last_attr_names.push(local_name.into_bytes());
                } else {
                    // Keep the slot aligned with last_attr_values so index math stays valid.
                    handle.last_attr_names.push(Vec::new());
                }
                handle.last_attr_values.push(value.into_bytes());
                let last_idx = handle.last_attr_names.len() - 1;
                let name_bytes = &handle.last_attr_names[last_idx];
                let value_bytes = &handle.last_attr_values[last_idx];
                handle.last_attributes.push(RustFfiAttribute {
                    name_id: local_name_id,
                    name_ptr: if local_name_id == 0 {
                        name_bytes.as_ptr()
                    } else {
                        ptr::null()
                    },
                    name_len: if local_name_id == 0 { name_bytes.len() } else { 0 },
                    value_ptr: value_bytes.as_ptr(),
                    value_len: value_bytes.len(),
                    name_start_line: name_start_position.line,
                    name_start_column: name_start_position.column,
                    name_end_line: name_end_position.line,
                    name_end_column: name_end_position.column,
                    value_start_line: value_start_position.line,
                    value_start_column: value_start_position.column,
                    value_end_line: value_end_position.line,
                    value_end_column: value_end_position.column,
                });
            }
            out.attributes_ptr = handle.last_attributes.as_ptr();
            out.attributes_len = handle.last_attributes.len();
        }
        TokenPayload::Comment(data) => {
            handle.last_comment = data.into_bytes();
            out.comment_ptr = handle.last_comment.as_ptr();
            out.comment_len = handle.last_comment.len();
        }
        TokenPayload::Doctype(doctype) => {
            handle.last_doctype_name = doctype.name.into_bytes();
            handle.last_public_id = doctype.public_identifier.into_bytes();
            handle.last_system_id = doctype.system_identifier.into_bytes();
            out.doctype_name_ptr = handle.last_doctype_name.as_ptr();
            out.doctype_name_len = handle.last_doctype_name.len();
            out.public_id_ptr = handle.last_public_id.as_ptr();
            out.public_id_len = handle.last_public_id.len();
            out.system_id_ptr = handle.last_system_id.as_ptr();
            out.system_id_len = handle.last_system_id.len();
            out.force_quirks = doctype.force_quirks;
            out.missing_name = doctype.missing_name;
            out.missing_public_id = doctype.missing_public_identifier;
            out.missing_system_id = doctype.missing_system_identifier;
        }
        TokenPayload::None => {}
    }

    true
}

/// Switch the tokenizer to a new state.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_tokenizer_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_switch_state(handle: *mut RustFfiTokenizerHandle, state: u8) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    let Some(state) = State::from_ffi(state) else {
        return;
    };
    handle.tokenizer.switch_to(state);
}

/// Insert input (as UTF-32 code points) at the current insertion point.
///
/// # Safety
/// `handle` must be a valid pointer. `input` must point to `len` valid u32 values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_insert_input(
    handle: *mut RustFfiTokenizerHandle,
    input: *const u32,
    len: usize,
) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    let code_points = if input.is_null() || len == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(input, len) }
    };
    handle.tokenizer.insert_input_at_insertion_point(code_points);
}

/// Append input (as UTF-32 code points) to the tokenizer input stream.
///
/// # Safety
/// `handle` must be a valid pointer. `input` must point to `len` valid u32 values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_append_input(
    handle: *mut RustFfiTokenizerHandle,
    input: *const u32,
    len: usize,
) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    let code_points = if input.is_null() || len == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(input, len) }
    };
    handle.tokenizer.append_input(code_points);
}

/// Get the tokenizer input that has not been consumed yet.
///
/// # Safety
/// `handle`, `out_ptr`, and `out_len` must be valid pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_unparsed_input(
    handle: *mut RustFfiTokenizerHandle,
    out_ptr: *mut *const u8,
    out_len: *mut usize,
) {
    if handle.is_null() || out_ptr.is_null() || out_len.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    handle.last_unparsed_input = handle.tokenizer.unparsed_input().into_bytes();
    unsafe {
        *out_ptr = handle.last_unparsed_input.as_ptr();
        *out_len = handle.last_unparsed_input.len();
    }
}

/// Compact already-tokenized input after the parser has consumed a chunk.
///
/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_parser_did_run(handle: *mut RustFfiTokenizerHandle) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.parser_did_run();
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_store_insertion_point(handle: *mut RustFfiTokenizerHandle) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.store_insertion_point();
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_restore_insertion_point(handle: *mut RustFfiTokenizerHandle) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.restore_insertion_point();
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_update_insertion_point(handle: *mut RustFfiTokenizerHandle) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.update_insertion_point();
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_undefine_insertion_point(handle: *mut RustFfiTokenizerHandle) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.undefine_insertion_point();
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_is_insertion_point_defined(handle: *mut RustFfiTokenizerHandle) -> bool {
    if handle.is_null() {
        return false;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.is_insertion_point_defined()
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_is_insertion_point_reached(handle: *mut RustFfiTokenizerHandle) -> bool {
    if handle.is_null() {
        return false;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.is_insertion_point_reached()
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_set_blocked(handle: *mut RustFfiTokenizerHandle, blocked: bool) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.set_blocked(blocked);
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_is_blocked(handle: *mut RustFfiTokenizerHandle) -> bool {
    if handle.is_null() {
        return false;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.is_blocked()
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_set_input_stream_closed(
    handle: *mut RustFfiTokenizerHandle,
    closed: bool,
) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.set_input_stream_closed(closed);
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_insert_eof(handle: *mut RustFfiTokenizerHandle) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.insert_eof();
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_is_eof_inserted(handle: *mut RustFfiTokenizerHandle) -> bool {
    if handle.is_null() {
        return false;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.is_eof_inserted()
}

/// # Safety
/// `handle` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_abort(handle: *mut RustFfiTokenizerHandle) {
    if handle.is_null() {
        return;
    }
    let handle = unsafe { &mut *handle };
    handle.tokenizer.abort();
}

/// Destroy a Rust HTML tokenizer.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_tokenizer_create`,
/// and must not be used after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_destroy(handle: *mut RustFfiTokenizerHandle) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}

// -- Interned name table enumeration --------------------------------------
//
// The C++ side builds a parallel FlyString array at static-init time by
// enumerating the Rust-owned list once. Ids are 1-based; id 0 is reserved
// for "not interned" in the per-token FFI struct.

/// Number of interned HTML tag names known to the Rust tokenizer.
#[unsafe(no_mangle)]
pub extern "C" fn rust_html_tokenizer_interned_tag_name_count() -> usize {
    interned_names::tag_name_count()
}

/// Number of interned HTML attribute names known to the Rust tokenizer.
#[unsafe(no_mangle)]
pub extern "C" fn rust_html_tokenizer_interned_attr_name_count() -> usize {
    interned_names::attr_name_count()
}

/// Write the bytes and length of the interned tag name with the given
/// 1-based id to the caller-provided out parameters. On unknown ids the
/// out parameters are set to (null, 0).
///
/// # Safety
/// `out_ptr` and `out_len` must be valid pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_interned_tag_name(id: u16, out_ptr: *mut *const u8, out_len: *mut usize) {
    if out_ptr.is_null() || out_len.is_null() {
        return;
    }
    match interned_names::tag_name_by_id(id) {
        Some(bytes) => unsafe {
            *out_ptr = bytes.as_ptr();
            *out_len = bytes.len();
        },
        None => unsafe {
            *out_ptr = ptr::null();
            *out_len = 0;
        },
    }
}

/// Same as `rust_html_tokenizer_interned_tag_name` for attribute names.
///
/// # Safety
/// `out_ptr` and `out_len` must be valid pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_tokenizer_interned_attr_name(id: u16, out_ptr: *mut *const u8, out_len: *mut usize) {
    if out_ptr.is_null() || out_len.is_null() {
        return;
    }
    match interned_names::attr_name_by_id(id) {
        Some(bytes) => unsafe {
            *out_ptr = bytes.as_ptr();
            *out_len = bytes.len();
        },
        None => unsafe {
            *out_ptr = ptr::null();
            *out_len = 0;
        },
    }
}
