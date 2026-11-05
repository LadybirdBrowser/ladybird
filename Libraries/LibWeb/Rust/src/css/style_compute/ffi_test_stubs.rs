/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The standalone cargo test binary has no C++ side, so release callbacks are
//! stubbed out here.

use std::cell::Cell;
use std::ffi::c_void;

use crate::css::style::reaction_application::{HostElement, HostStyleReactionApplication};

thread_local! {
    static FONT_CASCADE_LIST_UNREFS: Cell<usize> = const { Cell::new(0) };
}

pub(crate) fn font_cascade_list_unref_count() -> usize {
    FONT_CASCADE_LIST_UNREFS.get()
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_fly_string_unref(_raw: usize) {}
#[unsafe(no_mangle)]
extern "C" fn web_css_font_face_snapshot_reference(_snapshot: *const c_void) {}
#[unsafe(no_mangle)]
extern "C" fn web_css_font_cascade_memo_reference(_memo: *const c_void) {}
#[unsafe(no_mangle)]
extern "C" fn web_css_font_face_snapshot_unreference(_snapshot: *const c_void) {}
#[unsafe(no_mangle)]
extern "C" fn web_css_font_cascade_memo_unreference(_memo: *const c_void) {}
#[unsafe(no_mangle)]
extern "C" fn web_css_resolve_font(
    _memo: *const c_void,
    _snapshot: *const c_void,
    _request: crate::css::style::bridge::FfiFontResolutionRequest,
) -> crate::css::style::bridge::FfiResolvedFont {
    crate::css::style::bridge::FfiResolvedFont::default()
}
#[unsafe(no_mangle)]
extern "C" fn web_css_resolve_font_for_fork(
    _memo: *const c_void,
    _snapshot: *const c_void,
    _request: crate::css::style::bridge::FfiFontResolutionRequest,
) -> crate::css::style::bridge::FfiResolvedFont {
    crate::css::style::bridge::FfiResolvedFont::default()
}
#[unsafe(no_mangle)]
extern "C" fn web_css_style_reaction_element(
    _application: *mut HostStyleReactionApplication,
    _node: u32,
) -> crate::css::style::reaction_application::FfiReactionElement {
    unreachable!("no unit test applies style reactions");
}
#[unsafe(no_mangle)]
extern "C" fn web_css_run_style_update(_application: *mut HostStyleReactionApplication, _flown: bool) {
    unreachable!("no unit test applies style reactions");
}
#[unsafe(no_mangle)]
extern "C" fn web_css_has_pending_style_update(_application: *mut HostStyleReactionApplication) -> bool {
    unreachable!("no unit test applies style reactions");
}
#[unsafe(no_mangle)]
extern "C" fn web_css_note_style_update_has_reactions(_application: *mut HostStyleReactionApplication) {
    unreachable!("no unit test applies style reactions");
}
#[unsafe(no_mangle)]
extern "C" fn web_css_sample_animations_for_style_update(_application: *mut HostStyleReactionApplication) {
    unreachable!("no unit test applies style reactions");
}
#[unsafe(no_mangle)]
extern "C" fn web_css_sample_undisplayed_animations_for_read(
    _application: *mut HostStyleReactionApplication,
    _element: *mut HostElement,
) {
    unreachable!("no unit test applies style reactions");
}
#[unsafe(no_mangle)]
extern "C" fn web_css_begin_style_update_pass(_application: *mut HostStyleReactionApplication, _first: bool) {
    unreachable!("no unit test applies style reactions");
}
#[unsafe(no_mangle)]
extern "C" fn web_css_style_inheritance_parent(
    _application: *mut HostStyleReactionApplication,
    _element: *mut HostElement,
) -> crate::css::style::reaction_application::FfiReactionElement {
    unreachable!("no unit test applies style reactions");
}
#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_string_unref(_raw: usize) {}
#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_string_create_uninitialized(length: usize, has_latin1_storage: bool) -> usize {
    // The unref stub above is a no-op, so this allocation lives until the test process exits.
    let unit_size = if has_latin1_storage { 1 } else { size_of::<u16>() };
    let bytes = size_of::<ak::Utf16StringDataHeader>() + length * unit_size;
    let storage = Box::leak(vec![0_u64; bytes.div_ceil(size_of::<u64>())].into_boxed_slice());
    let header = storage.as_mut_ptr().cast::<ak::Utf16StringDataHeader>();
    unsafe {
        header.write(ak::Utf16StringDataHeader {
            reference_count: std::sync::atomic::AtomicU32::new(1),
            length_in_code_units: u32::try_from(length).unwrap(),
            length_in_code_points: std::sync::atomic::AtomicU32::new(u32::MAX),
            hash: std::sync::atomic::AtomicU32::new(0),
            flags: std::sync::atomic::AtomicU32::new(if has_latin1_storage { 0 } else { ak::HAS_UTF16_STORAGE }),
        });
    }
    header as usize
}
#[unsafe(no_mangle)]
unsafe extern "C" fn ladybird_utf16_fly_string_from_utf8(data: *const u8, length: usize) -> usize {
    let bytes = unsafe { ak::slice_from_raw_parts(data, length) };
    let units = std::str::from_utf8(bytes).unwrap().encode_utf16().collect::<Vec<_>>();
    unsafe { ladybird_utf16_fly_string_from_utf16(units.as_ptr(), units.len()) }
}
#[unsafe(no_mangle)]
unsafe extern "C" fn ladybird_utf16_fly_string_from_utf16(data: *const u16, length: usize) -> usize {
    // Keep a process-wide test atom table with AK's header layout so worker-parsed
    // names remain alive after the worker exits.
    #[repr(C, align(8))]
    struct Header {
        references: std::sync::atomic::AtomicU32,
        length: u32,
        code_points: std::sync::atomic::AtomicU32,
        hash: std::sync::atomic::AtomicU32,
        flags: std::sync::atomic::AtomicU32,
        reserved: u32,
    }
    const _: () = assert!(size_of::<Header>() == 24);
    let units = unsafe { ak::slice_from_raw_parts(data, length) };
    static FLY_STRINGS: std::sync::LazyLock<std::sync::Mutex<std::collections::HashMap<Vec<u16>, Box<[u64]>>>> =
        std::sync::LazyLock::new(|| std::sync::Mutex::new(std::collections::HashMap::new()));
    {
        let mut strings = FLY_STRINGS.lock().unwrap();
        let storage = strings.entry(units.to_vec()).or_insert_with(|| {
            let bytes = size_of::<Header>().checked_add(size_of_val(units)).unwrap();
            let mut storage = vec![0_u64; bytes.div_ceil(size_of::<u64>())].into_boxed_slice();
            unsafe {
                storage.as_mut_ptr().cast::<Header>().write(Header {
                    references: std::sync::atomic::AtomicU32::new(1),
                    length: u32::try_from(length).unwrap(),
                    code_points: std::sync::atomic::AtomicU32::new(u32::MAX),
                    hash: std::sync::atomic::AtomicU32::new(0),
                    flags: std::sync::atomic::AtomicU32::new(1),
                    reserved: 0,
                });
                storage
                    .as_mut_ptr()
                    .cast::<Header>()
                    .add(1)
                    .cast::<u16>()
                    .copy_from_nonoverlapping(units.as_ptr(), length);
            }
            storage
        });
        let raw = storage.as_ptr() as usize;
        // The table retains its own reference for the process's lifetime.
        unsafe { ak::reference_utf16_string(raw) };
        raw
    }
}
#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_cascade_list_ref(_raw: *const std::ffi::c_void) {}
#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_cascade_list_unref(_raw: *const std::ffi::c_void) {
    FONT_CASCADE_LIST_UNREFS.set(FONT_CASCADE_LIST_UNREFS.get() + 1);
}
#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_cascade_list_equals(list: *const c_void, other: *const c_void) -> bool {
    list == other
}
#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_cascade_list_frozen(_raw: *const std::ffi::c_void) -> *const std::ffi::c_void {
    std::ptr::null()
}
#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_ref(_raw: *const std::ffi::c_void) {}
#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_unref(_raw: *const std::ffi::c_void) {}
#[unsafe(no_mangle)]
unsafe extern "C" fn ladybird_gfx_decoded_image_frame_retain(
    frame: *const std::ffi::c_void,
    out_snapshot: *mut libgfx_rust::image_frame::FfiImageFrameSnapshot,
) -> *mut std::ffi::c_void {
    // Test frames are dangling pointers; their address stands in for the id.
    unsafe {
        *out_snapshot = libgfx_rust::image_frame::FfiImageFrameSnapshot {
            id: frame as usize as u64,
            ..Default::default()
        }
    };
    frame.cast_mut()
}
#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_decoded_image_frame_release(_frame: *mut std::ffi::c_void) {}
#[unsafe(no_mangle)]
unsafe extern "C" fn ladybird_gfx_font_snapshot(
    font: *const std::ffi::c_void,
    out_snapshot: *mut libgfx_rust::font::FfiFontSnapshot,
) {
    // Test fonts are dangling pointers; their address stands in for the id.
    unsafe {
        *out_snapshot = libgfx_rust::font::FfiFontSnapshot {
            id: font as usize as u64,
            ..Default::default()
        }
    };
}
#[unsafe(no_mangle)]
unsafe extern "C" fn unicode_rust_idna_to_ascii(
    domain: *const u8,
    domain_length: usize,
    _options: *const c_void,
    context: *mut c_void,
    on_success: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    // The style tests only need ASCII URL hosts. The production build uses LibUnicode's C++
    // implementation for the full IDNA algorithm.
    let domain = unsafe { ak::slice_from_raw_parts(domain, domain_length) };
    if domain.is_ascii() {
        unsafe { on_success(context, domain.as_ptr(), domain.len()) };
    }
}
#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_process_next_structural_epoch() -> u64 {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
}
#[unsafe(no_mangle)]
extern "C" fn unicode_layout_segmenter_destroy(_handle: *mut std::ffi::c_void) {}
